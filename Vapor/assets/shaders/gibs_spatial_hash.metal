#include <metal_stdlib>
#include "gibs_common.metal"
using namespace metal;

// ============================================================================
// GIBS Spatial Hash Construction
// Builds spatial hash for fast surfel neighbor queries
//
// Design Decision: Uniform spatial hash chosen over octree for GPU efficiency
// Uses counting sort approach:
// 1. Count surfels per cell
// 2. Prefix sum to get offsets
// 3. Scatter surfels to sorted positions
// See GIBS_DESIGN.md Decision #2
// ============================================================================

// Pass 1: Count surfels per cell
kernel void countSurfelsPerCell(
    device const Surfel* surfels [[buffer(0)]],
    device atomic_uint* cellCounts [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfels[gid];

    // Skip invalid surfels
    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
        return;
    }

    // Increment count for this cell
    uint cellIndex = surfel.cellHash;
    if (cellIndex < gibs.totalCells) {
        atomic_fetch_add_explicit(&cellCounts[cellIndex], 1, memory_order_relaxed);
    }
}

// Pass 2: Prefix sum for cell offsets (simplified single-threaded version)
// For production, use parallel prefix sum (Blelloch scan)
kernel void prefixSumCells(
    device uint* cellCounts [[buffer(0)]],
    device SurfelCell* cells [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    uint gid [[thread_position_in_grid]]
) {
    // This kernel processes cells in blocks for prefix sum
    // For simplicity, this is a work-efficient but not fully parallel version

    uint blockSize = 256;
    uint blockIndex = gid;
    uint startCell = blockIndex * blockSize;
    uint endCell = min(startCell + blockSize, gibs.totalCells);

    if (startCell >= gibs.totalCells) {
        return;
    }

    // Local prefix sum within this block
    uint runningSum = 0;

    // First, we need to know the sum of all previous blocks
    // This is handled by a separate reduction pass in practice
    // For now, compute prefix within block

    for (uint i = startCell; i < endCell; i++) {
        uint count = cellCounts[i];

        cells[i].surfelOffset = runningSum;
        cells[i].surfelCount = count;

        runningSum += count;
    }
}

// Alternative: Simple serial prefix sum (for small cell counts)
kernel void prefixSumCellsSerial(
    device uint* cellCounts [[buffer(0)]],
    device SurfelCell* cells [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    uint gid [[thread_position_in_grid]]
) {
    // Only thread 0 does the work
    if (gid != 0) {
        return;
    }

    uint runningSum = 0;
    for (uint i = 0; i < gibs.totalCells; i++) {
        uint count = cellCounts[i];

        cells[i].surfelOffset = runningSum;
        cells[i].surfelCount = count;

        runningSum += count;

        // Reset count for next frame
        cellCounts[i] = 0;
    }
}

// Pass 3: Scatter surfels to sorted positions
kernel void scatterSurfels(
    device const Surfel* surfelsIn [[buffer(0)]],
    device Surfel* surfelsOut [[buffer(1)]],
    device SurfelCell* cells [[buffer(2)]],
    device atomic_uint* cellWriteOffsets [[buffer(3)]],
    constant GIBSData& gibs [[buffer(4)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfelsIn[gid];

    // Skip invalid surfels
    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
        return;
    }

    uint cellIndex = surfel.cellHash;
    if (cellIndex >= gibs.totalCells) {
        return;
    }

    // Get base offset for this cell
    uint baseOffset = cells[cellIndex].surfelOffset;

    // Atomically get write position within cell
    uint localOffset = atomic_fetch_add_explicit(&cellWriteOffsets[cellIndex], 1, memory_order_relaxed);

    // Bounds check
    uint writeIndex = baseOffset + localOffset;
    if (writeIndex < gibs.maxSurfels && localOffset < cells[cellIndex].surfelCount) {
        surfelsOut[writeIndex] = surfel;
    }
}

// Clear cell counts for next frame
kernel void clearCellCounts(
    device uint* cellCounts [[buffer(0)]],
    constant GIBSData& gibs [[buffer(1)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid < gibs.totalCells) {
        cellCounts[gid] = 0;
    }
}

// Initialize cells (set all to empty)
kernel void initializeCells(
    device SurfelCell* cells [[buffer(0)]],
    constant GIBSData& gibs [[buffer(1)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid < gibs.totalCells) {
        cells[gid].surfelOffset = 0;
        cells[gid].surfelCount = 0;
    }
}
