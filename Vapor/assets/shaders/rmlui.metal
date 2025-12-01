#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4x4 projectionMatrix;
    float4x4 transformMatrix;
};

struct VertexIn {
    float2 position  [[attribute(0)]];
    float4 color     [[attribute(1)]];
    float2 texCoords [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 texCoords;
    float4 color;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                           constant Uniforms& uniforms [[buffer(0)]]) {
    VertexOut out;
    // Transform position from screen space to NDC
    float4 pos = float4(in.position, 0.0, 1.0);
    out.position = uniforms.projectionMatrix * uniforms.transformMatrix * pos;
    out.texCoords = in.texCoords;
    out.color = in.color;
    return out;
}

fragment half4 fragmentMain(VertexOut in [[stage_in]],
                           texture2d<half, access::sample> uiTexture [[texture(0)]]) {
    constexpr sampler linearSampler(coord::normalized,
                                    address::clamp_to_edge,
                                    filter::linear);
    
    // Check if texture is valid (non-null texture will have valid data)
    // In Metal, we can't easily check if texture is null, so we'll always sample
    // and use alpha to determine if texture should be used
    // For now, always use texture if provided (RmlUI will provide a white texture if none)
    half4 texColor = uiTexture.sample(linearSampler, in.texCoords);
    return half4(in.color) * texColor;
}

