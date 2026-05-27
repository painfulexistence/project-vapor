#include <metal_stdlib>
using namespace metal;

// Tilt-Shift DOF - Composite pass
// Blends sharp and blurred images based on CoC

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
    vert.uv.y = 1.0 - vert.uv.y;
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texSharp [[texture(0)]],   // Original sharp image
    texture2d<float, access::sample> texBlurred [[texture(1)]], // Blurred image with CoC
    constant float& blendSharpness [[buffer(0)]]                // How sharp the transition is
) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);

    float3 sharpColor = texSharp.sample(s, in.uv).rgb;
    float4 blurredData = texBlurred.sample(s, in.uv);
    float3 blurredColor = blurredData.rgb;
    float coc = blurredData.a;

    // Blend based on CoC with adjustable sharpness
    float blendFactor = smoothstep(0.0, blendSharpness, coc);

    float3 finalColor = mix(sharpColor, blurredColor, blendFactor);

    return float4(finalColor, 1.0);
}
