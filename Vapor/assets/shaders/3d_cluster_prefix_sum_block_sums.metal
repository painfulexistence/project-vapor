#include <metal_stdlib>
using namespace metal;

// Pass 2b: Scan block sums (small array, single threadgroup is enough)

kernel void computeMain(
    device uint* blockSums [[buffer(0)]],
    constant uint& numBlocks [[buffer(1)]],
    uint tid [[thread_index_in_threadgroup]]
) {
    threadgroup uint sharedData[256]; // Max 256 blocks = 65536 clusters

    // Load
    sharedData[tid] = (tid < numBlocks) ? blockSums[tid] : 0;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Hillis-Steele inclusive scan
    for (uint stride = 1; stride < 256; stride *= 2) {
        uint val = 0;
        if (tid >= stride) {
            val = sharedData[tid - stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        sharedData[tid] += val;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    // Convert to exclusive and write back
    if (tid < numBlocks) {
        blockSums[tid] = (tid > 0) ? sharedData[tid - 1] : 0;
    }
}
