#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"    // CameraData
#include "Res/shaders/3d_tess_lib.metal"  // CBT/LEB shared library

// ============================================================================
// Adaptive GPU tessellation — mesh-shader fast path (Metal object/mesh
// functions; used when RHICapabilities::meshShaders is true).
//
// The CBT update stays in compute either way — only leaf rendering forks:
// the object stage replaces both the tessLeafPrep corner cache AND the
// instanced draw: each 32-thread group claims 32 leaf indices, decodes them
// from the CBT (heap descent + bit walk), frustum-culls, compacts survivors
// with SIMD prefix sums (same hazard-free pattern 3d_meshlet.metal settled
// on), and forwards heap node ids in the payload. The mesh stage re-derives
// the leaf's corner attributes from the node id (a pure bit walk — no CBT
// reads) and emits the same 45-vertex / 64-triangle grid the compute path
// instances, with identical barycentrics — the two paths are pixel-identical.
//
// Launched via drawMeshTasksIndirect on TessArgs.meshTasks (task-grid size =
// ceil(leafCount / 32), GPU-written by tessPrepareArgs).
//
// Bindings (RHI setVertexBuffer/setVertexBytes route to BOTH object and mesh
// stages for mesh pipelines): 0 = CBT storage (read), 1 = CameraData,
// 2 = TessRoot[], 3 = TessParams (setVertexBytes).
// ============================================================================

struct TessPayload {
    uint nodes[32];
};

[[object]] void tessObjectMain(
    object_data TessPayload& payload [[payload]],
    mesh_grid_properties grid,
    device const uint*       cbt    [[buffer(0)]],
    device const CameraData& cam    [[buffer(1)]],
    device const TessRoot*   roots  [[buffer(2)]],
    constant TessParams&     params [[buffer(3)]],
    uint tid [[thread_position_in_threadgroup]],
    uint gid [[threadgroup_position_in_grid]]
) {
    uint leafIndex = gid * 32u + tid;
    uint drawn = min(tessLeafCount(cbt), params.maxLeaves);

    uint node = 0u;
    bool visible = false;
    if (leafIndex < drawn) {
        node = tessDecodeLeaf(cbt, params.maxDepth, params.rootDepth, leafIndex);
        TessTriangle tri = tessDecodeTriangle(node, params.rootDepth);
        float3 wc0 = (params.model * float4(tessCornerFromWeights(tri.w0, tri.rootSlot, roots).pos, 1.0)).xyz;
        float3 wc1 = (params.model * float4(tessCornerFromWeights(tri.w1, tri.rootSlot, roots).pos, 1.0)).xyz;
        float3 wc2 = (params.model * float4(tessCornerFromWeights(tri.w2, tri.rootSlot, roots).pos, 1.0)).xyz;
        float3 center = (wc0 + wc1 + wc2) / 3.0;
        float radius = max(distance(center, wc0), max(distance(center, wc1), distance(center, wc2)));
        radius += abs(params.displacementScale) * 1.5;
        visible = true;
        for (int i = 0; i < 6; ++i) {
            float4 plane = cam.frustumPlanes[i];
            if (dot(plane.xyz, center) + plane.w < -radius) { visible = false; break; }
        }
    }

    // SIMD-prefix-sum compaction (one 32-wide SIMD group per object
    // threadgroup — in-lane slot assignment, no threadgroup atomics/barriers;
    // see 3d_meshlet.metal's payload-ordering note for why).
    uint vote = visible ? 1u : 0u;
    uint slot = simd_prefix_exclusive_sum(vote);
    if (visible) {
        payload.nodes[slot] = node;
    }
    uint count = simd_sum(vote);
    if (tid == 0u) {
        grid.set_threadgroups_per_grid(uint3(count, 1u, 1u));
    }
}

struct TessMeshVertexOut {
    float4 position [[position]];
    float3 worldNormal;
    float3 worldPosition;
    float2 uv;
    uint depth [[flat]];  // same for all verts of a leaf; flat = provoking vertex
    uint node [[flat]];
};

// Real per-primitive type + set_primitive() per counted primitive — the
// structure 3d_meshlet.metal proved out (see its MeshletPrimOut note). The
// fragment consumes only the vertex struct, matching that shader.
struct TessMeshPrimOut {
    float3 primColor [[flat]];
};

using TessMeshT = metal::mesh<TessMeshVertexOut, TessMeshPrimOut, TESS_GRID_VERTS,
                              TESS_GRID_TRIS, metal::topology::triangle>;

[[mesh]] void tessMeshMain(
    TessMeshT output,
    const object_data TessPayload& payload [[payload]],
    device const CameraData& cam    [[buffer(1)]],
    device const TessRoot*   roots  [[buffer(2)]],
    constant TessParams&     params [[buffer(3)]],
    uint tid [[thread_position_in_threadgroup]],
    uint gid [[threadgroup_position_in_grid]]
) {
    uint node = payload.nodes[gid];
    // Every thread re-runs the (cheap, CBT-free) bit-walk decode — redundant
    // arithmetic instead of threadgroup memory + barrier.
    TessTriangle tri = tessDecodeTriangle(node, params.rootDepth);
    TessCorner c0 = tessCornerFromWeights(tri.w0, tri.rootSlot, roots);
    TessCorner c1 = tessCornerFromWeights(tri.w1, tri.rootSlot, roots);
    TessCorner c2 = tessCornerFromWeights(tri.w2, tri.rootSlot, roots);

    if (tid < TESS_GRID_VERTS) {
        uint2 rc = tessGridVertexRC(tid);
        float3 w = tessGridBarycentric(rc.x, rc.y);
        float3 pos = w.x * c0.pos + w.y * c1.pos + w.z * c2.pos;
        float3 nrm = normalize(w.x * c0.nrm + w.y * c1.nrm + w.z * c2.nrm);
        float2 uv = w.x * c0.uv + w.y * c1.uv + w.z * c2.uv;
        pos += nrm * tessDisplaceAmount(pos, params.displacementScale);

        float4 world = params.model * float4(pos, 1.0);
        TessMeshVertexOut v;
        v.position = cam.proj * cam.view * world;
        v.worldPosition = world.xyz;
        v.worldNormal = normalize((params.model * float4(nrm, 0.0)).xyz);
        v.uv = uv;
        v.depth = tessDepthOf(node);
        v.node = node;
        output.set_vertex(tid, v);
    }
    if (tid < TESS_GRID_TRIS) {
        uint3 idx = tessGridTriangle(tid);
        output.set_index(tid * 3u + 0u, idx.x);
        output.set_index(tid * 3u + 1u, idx.y);
        output.set_index(tid * 3u + 2u, idx.z);
        TessMeshPrimOut p;
        p.primColor = tessHashColor(node);
        output.set_primitive(tid, p);
    }
    if (tid == 0u) {
        output.set_primitive_count(TESS_GRID_TRIS);
    }
}

fragment float4 tessMeshFragmentMain(TessMeshVertexOut in [[stage_in]]) {
    // Same debug shading as the compute path (tessFragmentMain) so the two
    // routes are visually interchangeable.
    float3 base = tessHashColor(in.depth * 2654435761u);
    float3 lightDir = normalize(float3(0.4, 1.0, 0.3));
    float ndl = max(dot(normalize(in.worldNormal), lightDir), 0.0);
    float3 color = base * (0.25 + 0.75 * ndl);
    return float4(color, 1.0);
}
