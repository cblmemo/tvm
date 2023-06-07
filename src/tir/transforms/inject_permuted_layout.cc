/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file inject_permuted_layout.cc
 * \brief The pass for inject permuted layout.
 */

#include <tvm/arith/analyzer.h>
#include <tvm/tir/function.h>
#include <tvm/tir/op.h>
#include <tvm/tir/stmt_functor.h>
#include <tvm/tir/transform.h>

#include "../../support/utils.h"
#include "../ir/functor_common.h"
#include "ir_utils.h"

namespace tvm {
namespace tir {

using tir::Block;
using tir::BlockRealize;
using tir::Call;
using tir::For;

class PermutedLayoutInjector : public StmtExprMutator {
 public:
  explicit PermutedLayoutInjector() {}

 private:
  Array<PrimExpr> GetNewIndices(PrimExpr s0, PrimExpr s1, int smem_width) {
    if (smem_width % 64 == 0) {
      return {s0, ((floordiv(floordiv(s1, 8), 8) * 8 +
                    (floormod(floordiv(s1, 8), 8) ^ floormod(s0, 8))) *
                       8 +
                   floormod(s1, 8))};
    } else {
      return {s0,
              (floormod(floordiv(s1, 8), 4) ^ floordiv(floormod(s0, 8), 2)) * 8 + floormod(s1, 8)};
    }
  }

  Stmt VisitStmt_(const BlockRealizeNode* _op) final {
    BlockRealize br = Downcast<BlockRealize>(StmtExprMutator::VisitStmt_(_op));
    BlockRealizeNode* op = br.CopyOnWrite();
    if (op->block->annotations.count("permuted_layout") == 0) {
      return br;
    }
    String val = Downcast<String>(op->block->annotations.at("permuted_layout"));
    if (val.empty()) return br;
    Block blk = op->block;
    Stmt body = blk->body;
    if (support::StartsWith(val, "g2s")) {
      bool have_local_stage = (body.as<SeqStmtNode>() != nullptr);
      Stmt upper_loop;
      if (have_local_stage) {
        SeqStmt seq = Downcast<SeqStmt>(body);
        ICHECK(seq->size() == 2);
        upper_loop = seq->seq[0];
        body = seq->seq[1];
      }
      std::vector<const ForNode*> loops;
      while (const ForNode* loop = body.as<ForNode>()) {
        loops.push_back(loop);
        body = loop->body;
      }
      const BufferStoreNode* store = body.as<BufferStoreNode>();
      ICHECK(store);
      auto smem_width = store->buffer->shape[1].as<IntImmNode>()->value;
      if (smem_width % 32 != 0) {
        LOG(INFO) << "Permuted Layout for " << op->block->name_hint
                  << " is not supported since its second dimension is not divisible by 32";
        return br;
      }
      if (smem_width % 64 == 32) {
        if (store->buffer->shape[0].as<IntImmNode>()->value % 2 != 0) {
          LOG(INFO) << "Permuted Layout for " << op->block->name_hint
                    << " is not supported since its first dimension is not divisible by 2"
                    << " and second dimension is not divisible by 64";
          return br;
        }
      }
      if (val.at(4) == 'A') {
        smem_width_A_ = smem_width;
      } else {
        smem_width_B_ = smem_width;
      }
      PrimExpr s0 = store->indices[0];
      PrimExpr s1 = store->indices[1];
      Array<PrimExpr> new_indices = GetNewIndices(s0, s1, smem_width);
      Stmt new_body = BufferStore(store->buffer, store->value, new_indices);
      for (int i = loops.size() - 1; i >= 0; i--) {
        const ForNode* loop = loops[i];
        new_body = For(loop->loop_var, loop->min, loop->extent, loop->kind, new_body,
                       loop->thread_binding, loop->annotations);
      }
      if (have_local_stage) {
        new_body = SeqStmt({upper_loop, new_body});
      }
      Block new_blk = Block(blk->iter_vars, blk->reads, blk->writes, blk->name_hint, new_body,
                            blk->init, blk->alloc_buffers, blk->match_buffers, blk->annotations);
      BlockRealize new_br = BlockRealize(op->iter_values, op->predicate, new_blk);
      return new_br;
    } else if (support::StartsWith(val, "s2l")) {
      int smem_width = val.at(4) == 'A' ? smem_width_A_ : smem_width_B_;
      if (smem_width == -1) {
        return br;
      }

      Evaluate eval = Downcast<Evaluate>(body);
      Call ldmat_call = Downcast<Call>(eval->value);
      ICHECK(ldmat_call->args.size() == 7);
      Array<PrimExpr> new_ldmat_args;
      for (int i = 0; i < 5; i++) {
        new_ldmat_args.push_back(ldmat_call->args[i]);
      }
      Call accptr_call = Downcast<Call>(ldmat_call->args[5]);
      PrimExpr smem_offset = ldmat_call->args[6];

      Array<PrimExpr> new_accptr_args;
      for (int i = 0; i < 5; i++) {
        new_accptr_args.push_back(i == 2 ? 0 : accptr_call->args[i]);
      }
      Call new_accptr_call = Call(accptr_call->dtype, accptr_call->op, new_accptr_args);
      new_ldmat_args.push_back(new_accptr_call);

      PrimExpr accptr_offset = accptr_call->args[2];
      PrimExpr offset = smem_offset + accptr_offset;
      PrimExpr s0 = floordiv(offset, smem_width), s1 = floormod(offset, smem_width);
      Array<PrimExpr> new_indices = GetNewIndices(s0, s1, smem_width);
      PrimExpr new_offset = new_indices[0] * smem_width + new_indices[1];
      new_ldmat_args.push_back(new_offset);
      Call new_ldmat_call = Call(ldmat_call->dtype, ldmat_call->op, new_ldmat_args);
      Stmt new_body = Evaluate(new_ldmat_call);
      Block new_blk = Block(blk->iter_vars, blk->reads, blk->writes, blk->name_hint, new_body,
                            blk->init, blk->alloc_buffers, blk->match_buffers, blk->annotations);
      BlockRealize new_br = BlockRealize(op->iter_values, op->predicate, new_blk);
      return new_br;
    }

    return StmtExprMutator::VisitStmt_(op);
  }

  int smem_width_A_ = -1;
  int smem_width_B_ = -1;
};

PrimFunc InjectPermutedLayout(PrimFunc func) {
  auto fptr = func.CopyOnWrite();
  fptr->body = PermutedLayoutInjector()(std::move(fptr->body));
  return func;
}

namespace transform {

Pass InjectPermutedLayout() {
  auto pass_func = [=](PrimFunc f, IRModule m, PassContext ctx) {
    return InjectPermutedLayout(std::move(f));
  };
  return CreatePrimFuncPass(pass_func, 0, "tir.InjectPermutedLayout", {});
}

TVM_REGISTER_GLOBAL("tir.transform.InjectPermutedLayout").set_body_typed(InjectPermutedLayout);

}  // namespace transform

}  // namespace tir
}  // namespace tvm
