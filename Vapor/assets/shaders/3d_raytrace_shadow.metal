#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "assets/shaders/3d_common.metal" // TODO: use more robust include path


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

kernel void computeMain(
    texture2d<float> depthTexture [[texture(0)]],
    texture2d<float> normalTexture [[texture(1)]],
    texture2d<float, access::write> shadowTexture [[texture(2)]],
    constant CameraData& camera [[buffer(0)]],
    const device DirLight* directionalLights [[buffer(1)]],
    const device PointLight* pointLights [[buffer(2)]],
    constant float2& screenSize [[buffer(3)]],
    instance_acceleration_structure TLAS [[buffer(4)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = shadowTexture.get_width();
    uint h = shadowTexture.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    float4 finalColor = float4(1.0, 1.0, 1.0, 1.0); // default to white
    if (!is_null_instance_acceleration_structure(TLAS)) {
        float2 uv = float2(tid) / float2(w, h);
        uv.y = 1.0 - uv.y;
        float depth = depthTexture.read(tid).r;
        float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
        float4 viewPos = camera.invProj * ndc;
        viewPos /= viewPos.w;
        float3 worldPos = (camera.invView * viewPos).xyz;

        float3 worldNormal = normalize(normalTexture.read(tid).xyz); // RGBA16Float

        DirLight light = directionalLights[0];

        raytracing::ray r;
        r.origin = worldPos + worldNormal * 0.001;
        r.direction = normalize(-light.direction);
        r.min_distance = 0.1;
        r.max_distance = 10000.0;

        raytracing::intersector<raytracing::instancing, raytracing::triangle_data> inter;
        inter.assume_geometry_type(raytracing::geometry_type::triangle);
        auto intersection = inter.intersect(r, TLAS, 0xFF);
        if (intersection.type == raytracing::intersection_type::triangle) {
            finalColor = float4(0.0, 0.0, 0.0, 1.0);
        } else if (intersection.type == raytracing::intersection_type::none) {
            finalColor = float4(1.0, 1.0, 1.0, 1.0);
        }
        // Debug output - world position
        // finalColor = float4(float3(worldPos), 1.0);
        // Debug output - world normal
        // finalColor = float4(float3(worldNormal), 1.0);
    }
    shadowTexture.write(finalColor, tid);
}