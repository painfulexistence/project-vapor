#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"    // CameraData
#include "Res/shaders/3d_terrain_noise.metal"  // terrain heightfield (used by 3d_tess_lib)
#include "Res/shaders/3d_tess_lib.metal"  // CBT/LEB shared library

// ============================================================================
// Adaptive GPU tessellation — TessUpdate pass kernels (one chain per
// tessellated mesh, all inside one compute encoder, barriers between):
//
//   tessClassify     indirect dispatch over last frame's leaf count: decode
//                    each leaf, evaluate the screen-space LoD metric on its
//                    hypotenuse, and split (conforming chain) or merge
//                    (diamond rule) via single-bit atomics on the bitfield.
//   tessReduceFirst  popcount the bitfield into the deepest count level.
//   tessReduceLevel  one halving step per level up to the root (tiny
//                    dispatches; count[0] ends up = total leaf count).
//   tessPrepareArgs  single thread: write next frame's classify dispatch
//                    args, this frame's leafPrep args, the indirect draw
//                    command (instanceCount = leaf count) and the mesh-path
//                    task grid — the CPU never sees any of these numbers.
//   tessLeafPrep     indirect dispatch over the NEW leaf count: decode each
//                    leaf once into the TessLeafData corner cache + frustum
//                    visibility, so the instanced vertex shader only lerps.
//                    (The mesh-shader path skips this: its object stage
//                    decodes and culls in-line.)
//
// Bindings (all kernels, unused slots simply not bound):
//   0 = CBT storage, read view (device const uint*)
//   1 = CBT storage, SAME buffer, atomic write view (split/merge bits)
//   2 = TessRoot[] (corners + root adjacency)
//   3 = CameraData
//   4 = inline constants (TessParams, or TessReduceParams for the reduction)
//   5 = TessArgs
//   6 = TessLeafData[] corner cache
// ============================================================================

// ---- classify --------------------------------------------------------------

static void tessSplitBit(device atomic_uint* cbtRW, uint maxDepth, uint node) {
    uint d = tessDepthOf(node);
    if (d >= maxDepth) return;
    uint pos = tessCeilBitOf(node << 1u | 1u, maxDepth);
    atomic_fetch_or_explicit(&cbtRW[tessBitfieldOffset(maxDepth) + (pos >> 5u)],
                             1u << (pos & 31u), memory_order_relaxed);
}

// Conforming split: mark the node, then walk the compatibility chain across
// hypotenuses (mirror of CBT::splitConforming — idempotent bit sets, safe
// under concurrency).
static void tessSplitConforming(device atomic_uint* cbtRW, uint node,
                                constant TessParams& params, device const TessRoot* roots) {
    if (tessDepthOf(node) >= params.maxDepth) return;
    tessSplitBit(cbtRW, params.maxDepth, node);
    uint t = tessNeighbors(node, params.rootDepth, roots).edge;
    uint guard = 0u;
    while (t != 0u && guard++ < 64u) {
        tessSplitBit(cbtRW, params.maxDepth, t);
        // A root's hypotenuse partner is the node the chain came from.
        if (tessDepthOf(t) <= params.rootDepth) break;
        t >>= 1u;
        tessSplitBit(cbtRW, params.maxDepth, t);
        t = tessNeighbors(t, params.rootDepth, roots).edge;
    }
}

// Diamond merge (mirror of CBT::mergeConforming): clear our pair's right
// sibling if the parent holds exactly its two leaves and the parent's
// hypotenuse partner holds at most two (== 1: partner already merged; > 2:
// still subdivided on the shared edge — merging would leave a T-vertex).
static void tessMergeConforming(device const uint* cbtRO, device atomic_uint* cbtRW,
                                uint leaf, constant TessParams& params,
                                device const TessRoot* roots) {
    uint d = tessDepthOf(leaf);
    if (d <= params.rootDepth) return;
    uint parent = leaf >> 1u;
    if (tessHeapRead(cbtRO, params.maxDepth, parent) != 2u) return;
    uint top = tessNeighbors(parent, params.rootDepth, roots).edge;
    if (top != 0u && tessHeapRead(cbtRO, params.maxDepth, top) > 2u) return;
    uint pos = tessCeilBitOf(leaf | 1u, params.maxDepth);
    atomic_fetch_and_explicit(&cbtRW[tessBitfieldOffset(params.maxDepth) + (pos >> 5u)],
                              ~(1u << (pos & 31u)), memory_order_relaxed);
}

kernel void tessClassify(
    device const uint*       cbtRO  [[buffer(0)]],
    device atomic_uint*      cbtRW  [[buffer(1)]],
    device const TessRoot*   roots  [[buffer(2)]],
    device const CameraData& cam    [[buffer(3)]],
    constant TessParams&     params [[buffer(4)]],
    uint tid [[thread_position_in_grid]]
) {
    uint leafCount = tessLeafCount(cbtRO);
    if (tid >= leafCount) return;
    if ((params.flags & TESS_FLAG_FREEZE) != 0u) return;

    uint node = tessDecodeLeaf(cbtRO, params.maxDepth, params.rootDepth, tid);
    TessTriangle tri = tessDecodeTriangle(node, params.rootDepth);

    // World-space hypotenuse endpoints (v2, v0) — the LoD metric edge.
    float3 p0 = tessCornerFromWeights(tri.w0, tri.rootSlot, roots).pos;
    float3 p2 = tessCornerFromWeights(tri.w2, tri.rootSlot, roots).pos;
    float3 w0 = (params.model * float4(p0, 1.0)).xyz;
    float3 w2 = (params.model * float4(p2, 1.0)).xyz;
    // Terrain: measure the DISPLACED edge — with up to heightScale of relief,
    // a flat-plane metric would think geometry right under the camera is
    // hundreds of metres away and under-subdivide exactly where it matters.
    if ((params.flags & TESS_FLAG_TERRAIN) != 0u) {
        w0.y += tessTerrainHeight(p0, params);
        w2.y += tessTerrainHeight(p2, params);
    }
    float hypPx = tessProjectedPixels(w2, w0, cam, params.screenHeight);

    uint depth = tessDepthOf(node);
    if (hypPx > params.splitPixels && depth < params.maxDepth) {
        // Soft capacity guard: a split round adds at most ~2 leaves per
        // split chain link; stop splitting well before the leaf cache fills
        // so the draw clamp never truncates visible geometry.
        if (leafCount * 4u < params.maxLeaves * 3u) {
            tessSplitConforming(cbtRW, node, params, roots);
        }
    } else if (depth > params.rootDepth) {
        // Merge when even the PARENT's hypotenuse projects under the split
        // threshold (hysteresis: a just-merged parent never immediately
        // re-splits). The parent hypotenuse is (parent v2, parent v0);
        // reconstruct it by undoing the last bisection: for bit 0 the parent
        // corners are (v0, v2', ...) — cheaper: decode the parent directly.
        TessTriangle pt = tessDecodeTriangle(node >> 1u, params.rootDepth);
        float3 pp0 = tessCornerFromWeights(pt.w0, pt.rootSlot, roots).pos;
        float3 pp2 = tessCornerFromWeights(pt.w2, pt.rootSlot, roots).pos;
        float3 pw0 = (params.model * float4(pp0, 1.0)).xyz;
        float3 pw2 = (params.model * float4(pp2, 1.0)).xyz;
        if ((params.flags & TESS_FLAG_TERRAIN) != 0u) {
            pw0.y += tessTerrainHeight(pp0, params);
            pw2.y += tessTerrainHeight(pp2, params);
        }
        float parentPx = tessProjectedPixels(pw2, pw0, cam, params.screenHeight);
        if (parentPx < params.splitPixels) {
            tessMergeConforming(cbtRO, cbtRW, node, params, roots);
        }
    }
}

// ---- reduction -------------------------------------------------------------

// Inline constants for the reduction kernels (setComputeBytes binding 4).
struct TessReduceParams {
    uint maxDepth;
    uint level;  // tessReduceLevel: the level being WRITTEN
    uint pad0;
    uint pad1;
};

// bitfield word -> popcount into the deepest count level (maxDepth - 5).
kernel void tessReduceFirst(
    device uint*                cbt    [[buffer(0)]],
    constant TessReduceParams&  params [[buffer(4)]],
    uint tid [[thread_position_in_grid]]
) {
    uint s = params.maxDepth - 5u;
    if (tid >= (1u << s)) return;
    cbt[(1u << s) - 1u + tid] = popcount(cbt[tessBitfieldOffset(params.maxDepth) + tid]);
}

// One halving step: level d from level d+1 (dispatched for d = S-1 .. 0).
kernel void tessReduceLevel(
    device uint*                cbt    [[buffer(0)]],
    constant TessReduceParams&  params [[buffer(4)]],
    uint tid [[thread_position_in_grid]]
) {
    uint d = params.level;
    if (tid >= (1u << d)) return;
    uint childOff = (2u << d) - 1u;
    cbt[(1u << d) - 1u + tid] = cbt[childOff + 2u * tid] + cbt[childOff + 2u * tid + 1u];
}

// ---- indirect args ---------------------------------------------------------

kernel void tessPrepareArgs(
    device const uint*   cbtRO  [[buffer(0)]],
    constant TessParams& params [[buffer(4)]],
    device TessArgs&     args   [[buffer(5)]],
    uint tid [[thread_position_in_grid]]
) {
    if (tid != 0u) return;
    uint leafCount = tessLeafCount(cbtRO);
    uint drawn = min(leafCount, params.maxLeaves);

    args.classify[0] = (leafCount + 63u) / 64u;
    args.classify[1] = 1u;
    args.classify[2] = 1u;
    args.leafPrep[0] = (drawn + 63u) / 64u;
    args.leafPrep[1] = 1u;
    args.leafPrep[2] = 1u;
    args.draw[0] = params.gridIndexCount;  // indexCount
    args.draw[1] = drawn;                  // instanceCount (one per leaf)
    args.draw[2] = 0u;                     // firstIndex
    args.draw[3] = 0u;                     // vertexOffset
    args.draw[4] = 0u;                     // firstInstance
    args.meshTasks[0] = (drawn + 31u) / 32u;
    args.meshTasks[1] = 1u;
    args.meshTasks[2] = 1u;
    args.leafCount = leafCount;
}

// ---- leaf corner cache (compute path only) ---------------------------------

kernel void tessLeafPrep(
    device const uint*       cbtRO  [[buffer(0)]],
    device const TessRoot*   roots  [[buffer(2)]],
    device const CameraData& cam    [[buffer(3)]],
    constant TessParams&     params [[buffer(4)]],
    device TessLeafData*     leaves [[buffer(6)]],
    uint tid [[thread_position_in_grid]]
) {
    uint drawn = min(tessLeafCount(cbtRO), params.maxLeaves);
    if (tid >= drawn) return;

    uint node = tessDecodeLeaf(cbtRO, params.maxDepth, params.rootDepth, tid);
    TessTriangle tri = tessDecodeTriangle(node, params.rootDepth);
    TessCorner c0 = tessCornerFromWeights(tri.w0, tri.rootSlot, roots);
    TessCorner c1 = tessCornerFromWeights(tri.w1, tri.rootSlot, roots);
    TessCorner c2 = tessCornerFromWeights(tri.w2, tri.rootSlot, roots);

    // Frustum: world bounding sphere of the three corners, padded by the
    // displacement bound.
    float3 wc0 = (params.model * float4(c0.pos, 1.0)).xyz;
    float3 wc1 = (params.model * float4(c1.pos, 1.0)).xyz;
    float3 wc2 = (params.model * float4(c2.pos, 1.0)).xyz;
    float3 center = (wc0 + wc1 + wc2) / 3.0;
    float radius = max(distance(center, wc0), max(distance(center, wc1), distance(center, wc2)));
    radius += abs(params.displacementScale) * 1.5;
    bool visible = true;
    for (int i = 0; i < 6; ++i) {
        float4 plane = cam.frustumPlanes[i];
        if (dot(plane.xyz, center) + plane.w < -radius) { visible = false; break; }
    }

    TessLeafData L;
    L.posU[0] = float4(c0.pos, c0.uv.x);
    L.posU[1] = float4(c1.pos, c1.uv.x);
    L.posU[2] = float4(c2.pos, c2.uv.x);
    L.nrmV[0] = float4(c0.nrm, c0.uv.y);
    L.nrmV[1] = float4(c1.nrm, c1.uv.y);
    L.nrmV[2] = float4(c2.nrm, c2.uv.y);
    L.visible = visible ? 1u : 0u;
    L.depth = tessDepthOf(node);
    L.node = node;
    L.pad = 0u;
    leaves[tid] = L;
}
