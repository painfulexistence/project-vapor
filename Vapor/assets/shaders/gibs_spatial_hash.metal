#include <metal_stdlib>
#include "Res/shaders/gibs_common.metal"
using namespace metal;

// ============================================================================
// GIBS Spatial Hash Construction — linked-list variant
//
// Design Decision: per-cell linked lists instead of counting sort.
// The previous counting-sort approach needed a prefix sum over every cell;
// its serial kernel walked millions of cells on ONE thread and dominated
// frame time. Linked lists need only two fully parallel dispatches:
//   1. clearCellHeads: cellHeads[i] = INVALID for all cells
//   2. insertSurfels:  next[i] = atomic_exchange(cellHeads[cell], i)
// Lookup walks the chain: for (j = cellHeads[cell]; j != INVALID; j = next[j])
// No sorted surfel copy is needed — all passes read the canonical buffer,
// which also removes the sorted-vs-canonical irradiance duality.
// ============================================================================

// Pass 1: reset all cell list heads
kernel void clearCellHeads(
    device uint* cellHeads [[buffer(0)]],
    constant GIBSData& gibs [[buffer(1)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid < gibs.totalCells) {
        cellHeads[gid] = GIBS_INVALID_INDEX;
    }
}

// Pass 2: push each surfel onto its cell's list
kernel void insertSurfels(
    device const Surfel* surfels [[buffer(0)]],
    device atomic_uint* cellHeads [[buffer(1)]],
    device uint* surfelNext [[buffer(2)]],
    constant GIBSData& gibs [[buffer(3)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfels[gid];
    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
        return;
    }

    uint cellIndex = surfel.cellHash;
    if (cellIndex >= gibs.totalCells) {
        return;
    }

    uint previousHead = atomic_exchange_explicit(&cellHeads[cellIndex], gid, memory_order_relaxed);
    surfelNext[gid] = previousHead;
}
