#include <metal_stdlib>
using namespace metal;

constexpr constant float GAMMA = 2.2;
constexpr constant float INV_GAMMA = 1.0 / GAMMA;

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]]) {
    RasterizerData vert;
    vert.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    vert.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    vert.uv.y = 1.0 - vert.uv.y; // flip Y axis because Metal uses Y-down UV space
    return vert;
}

float3 aces(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float3 linearToSRGB(float3 color) {
    // return pow(linear, float3(INV_GAMMA));
    return mix(
        color * 12.92,
        1.055 * pow(color, float3(1.0 / 2.4)) - 0.055,
        step(0.0031308, color)
    );
}

fragment float4 fragmentMain(RasterizerData in [[stage_in]], texture2d<float, access::sample> texScreen [[texture(0)]]) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    float3 color = texScreen.sample(s, in.uv).rgb;

    color = aces(color);

    // color = linearToSRGB(color);  // Already handled by swapchain

    return float4(color, 1.0);
}