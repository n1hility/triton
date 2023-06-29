#include "ScanOpToLLVM.h"
#include "TritonGPUToLLVMBase.h"
#include "triton/Analysis/Utility.h"

using namespace mlir;
using namespace mlir::triton;

using ::mlir::LLVM::delinearize;
using ::mlir::LLVM::linearize;
using ::mlir::LLVM::shflUpSync;
using ::mlir::LLVM::storeShared;

// Apply the region of the scan op to the acc and cur values and update acc
// inplace with the result.
static void accumulate(ConversionPatternRewriter &rewriter, Region &combineOp,
                       Value &acc, Value cur) {
  if (!acc) {
    acc = cur;
    return;
  }
  // Create a new copy of the reduce block, and inline it
  Block *currentBlock = rewriter.getBlock();
  Region &parent = *currentBlock->getParent();
  rewriter.cloneRegionBefore(combineOp, &parent.front());
  auto &newScan = parent.front();
  auto returnOp = dyn_cast<triton::ScanReturnOp>(newScan.getTerminator());
  llvm::SmallVector<Value> combineArgs = {acc, cur};
  rewriter.inlineBlockBefore(&newScan, &*rewriter.getInsertionPoint(),
                             combineArgs);
  auto results = returnOp.getResult();
  acc = results[0];
  // Delete the terminator, which is no longer used
  rewriter.eraseOp(returnOp);
}

// Scan a contiguous elements within a thread and update `srcValues` in place.
static void scanThreadContiguousElements(SmallVector<Value> &srcValues,
                                         ConversionPatternRewriter &rewriter,
                                         ScanLoweringHelper &helper) {
  // TODO: this assumes that axis is the fastest moving dimension. We should
  // relax that.
  unsigned scanElementsPerThreads = helper.getAxisNumElementsPerThreads();
  // Loop through the blocks of contiguous elements.
  for (unsigned j = 0; j < srcValues.size(); j += scanElementsPerThreads) {
    // Reset the accumulator at the beginning of each block of contiguous
    // elements.
    Value acc;
    // Loop through the contiguous elements.
    for (unsigned i = 0; i < scanElementsPerThreads; ++i) {
      accumulate(rewriter, helper.getCombineOp(), acc, srcValues[i + j]);
      srcValues[i + j] = acc;
    }
  }
}

// Apply a scan across threads of the warp for the last element of each
// contiguous group of elements.
static void warpScan(SmallVector<Value> &srcValues,
                     ConversionPatternRewriter &rewriter,
                     ScanLoweringHelper &helper, Value laneId) {
  Location loc = helper.getLoc();
  unsigned scanElementsPerThreads = helper.getAxisNumElementsPerThreads();
  for (unsigned j = scanElementsPerThreads - 1; j < srcValues.size();
       j += scanElementsPerThreads) {
    Value acc = srcValues[j];
    unsigned scanDim = helper.getAxisNumThreadsPerWarp();
    // Reduce within warps.
    for (unsigned i = 1; i <= scanDim / 2; i = i << 1) {
      Value shfl = shflUpSync(loc, rewriter, acc, i);
      Value tempAcc = acc;
      accumulate(rewriter, helper.getCombineOp(), tempAcc, shfl);
      Value mask = icmp_slt(laneId, i32_val(i));
      acc = select(mask, acc, tempAcc);
    }
    srcValues[j] = acc;
  }
}

// For each set of contiguous elements within a thread we store the partial
// reduction into shared memory. Each parallel scan and each warp will store its
// own partial reductions. The shared memory is organized as follow:
//          -----------------------------------------------------------------
// chunk 0: | acc[0] warp 0 | acc[1] warp 0 | acc[0] warp 1 | acc[1] warp 1 |
// chunk 1: | acc[0] warp 0 | acc[1] warp 0 | acc[0] warp 1 | acc[1] warp 1 |
static void storeWarpAccumulator(SmallVector<Value> &srcValues,
                                 ConversionPatternRewriter &rewriter,
                                 ScanLoweringHelper &helper, Value laneId,
                                 Value warpId, Value baseSharedMemPtr,
                                 Value parallelLaneId) {
  Location loc = helper.getLoc();
  unsigned scanElementsPerThreads = helper.getAxisNumElementsPerThreads();
  unsigned scanDim = helper.getAxisNumThreadsPerWarp();
  unsigned numParallelLane = helper.getNonAxisNumThreadsPerCTA();
  unsigned numWarps = helper.getAxisNumWarps();
  unsigned chunkId = 0;
  for (unsigned j = scanElementsPerThreads - 1; j < srcValues.size();
       j += scanElementsPerThreads, ++chunkId) {
    Value lastElement = srcValues[j];
    Value mask = icmp_eq(laneId, i32_val(scanDim - 1));
    Value index = add(parallelLaneId, mul(warpId, i32_val(numParallelLane)));
    index = add(index, i32_val(chunkId * numParallelLane * numWarps));
    Value writePtr = gep(baseSharedMemPtr.getType(), baseSharedMemPtr, index);
    storeShared(rewriter, loc, writePtr, lastElement, mask);
  }
}

// Read the partial reductions from shared memory from each chunk of contiguous
// elements for each warp and parallel scan. Then combine the partial reduction
// with the right elements. Within a given contiguous element chunk we update
// all the elements by accumulating the value from the last element of the
// reduced value from the previous lane.
static void AddPartialReduce(SmallVector<Value> &srcValues,
                             ConversionPatternRewriter &rewriter,
                             ScanLoweringHelper &helper, Value sharedMemoryPtr,
                             Value warpId, Value laneId, Value parallelLaneId) {
  Location loc = helper.getLoc();
  unsigned numParallelLane = helper.getNonAxisNumThreadsPerCTA();
  unsigned numWarps = helper.getAxisNumWarps();
  unsigned scanElementsPerThreads = helper.getAxisNumElementsPerThreads();
  unsigned parallelElementsPerThread = helper.getNonAxisNumElementsPerThread();
  Value maskFirstWarp = icmp_eq(warpId, i32_val(0));
  Value maskFirstLane = icmp_eq(laneId, i32_val(0));
  Value maskFirstThread = and_(maskFirstWarp, maskFirstLane);
  struct Accumulator {
    Value acc;
    Value maskedAcc;
  };
  unsigned numScanBlocks = helper.getAxisNumBlocks();
  unsigned numParallelBlocks = helper.getNonAxisNumBlocks();
  assert(numScanBlocks * numParallelBlocks * parallelElementsPerThread *
             scanElementsPerThreads ==
         srcValues.size());
  SmallVector<Accumulator> accumulators(numParallelBlocks *
                                        parallelElementsPerThread);
  unsigned chunkId = 0;
  for (unsigned parallelBlockId = 0; parallelBlockId < numParallelBlocks;
       ++parallelBlockId) {
    for (unsigned scanBlockId = 0; scanBlockId < numScanBlocks; ++scanBlockId) {
      for (unsigned parallelElementId = 0;
           parallelElementId < parallelElementsPerThread; ++parallelElementId) {
        unsigned accumulatorIndex =
            parallelElementId + parallelBlockId * parallelElementsPerThread;
        Accumulator &accumulator = accumulators[accumulatorIndex];
        for (unsigned i = 0; i < numWarps; ++i) {
          Value index = add(parallelLaneId, i32_val(numParallelLane *
                                                    (i + chunkId * numWarps)));
          Value ptr = gep(sharedMemoryPtr.getType(), sharedMemoryPtr, index);
          Value partialReduce = load(ptr);
          if (!accumulator.acc) {
            accumulator.acc = partialReduce;
            accumulator.maskedAcc = partialReduce;
            continue;
          }
          accumulate(rewriter, helper.getCombineOp(), accumulator.acc,
                     partialReduce);
          Value mask = icmp_slt(warpId, i32_val(i + 1));
          accumulator.maskedAcc =
              select(mask, accumulator.maskedAcc, accumulator.acc);
        }
        unsigned lastElementIndex =
            chunkId * scanElementsPerThreads + scanElementsPerThreads - 1;
        Value temp = srcValues[lastElementIndex];
        accumulate(rewriter, helper.getCombineOp(), temp,
                   accumulator.maskedAcc);
        if (scanBlockId == 0) {
          // For the first warp and first chunk we don't have anything to
          // accumulate.
          temp = select(maskFirstWarp, srcValues[lastElementIndex], temp);
        }
        srcValues[lastElementIndex] = temp;

        // Update the rest of the contiguous elements.
        Value lastElement =
            shflUpSync(loc, rewriter, srcValues[lastElementIndex], 1);
        lastElement = select(maskFirstLane, accumulator.maskedAcc, lastElement);
        for (unsigned i = 1; i < scanElementsPerThreads; ++i) {
          Value laneValue = srcValues[lastElementIndex - i];
          accumulate(rewriter, helper.getCombineOp(), laneValue, lastElement);
          if (scanBlockId == 0) {
            // For the first warp and first chunk we don't have anything to
            // accumulate.
            laneValue = select(maskFirstThread, srcValues[lastElementIndex - i],
                               laneValue);
          }
          srcValues[lastElementIndex - i] = laneValue;
        }
        // For the next chunk start back from the value containing the
        // accumulated value of all the warps.
        accumulator.maskedAcc = accumulator.acc;
        chunkId++;
      }
    }
  }
}

namespace {
struct ScanOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::ScanOp> {
public:
  using ConvertTritonGPUOpToLLVMPattern<
      triton::ScanOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::ScanOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (succeeded(emitFastScan(op, adaptor, rewriter)))
      return success();
    return failure();
  }

private:
  std::tuple<Value, Value, Value>
  getDelinearizedIds(ConversionPatternRewriter &rewriter,
                     ScanLoweringHelper &helper) const;
  LogicalResult emitFastScan(triton::ScanOp op, triton::ScanOpAdaptor adaptor,
                             ConversionPatternRewriter &rewriter) const;
};

// Break up the threadId into lane and warp id along the scan dimension and
// compute a flat id for the parallel dimensions.
std::tuple<Value, Value, Value>
ScanOpConversion::getDelinearizedIds(ConversionPatternRewriter &rewriter,
                                     ScanLoweringHelper &helper) const {
  auto loc = helper.getLoc();
  unsigned axis = helper.getAxis();
  auto srcEncoding = helper.getEncoding();

  Value threadId = getThreadId(rewriter, loc);
  Value warpSize = i32_val(32);
  Value warpId = udiv(threadId, warpSize);
  Value laneId = urem(threadId, warpSize);

  auto threadsPerWarp = triton::gpu::getThreadsPerWarp(srcEncoding);
  auto warpsPerCTA = triton::gpu::getWarpsPerCTA(srcEncoding);
  auto order = triton::gpu::getOrder(srcEncoding);
  SmallVector<Value> multiDimLaneId =
      delinearize(rewriter, loc, laneId, threadsPerWarp, order);
  SmallVector<Value> multiDimWarpId =
      delinearize(rewriter, loc, warpId, warpsPerCTA, order);

  Value laneIdAxis = multiDimLaneId[axis];
  Value warpIdAxis = multiDimWarpId[axis];

  multiDimLaneId[axis] = i32_val(0);
  threadsPerWarp[axis] = 1;
  Value laneIdParallel =
      linearize(rewriter, loc, multiDimLaneId, threadsPerWarp, order);
  multiDimWarpId[axis] = i32_val(0);
  warpsPerCTA[axis] = 1;
  Value warpIdParallel =
      linearize(rewriter, loc, multiDimWarpId, warpsPerCTA, order);
  Value flatIdParallel =
      add(laneIdParallel,
          mul(warpIdParallel, i32_val(helper.getNonAxisNumThreadsPerWarp())));
  return std::make_tuple(laneIdAxis, warpIdAxis, flatIdParallel);
}

// Lowering using warp shuffle operations to do warp level scan.
LogicalResult
ScanOpConversion::emitFastScan(triton::ScanOp op, triton::ScanOpAdaptor adaptor,
                               ConversionPatternRewriter &rewriter) const {
  ScanLoweringHelper helper(op);
  auto loc = helper.getLoc();
  if (!helper.isSupported())
    return failure();

  auto [laneIdAxis, warpIdAxis, flatIdParallel] =
      getDelinearizedIds(rewriter, helper);
  auto input = adaptor.getOperands()[0];
  auto type = op.getOperand(0).getType().cast<RankedTensorType>();
  SmallVector<Value> srcValues =
      getTypeConverter()->unpackLLElements(loc, input, rewriter, type);

  // Scan contigous elements in a thread and update `srcValues`.
  scanThreadContiguousElements(srcValues, rewriter, helper);
  // Apply warp level scan to the last element of each chunk of contiguous
  // elements.
  warpScan(srcValues, rewriter, helper, laneIdAxis);

  // Store the partial reducing for each warp into shared memory.
  Type elemPtrTys = LLVM::LLVMPointerType::get(srcValues[0].getType(), 3);
  Value baseSharedMemPtr = bitcast(
      getSharedMemoryBase(loc, rewriter, op.getOperation()), elemPtrTys);
  storeWarpAccumulator(srcValues, rewriter, helper, laneIdAxis, warpIdAxis,
                       baseSharedMemPtr, flatIdParallel);
  barrier();
  // Read back the partial reduction of each warp and accumulate them based on
  // warpId. Then update each chunk of contiguous elements by adding the
  // accumulated value from the previous lane.
  AddPartialReduce(srcValues, rewriter, helper, baseSharedMemPtr, warpIdAxis,
                   laneIdAxis, flatIdParallel);

  Value results = getTypeConverter()->packLLElements(loc, srcValues, rewriter,
                                                     input.getType());
  rewriter.replaceOp(op, results);
  return success();
}
} // namespace

void populateScanOpToLLVMPatterns(
    TritonGPUToLLVMTypeConverter &typeConverter, RewritePatternSet &patterns,
    ModuleAllocation &allocation,
    ConvertTritonGPUOpToLLVMPatternBase::IndexCacheInfo &indexCacheInfo,
    PatternBenefit benefit) {
  patterns.add<ScanOpConversion>(typeConverter, allocation, indexCacheInfo,
                                 benefit);
}
