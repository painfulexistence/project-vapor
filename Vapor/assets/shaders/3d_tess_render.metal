#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"    // CameraData
#include "Res/shaders/3d_tess_lib.metal"  // CBT/LEB shared library

// ============================================================================
// Adaptive GPU tessellation — instanced draw path (the compute-primary route,
// used when mesh shaders are unavailable, and as the reference the mesh path
// must match pixel-for-pixel).
//
// One instance per CBT leaf (instanceCount written by tessPrepareArgs,
// consumed via drawIndexedIndirect). The vertex shader pulls a grid vertex's
// barycentrics from buffer(0) and lerps the leaf corners the tessLeafPrep
// kernel cached — no per-vertex CBT decode (that was the thesis pipeline's
// hot spot; here decode runs once per leaf in compute).
//
// Bindings: 0 = grid barycentrics (float2: w1, w2), 1 = CameraData,
//           2 = TessLeafData[], 3 = TessParams (setVertexBytes).
// ============================================================================

struct TessVertexOut {
    float4 position [[position]];
    float3 worldNormal;
    float3 worldPosition;
    float2 uv;
    uint depth [[flat]];
    uint node [[flat]];
};

vertex TessVertexOut tessVertexMain(
    uint vertexID [[vertex_id]],
    uint instanceID [[instance_id]],
    device const float2*       gridVerts [[buffer(0)]],
    device const CameraData&   cam       [[buffer(1)]],
    device const TessLeafData* leaves    [[buffer(2)]],
    constant TessParams&       params    [[buffer(3)]]
) {
    TessVertexOut out;
    TessLeafData leaf = leaves[instanceID];
    if (leaf.visible == 0u) {
        // Frustum-culled leaf: collapse the whole instance to one point so
        // the rasterizer drops it (kept instead of compacted — the leaf list
        // stays in deterministic heap order).
        out.position = float4(2.0, 2.0, 2.0, 1.0);
        out.worldNormal = float3(0, 1, 0);
        out.worldPosition = float3(0);
        out.uv = float2(0);
        out.depth = 0u;
        out.node = 0u;
        return out;
    }

    float2 g = gridVerts[vertexID];
    float3 w = float3(1.0 - g.x - g.y, g.x, g.y);  // (w0, w1, w2)

    float3 pos = w.x * leaf.posU[0].xyz + w.y * leaf.posU[1].xyz + w.z * leaf.posU[2].xyz;
    float3 nrm = w.x * leaf.nrmV[0].xyz + w.y * leaf.nrmV[1].xyz + w.z * leaf.nrmV[2].xyz;
    float2 uv = w.x * float2(leaf.posU[0].w, leaf.nrmV[0].w) +
                w.y * float2(leaf.posU[1].w, leaf.nrmV[1].w) +
                w.z * float2(leaf.posU[2].w, leaf.nrmV[2].w);

    // Displacement is a function of the undisplaced object-space position
    // only, so leaves sharing an edge displace its vertices identically.
    nrm = normalize(nrm);
    pos += nrm * tessDisplaceAmount(pos, params.displacementScale);

    float4 world = params.model * float4(pos, 1.0);
    out.position = cam.proj * cam.view * world;
    out.worldPosition = world.xyz;
    // Uniform-scale assumption for the normal (matches the debug shading
    // below; proper inverse-transpose comes with material integration).
    out.worldNormal = normalize((params.model * float4(nrm, 0.0)).xyz);
    out.uv = uv;
    out.depth = leaf.depth;
    out.node = leaf.node;
    return out;
}

fragment float4 tessFragmentMain(TessVertexOut in [[stage_in]]) {
    // Debug shading, matching the meshlet path's conventions: hash color per
    // subdivision depth (the LoD visualization), simple lambert + ambient.
    float3 base = tessHashColor(in.depth * 2654435761u);
    float3 lightDir = normalize(float3(0.4, 1.0, 0.3));
    float ndl = max(dot(normalize(in.worldNormal), lightDir), 0.0);
    float3 color = base * (0.25 + 0.75 * ndl);
    return float4(color, 1.0);
}
