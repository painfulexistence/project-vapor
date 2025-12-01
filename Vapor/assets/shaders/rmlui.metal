#include <metal_stdlib>
using namespace metal;

struct RmlUiUniforms {
    float4x4 projectionMatrix;
};

struct RmlUiVertexIn {
    float2 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
    float2 texCoord [[attribute(2)]];
};

struct RmlUiVertexOut {
    float4 position [[position]];
    float4 color;
    float2 texCoord;
};

// Vertex shader for RmlUi geometry
vertex RmlUiVertexOut rmlui_vertex_main(
    RmlUiVertexIn in [[stage_in]],
    constant RmlUiUniforms& uniforms [[buffer(1)]],
    constant float2& translation [[buffer(2)]]
) {
    RmlUiVertexOut out;
    float2 translatedPos = in.position + translation;
    out.position = uniforms.projectionMatrix * float4(translatedPos, 0.0, 1.0);
    out.color = in.color;
    out.texCoord = in.texCoord;
    return out;
}

// Fragment shader for RmlUi with texture
fragment half4 rmlui_fragment_textured(
    RmlUiVertexOut in [[stage_in]],
    texture2d<half, access::sample> tex [[texture(0)]]
) {
    constexpr sampler linearSampler(
        coord::normalized,
        min_filter::linear,
        mag_filter::linear,
        mip_filter::linear
    );
    half4 texColor = tex.sample(linearSampler, in.texCoord);
    return half4(in.color) * texColor;
}

// Fragment shader for RmlUi without texture (solid color)
fragment half4 rmlui_fragment_color(
    RmlUiVertexOut in [[stage_in]]
) {
    return half4(in.color);
}
