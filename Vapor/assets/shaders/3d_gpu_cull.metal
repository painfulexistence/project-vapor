#include <metal_stdlib>
using namespace metal;

// GPU-driven rendering: frustum-cull compute kernel (Metal backend).
// Mirror of GpuCull.comp. One thread per instance; writes one indirect draw
// command per instance. Visible => instanceCount 1, culled => 0 (the indirect
// draw skips those). firstInstance carries the instance index so the vertex
// shader can look up InstanceData.

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float near;
    float far;
    float3 position;
    float4 frustumPlanes[6];
};

struct InstanceData {
    float4x4 model;
    float4 color;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    float3 AABBMin;
    float3 AABBMax;
    float4 boundingSphere; // xyz = world center, w = radius
};

// Layout matches Vapor::DrawCommand and MTLDrawIndexedPrimitivesIndirectArguments
// (packed, 20 bytes).
struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

// setComputeBytes(binding 4). Mirror of GpuCull.comp's OccPC push constant.
struct OccParams {
    uint occlusionEnabled;
    uint hizMipCount;
    float2 hizSize;      // mip-0 dimensions in texels
};

// Hi-Z occlusion test — mirror of GpuCull.comp::occludedByHiZ. Standard [0,1]
// depth; store/compare against the farthest occluder depth over the AABB's
// screen footprint. Conservative: any doubt returns false (keep the instance).
static bool occludedByHiZ(device const CameraData& cam, float3 aabbMin, float3 aabbMax,
                          texture2d<float> hiz, sampler hizSampler, OccParams occ) {
    float4x4 vp = cam.proj * cam.view;
    float2 uvMin = float2(1.0);
    float2 uvMax = float2(0.0);
    float minDepth = 1.0;
    for (int i = 0; i < 8; ++i) {
        float3 corner = float3((i & 1) != 0 ? aabbMax.x : aabbMin.x,
                               (i & 2) != 0 ? aabbMax.y : aabbMin.y,
                               (i & 4) != 0 ? aabbMax.z : aabbMin.z);
        float4 clip = vp * float4(corner, 1.0);
        if (clip.w <= 0.0) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * 0.5 + 0.5;
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        minDepth = min(minDepth, ndc.z);
    }
    if (uvMax.x < 0.0 || uvMin.x > 1.0 || uvMax.y < 0.0 || uvMin.y > 1.0) return false;
    uvMin = clamp(uvMin, 0.0, 1.0);
    uvMax = clamp(uvMax, 0.0, 1.0);

    float2 hMin = float2(uvMin.x, 1.0 - uvMax.y);
    float2 hMax = float2(uvMax.x, 1.0 - uvMin.y);

    float2 sizePx = (hMax - hMin) * occ.hizSize;
    float lvl = ceil(log2(max(max(sizePx.x, sizePx.y), 1.0)));
    lvl = clamp(lvl, 0.0, float(occ.hizMipCount - 1u));

    float o = 0.0;
    o = max(o, hiz.sample(hizSampler, float2(hMin.x, hMin.y), level(lvl)).r);
    o = max(o, hiz.sample(hizSampler, float2(hMax.x, hMin.y), level(lvl)).r);
    o = max(o, hiz.sample(hizSampler, float2(hMin.x, hMax.y), level(lvl)).r);
    o = max(o, hiz.sample(hizSampler, float2(hMax.x, hMax.y), level(lvl)).r);

    return minDepth > o;
}

// Entry point must be named computeMain (RHI Metal compute convention).
kernel void computeMain(
    device const CameraData&   cam           [[buffer(0)]],
    device const InstanceData* instances     [[buffer(1)]],
    device DrawCommand*        commands      [[buffer(2)]],
    constant uint&             instanceCount [[buffer(3)]],
    constant OccParams&        occ           [[buffer(4)]],
    texture2d<float>           hiz           [[texture(4)]],
    sampler                    hizSampler    [[sampler(4)]],
    uint                       id            [[thread_position_in_grid]]
) {
    if (id >= instanceCount) {
        return;
    }

    InstanceData inst = instances[id];
    float3 center = inst.boundingSphere.xyz;
    float radius = inst.boundingSphere.w;

    bool visible = true;
    for (int i = 0; i < 6; ++i) {
        float4 plane = cam.frustumPlanes[i]; // normalized, points inward
        if (dot(plane.xyz, center) + plane.w < -radius) {
            visible = false;
            break;
        }
    }
    if (visible && occ.occlusionEnabled != 0u &&
        occludedByHiZ(cam, inst.AABBMin, inst.AABBMax, hiz, hizSampler, occ)) {
        visible = false;
    }

    DrawCommand cmd;
    cmd.indexCount = inst.indexCount;
    cmd.instanceCount = visible ? 1u : 0u;
    cmd.firstIndex = inst.indexOffset;
    // baseVertex must stay 0 on Metal, unlike the Vulkan kernel: [[vertex_id]]
    // already includes baseVertex, and the Metal main-pass vertex shader pulls
    // vertices manually via instances[iid].vertexOffset + vertex_id. Writing the
    // offset here too would apply it twice (2x vertexOffset in MDI mode -> every
    // mesh fetches past its own vertices -> garbage/degenerate geometry).
    cmd.vertexOffset = 0;
    cmd.firstInstance = id;
    commands[id] = cmd;
}
