#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

struct FrameData {
    uint frameNumber;
    float time;
    float deltaTime;
};

constant uint RAYS_PER_PIXEL = 2;      // temporal accumulation supplies the rest
constant float AO_RAY_LENGTH = 1.5;    // meters; also the falloff range

// True ray-traced AO, 1 sample slot per pixel per frame, meant to be followed
// by temporal accumulation + spatial (à-trous) denoise (ADR-008).
kernel void computeMain(
    texture2d<float> depthTexture [[texture(0)]],
    texture2d<float> normalTexture [[texture(1)]],
    texture2d<float, access::write> aoTexture [[texture(2)]],
    constant FrameData& frame [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    instance_acceleration_structure TLAS [[buffer(2)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = aoTexture.get_width();
    uint h = aoTexture.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    // The AO chain runs at half resolution; depth/normal are full-res
    uint2 fullTid = min(tid * 2, uint2(depthTexture.get_width() - 1, depthTexture.get_height() - 1));

    float depth = depthTexture.read(fullTid).r;
    // Sky / far plane: fully unoccluded, no rays
    if (depth >= 0.999999 || is_null_instance_acceleration_structure(TLAS)) {
        aoTexture.write(float4(1.0), tid);
        return;
    }

    float2 uv = (float2(tid) + 0.5) / float2(w, h);
    uv.y = 1.0 - uv.y;
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float3 worldNormal = normalize(normalTexture.read(fullTid).xyz);

    // Occlusion query: any hit terminates traversal; no per-triangle data needed
    raytracing::intersector<raytracing::instancing> isect;
    isect.assume_geometry_type(raytracing::geometry_type::triangle);
    isect.accept_any_intersection(true);

    float occlusion = 0.0;
    for (uint i = 0; i < RAYS_PER_PIXEL; i++) {
        // TODO: swap the hash for a tiled blue-noise texture (easier to denoise)
        float2 xi = random(tid.x + tid.y * w + i * 7919u + frame.frameNumber * 26699u);
        float3 dir = sampleCosineWeightedHemisphere(xi, worldNormal);

        raytracing::ray r;
        r.origin = worldPos + worldNormal * 1e-3;
        r.direction = dir;
        r.min_distance = 0.0;
        r.max_distance = AO_RAY_LENGTH;

        auto hit = isect.intersect(r, TLAS, 0xFF);
        if (hit.type != raytracing::intersection_type::none) {
            // Distance falloff: nearby occluders matter more. With any-hit the
            // reported distance is the first accepted hit, not the closest —
            // an acceptable bias for AO.
            occlusion += 1.0 - saturate(hit.distance / AO_RAY_LENGTH);
        }
    }
    float ao = 1.0 - occlusion / float(RAYS_PER_PIXEL);
    aoTexture.write(float4(ao, 0.0, 0.0, 0.0), tid);
}
