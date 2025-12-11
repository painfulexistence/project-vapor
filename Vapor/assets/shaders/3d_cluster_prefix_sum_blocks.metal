#include <metal_stdlib>
using namespace metal;

// Pass 2a: Block-level exclusive prefix sum using Hillis-Steele algorithm
// Each threadgroup processes BLOCK_SIZE elements

struct ClusterCompact {
    float4 min;
    float4 max;
    uint offset;
    uint lightCount;
};

constant uint BLOCK_SIZE = 256;

kernel void computeMain(
    device ClusterCompact* clusters [[buffer(0)]],
    device uint* blockSums [[buffer(1)]],
    constant uint& numClusters [[buffer(2)]],
    uint tid [[thread_index_in_threadgroup]],
    uint gid [[threadgroup_position_in_grid]],
    uint blockDim [[threads_per_threadgroup]]
) {
    threadgroup uint sharedData[BLOCK_SIZE];

    uint globalIdx = gid * BLOCK_SIZE + tid;

    // Load data into shared memory
    sharedData[tid] = (globalIdx < numClusters) ? clusters[globalIdx].lightCount : 0;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Hillis-Steele inclusive scan
    for (uint stride = 1; stride < BLOCK_SIZE; stride *= 2) {
        uint val = 0;
        if (tid >= stride) {
            val = sharedData[tid - stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        sharedData[tid] += val;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    // Convert to exclusive scan and write back
    uint exclusiveSum = (tid > 0) ? sharedData[tid - 1] : 0;
    if (globalIdx < numClusters) {
        clusters[globalIdx].offset = exclusiveSum;
    }

    // Last thread in block writes block sum
    if (tid == BLOCK_SIZE - 1) {
        blockSums[gid] = sharedData[tid];
    }
}
