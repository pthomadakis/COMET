//===- ReduceOpLowerToSCF.cpp - Lowering reduction operation to SCF ------===//
//
// Copyright 2022 Battelle Memorial Institute
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions
// and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
// and the following disclaimer in the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//
//
// This file implements a lowering of reduction operation for sparse and dense tensors to SCF
//===----------------------------------------------------------------------===//

#include "comet/Dialect/TensorAlgebra/IR/TADialect.h"
#include "comet/Dialect/TensorAlgebra/IR/TATypes.h"
#include "comet/Dialect/TensorAlgebra/Passes.h"
#include "comet/Dialect/Utils/Utils.h"

#include "mlir/Dialect/Linalg/EDSC/Intrinsics.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgTypes.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/StandardOps/EDSC/Intrinsics.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/Sequence.h"
#include "mlir/EDSC/Builders.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/Types.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"

#include "llvm/Support/Debug.h"
#include <iostream>
#include <algorithm>
#include <vector>

#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <string>

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace mlir::linalg;
using namespace mlir::tensorAlgebra;

#define DEBUG_TYPE "lowering-reduceop-to-scf"

// *********** For debug purpose *********//
// #ifndef DEBUG_MODE_ReduceOpLowerToSCFPass
// #define DEBUG_MODE_ReduceOpLowerToSCFPass
// #endif

#ifdef DEBUG_MODE_ReduceOpLowerToSCFPass
#define comet_debug() llvm::errs() << __FILE__ << " " << __LINE__ << " "
#define comet_pdump(n)                                \
  llvm::errs() << __FILE__ << " " << __LINE__ << " "; \
  n->dump()
#define comet_vdump(n)                                \
  llvm::errs() << __FILE__ << " " << __LINE__ << " "; \
  n.dump()
#else
#define comet_debug() llvm::nulls()
#define comet_pdump(n)
#define comet_vdump(n)
#endif
// *********** For debug purpose *********//

//===----------------------------------------------------------------------===//
// ReduceOpLowerToSCF PASS
//===----------------------------------------------------------------------===//

namespace
{
  //===----------------------------------------------------------------------===//
  // ReduceOptoSCF RewritePatterns: Reduction operation lowering for sparse and dense tensors
  //===----------------------------------------------------------------------===//

  struct ReduceOpLowering : public OpRewritePattern<tensorAlgebra::ReduceOp>
  {
    using OpRewritePattern<tensorAlgebra::ReduceOp>::OpRewritePattern;
    LogicalResult matchAndRewrite(tensorAlgebra::ReduceOp op,
                                  PatternRewriter &rewriter) const final
    {

      assert(isa<tensorAlgebra::ReduceOp>(op));
      comet_debug() << "Lowering Reduce operation to SCF\n";

      Location loc = op.getLoc();
      auto f64Type = rewriter.getF64Type();
      auto inputType = op->getOperand(0).getType();

      // Allocate memory for the result and initialized it
      auto cst_zero = rewriter.create<ConstantIndexOp>(loc, 0); // need to access res alloc
      MemRefType memTy_alloc_res = MemRefType::get({1}, f64Type);
      Value res = rewriter.create<memref::AllocOp>(loc, memTy_alloc_res);
      Value const_f64_0 = rewriter.create<mlir::ConstantOp>(loc, f64Type, rewriter.getF64FloatAttr(0));
      std::vector<Value> alloc_zero_loc = {cst_zero};
      rewriter.create<memref::StoreOp>(loc, const_f64_0,
                                       res, alloc_zero_loc);

      comet_vdump(res);

      if (inputType.isa<TensorType>())
      { // tensor is dense
        comet_debug() << "Input Tensor is dense\n";
        std::vector<Value> indices;
        auto alloc_op = op->getOperand(0).getDefiningOp()->getOperand(0);

        comet_vdump(alloc_op);

        for (unsigned rank = 0; rank < inputType.cast<mlir::TensorType>().getRank(); rank++)
        {
          auto dimSize = inputType.cast<mlir::TensorType>().getDimSize(rank);
          Value upperBound;
          if (dimSize == ShapedType::kDynamicSize)
          {
            comet_debug() << " This dimension is a dynamic size\n";

            comet_vdump(alloc_op);
            auto memRefType = alloc_op.getType().dyn_cast<MemRefType>();
            unsigned dynamicDimPos = memRefType.getDynamicDimIndex(rank);
            comet_debug() << " dynamicDimPos: " << dynamicDimPos << "\n";
            upperBound = alloc_op.getDefiningOp()->getOperand(dynamicDimPos);
          }
          else
          {
            upperBound = rewriter.create<ConstantIndexOp>(loc, dimSize);
          }
          auto lowerBound = rewriter.create<ConstantIndexOp>(loc, 0);
          auto step = rewriter.create<ConstantIndexOp>(loc, 1);
          // create for loops
          auto loop = rewriter.create<scf::ForOp>(loc, lowerBound, upperBound, step);
          indices.push_back(loop.getInductionVar());
          rewriter.setInsertionPointToStart(loop.getBody());
        }

        // Build loop body
        auto load_rhs = rewriter.create<memref::LoadOp>(loc, alloc_op, indices);
        auto res_load = rewriter.create<memref::LoadOp>(loc, res, alloc_zero_loc);
        auto reduced = rewriter.create<mlir::AddFOp>(loc, load_rhs, res_load);
        rewriter.create<memref::StoreOp>(loc, reduced, res, alloc_zero_loc);
      }
      else
      { // sparse tensor type
        assert(inputType.isa<SparseTensorType>());
        comet_debug() << __FILE__ << __LINE__ << "Input Tensor is sparse\n";

        comet_pdump(op);
        assert(isa<tensorAlgebra::SparseTensorConstructOp>(op->getOperand(0).getDefiningOp()));

        //TODO(gkestor): get tensor ranks by functions
        int tensorRanks = (op->getOperand(0).getDefiningOp()->getNumOperands() - 2) / 5;
        comet_debug() << " tensorRank: " << tensorRanks << " \n";
        comet_debug() << "Tensor to reduce:\n";
        comet_pdump(op->getOperand(0).getDefiningOp());

        // TODO(gkestor): need a better way to acces information from SparseTensorConstructOp
        //  create the lowerBound, upperbound and step for loop
        int indexValueSize = (tensorRanks * 4) + 1; // 4 corresponding to pos, crd, crd_size, pos_size
        comet_debug() << "indexValueSize in SparseTensorConstructOp:" << indexValueSize << "\n";

        auto loadOpForNNZ = op->getOperand(0).getDefiningOp()->getOperand(indexValueSize);
        comet_debug() << "Corresponding AllocOp from SparseTensorConstructOp:\n";
        comet_vdump(loadOpForNNZ);
        auto memAllocForNNZ = loadOpForNNZ.getDefiningOp()->getOperand(0);
        comet_debug() << "Corresponding MemAllocOp for NNZ:\n";
        comet_vdump(memAllocForNNZ);

        MemRefType resultMemTy = memAllocForNNZ.getDefiningOp()->getResult(0).getType().cast<MemRefType>();
        auto memRefRank = resultMemTy.getRank();
        comet_debug() << "memRefRank for alloc: " << memRefRank << "\n";
        assert(memRefRank ==  1); //Memref rank should be 1

        auto memRefDimSize  =  resultMemTy.getDimSize(memRefRank-1);
        comet_debug() << "memRefDimSize for alloc: " << memRefDimSize << "\n";

        Value upperBound;
        if (memRefDimSize == 1) //size of value array comes from temporary sparse tensor and Dimsize of alloc is one
        {
          upperBound = rewriter.create<memref::LoadOp>(loc, memAllocForNNZ, alloc_zero_loc);
        }
        else
        {
          //size of value array comes from read_input_sizes_2D_f64, and alloc dimsize can be only expected size
          auto expectedMemRefSize = tensorRanks * 2 + tensorRanks + 1; //"2" is corresponding the pos, crd per dimension, "1" is for value array 
          assert(memRefDimSize == expectedMemRefSize); 
          upperBound = op->getOperand(0).getDefiningOp()->getOperand(indexValueSize);
        }
        comet_debug() << "Upper Bound:\n";
        comet_vdump(upperBound);
        auto lowerBound = rewriter.create<ConstantIndexOp>(loc, 0);
        auto step = rewriter.create<ConstantIndexOp>(loc, 1);

        // create for loops
        auto loop = rewriter.create<scf::ForOp>(loc, lowerBound, upperBound, step);

        auto insertPt = rewriter.saveInsertionPoint();
        rewriter.setInsertionPointToStart(loop.getBody());

        // Build loop body
        int indexValuePtr = (tensorRanks * 2); // 2 corresponding to pos, crd
        auto alloc_op = op->getOperand(0).getDefiningOp()->getOperand(indexValuePtr).getDefiningOp()->getOperand(0);
        comet_debug() << " ValueAllocOp";
        comet_vdump(alloc_op);
        std::vector<Value> indices = {loop.getInductionVar()};
        auto load_rhs = rewriter.create<memref::LoadOp>(loc, alloc_op, indices);
        auto res_load = rewriter.create<memref::LoadOp>(loc, res, alloc_zero_loc);
        auto reduce = rewriter.create<mlir::AddFOp>(loc, load_rhs, res_load);
        rewriter.create<memref::StoreOp>(loc, reduce, res, alloc_zero_loc);

        // need to restore the insertion point to the previous point
        rewriter.restoreInsertionPoint(insertPt);
        comet_vdump(loop);
      }

      // Important to replace all uses of this operation with the new one, otherwise, the current op won't be lowered.
      op.replaceAllUsesWith(res);
      rewriter.eraseOp(op);

      return success();
    }
  }; // ReduceOpLowering

  struct ReduceOpLowerToSCFPass
      : public PassWrapper<ReduceOpLowerToSCFPass, FunctionPass>
  {
    void runOnFunction() final;
  };

} // end anonymous namespace.

void ReduceOpLowerToSCFPass::runOnFunction()
{
  LLVM_DEBUG(llvm::dbgs() << "start ReduceOpLowerToSCFPass\n");

  ConversionTarget target(getContext());
  target.addLegalDialect<LinalgDialect, StandardOpsDialect, scf::SCFDialect, AffineDialect, memref::MemRefDialect>();
  OwningRewritePatternList patterns(&getContext());
  patterns.insert<ReduceOpLowering>(&getContext());

  if (failed(applyPartialConversion(getFunction(), target, std::move(patterns))))
  {
    llvm::errs() << "Failed to Lower Reduce operation\n";
    signalPassFailure();
  }
}

// Lower sparse tensor algebra operation to loops
std::unique_ptr<Pass> mlir::tensorAlgebra::createReduceOpLowerToSCFPass()
{
  return std::make_unique<ReduceOpLowerToSCFPass>();
}