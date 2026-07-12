#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path


struct Ray {
    float3 origin;
    float3 direction;
    float minDistance;
    float maxDistance;
};

struct Intersection {
    float distance;
    // unsigned int primitiveIndex;
    // float2 barycentricCoords;
};

// Soft-shadow controls. angularRadius == 0 reproduces the old single-ray hard
// shadow exactly; > 0 cone-samples the sun's disk (the real sun subtends
// ~0.0047 rad half-angle) with `samples` rays and averages — a true RT
// penumbra: sharp at contact, wider with occluder distance. No temporal
// denoise yet, so the penumbra carries residual noise; the half-res target's
// bilinear upsample softens it. Keep radius <= ~0.02 until a denoiser lands.
struct SunShadowParams {
    float angularRadius;  // radians
    uint frameIndex;
    uint samples;         // rays per pixel when angularRadius > 0
    uint _pad;
};

kernel void computeMain(
    texture2d<float> depthTexture [[texture(0)]],
    texture2d<float> normalTexture [[texture(1)]],
    texture2d<float, access::write> shadowTexture [[texture(2)]],
    constant CameraData& camera [[buffer(0)]],
    const device DirLight* directionalLights [[buffer(1)]],
    const device PointLight* pointLights [[buffer(2)]],
    constant float2& screenSize [[buffer(3)]],
    instance_acceleration_structure TLAS [[buffer(4)]],
    constant SunShadowParams& sunParams [[buffer(5)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = shadowTexture.get_width();
    uint h = shadowTexture.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    // Resolution-agnostic: depth/normal may be higher-res than the shadow target
    // (e.g. half-res shadows on retina; consumers upsample bilinearly for free)
    uint2 fullDim = uint2(depthTexture.get_width(), depthTexture.get_height());
    uint2 scale = max(fullDim / uint2(w, h), 1u);
    uint2 fullTid = min(tid * scale, fullDim - 1);

    float4 finalColor = float4(1.0, 1.0, 1.0, 1.0); // default to white
    if (!is_null_instance_acceleration_structure(TLAS)) {
        float2 uv = (float2(tid) + 0.5) / float2(w, h);
        uv.y = 1.0 - uv.y;
        float depth = depthTexture.read(fullTid).r;
        float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
        float4 viewPos = camera.invProj * ndc;
        viewPos /= viewPos.w;
        float3 worldPos = (camera.invView * viewPos).xyz;

        float3 worldNormal = normalize(normalTexture.read(fullTid).xyz); // RGBA16Float

        DirLight light = directionalLights[0];
        float3 sunDir = normalize(-light.direction);

        // Occlusion query: any hit terminates traversal, and no per-triangle
        // data (barycentrics etc.) is needed — the result is only hit/none.
        raytracing::intersector<raytracing::instancing> inter;
        inter.assume_geometry_type(raytracing::geometry_type::triangle);
        inter.accept_any_intersection(true);

        uint sampleCount = (sunParams.angularRadius > 0.0) ? max(sunParams.samples, 1u) : 1u;

        // Orthonormal basis around the sun direction for disk sampling.
        float3 up = abs(sunDir.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        float3 t1 = normalize(cross(up, sunDir));
        float3 t2 = cross(sunDir, t1);
        float tanRadius = tan(sunParams.angularRadius);

        float visibility = 0.0;
        for (uint s = 0; s < sampleCount; s++) {
            float3 dir = sunDir;
            if (sunParams.angularRadius > 0.0) {
                // Uniform disk sample over the sun's angular extent, decorrelated
                // per pixel and per frame.
                uint seed = tid.x * 1973u + tid.y * 9277u
                          + sunParams.frameIndex * 26699u + s * 6151u;
                float2 rnd = random(seed);
                float rr = sqrt(rnd.x) * tanRadius;
                float phi = rnd.y * 2.0 * PI;
                dir = normalize(sunDir + t1 * (rr * cos(phi)) + t2 * (rr * sin(phi)));
            }
            raytracing::ray r;
            r.origin = worldPos + worldNormal * 0.005;
            r.direction = dir;
            r.min_distance = 0.001;
            r.max_distance = 10000.0;
            auto intersection = inter.intersect(r, TLAS, 0xFF);
            visibility += (intersection.type == raytracing::intersection_type::none) ? 1.0 : 0.0;
        }
        visibility /= float(sampleCount);
        finalColor = float4(visibility, visibility, visibility, 1.0);
        // Debug output - world position
        // finalColor = float4(float3(worldPos), 1.0);
        // Debug output - world normal
        // finalColor = float4(float3(worldNormal), 1.0);
    }
    shadowTexture.write(finalColor, tid);
}