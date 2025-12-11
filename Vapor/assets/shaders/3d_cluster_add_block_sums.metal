#include <metal_stdlib>
using namespace metal;

// Pass 2c: Add block sums to all elements

struct ClusterCompact {
    float4 min;
    float4 max;
    uint offset;
    uint lightCount;
};

constant uint BLOCK_SIZE = 256;

kernel void computeMain(
    device ClusterCompact* clusters [[buffer(0)]],
    const device uint* blockSums [[buffer(1)]],
    constant uint& numClusters [[buffer(2)]],
    uint tid [[thread_index_in_threadgroup]],
    uint gid [[threadgroup_position_in_grid]]
) {
    uint globalIdx = gid * BLOCK_SIZE + tid;

    if (globalIdx < numClusters && gid > 0) {
        clusters[globalIdx].offset += blockSums[gid];
    }
}
