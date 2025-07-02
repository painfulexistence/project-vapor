#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

constant int NUM_SAMPLES = 32;

struct FrameData {
    uint frameNumber;
    float time;
    float deltaTime;
};

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
    texture2d<float, access::write> aoTexture [[texture(2)]],
    constant FrameData& frame [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = aoTexture.get_width();
    uint h = aoTexture.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    float result = 1.0; // default to 1

    float2 uv = float2(tid) / float2(w, h);
    // uv.y = 1.0 - uv.y;
    float depth = depthTexture.read(tid).r;
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float3 worldNormal = normalize(normalTexture.read(tid).rgb); // RGBA16Float

    float bias = 0.001;
    float radius = 0.005;
    uint count = 0;
    float occlusion = 0.0;
    for (uint i = 0; i < NUM_SAMPLES; i++) {
        float2 seed = random(tid.x + tid.y * w + i * 79 + frame.frameNumber * 19);
        float3 dir = normalize(sampleHemisphere(seed, worldNormal));

        float3 samplePos = worldPos + worldNormal * 0.001 + dir * radius;
        float4 clipPos = camera.proj * camera.view * float4(samplePos, 1.0);
        clipPos.xyz /= clipPos.w;
        float2 sampleUV = clipPos.xy * 0.5 + 0.5;
        if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 && sampleUV.y >= 0.0 && sampleUV.y <= 1.0) {
            float sampleDepth = depthTexture.read(uint2(sampleUV * float2(w, h))).r;
            if (clipPos.z > sampleDepth + bias) {
                occlusion += 1.0;
            }
            count++;
        }
    }
    occlusion /= float(count);
    result = 1.0 - occlusion;
    // result = float(count) / float(NUM_SAMPLES);

    // Debug output - world position
    // finalColor = float4(float3(worldPos), 1.0);
    // Debug output - world normal
    // finalColor = float4(float3(worldNormal * 0.5 + 0.5), 1.0);
    // Debug output - depth value
    // finalColor = float4(float3(depth), 1.0);
    // Debug output - UV coordinates
    // finalColor = float4(float3(uv, 0.0), 1.0);
    // Debug output - raw occlusion (0-1 range)
    // finalColor = float4(float3(occlusion), 1.0);

    aoTexture.write(float4(result, 0.0, 0.0, 0.0), tid);
}