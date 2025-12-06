#include <metal_stdlib>
using namespace metal;

// Tilt-Shift DOF - High quality bokeh blur pass
// Uses hexagonal sampling pattern for cinematic bokeh

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

struct DOFBlurParams {
    float2 texelSize;       // 1.0 / textureSize
    float blurScale;        // Overall blur scale multiplier
    int sampleCount;        // Number of blur samples (quality)
};

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]]) {
    RasterizerData vert;
    vert.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    vert.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    vert.uv.y = 1.0 - vert.uv.y;
    return vert;
}

// Golden angle for spiral sampling (more uniform than random)
constant float GOLDEN_ANGLE = 2.39996323;

// Hexagonal bokeh kernel offsets
constant float2 hexOffsets[6] = {
    float2( 1.0,  0.0),
    float2( 0.5,  0.866),
    float2(-0.5,  0.866),
    float2(-1.0,  0.0),
    float2(-0.5, -0.866),
    float2( 0.5, -0.866)
};

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texColorCoC [[texture(0)]],
    constant DOFBlurParams& params [[buffer(0)]]
) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);

    float4 centerSample = texColorCoC.sample(s, in.uv);
    float3 color = centerSample.rgb;
    float coc = centerSample.a;

    // Early out if no blur needed
    if (coc < 0.001) {
        return float4(color, 1.0);
    }

    float3 accumColor = float3(0.0);
    float accumWeight = 0.0;

    // Blur radius in pixels
    float blurRadius = coc * params.blurScale * 32.0; // Max 32 pixels blur

    // High quality: Use spiral sampling with golden angle
    int samples = max(8, min(params.sampleCount, 64));

    for (int i = 0; i < samples; i++) {
        float t = float(i) / float(samples);
        float radius = sqrt(t) * blurRadius; // Square root for uniform disk distribution
        float angle = float(i) * GOLDEN_ANGLE;

        float2 offset = float2(cos(angle), sin(angle)) * radius * params.texelSize;
        float4 sampleData = texColorCoC.sample(s, in.uv + offset);

        // Weight by sample's CoC to prevent sharp objects bleeding into blur
        float sampleCoC = sampleData.a;
        float weight = smoothstep(0.0, 1.0, sampleCoC);

        // Also weight by distance for smoother falloff
        weight *= 1.0 - t * 0.5;

        accumColor += sampleData.rgb * weight;
        accumWeight += weight;
    }

    // Add hexagonal samples for bokeh shape
    for (int i = 0; i < 6; i++) {
        float2 offset = hexOffsets[i] * blurRadius * 0.7 * params.texelSize;
        float4 sampleData = texColorCoC.sample(s, in.uv + offset);
        float weight = smoothstep(0.0, 1.0, sampleData.a) * 0.5;

        accumColor += sampleData.rgb * weight;
        accumWeight += weight;
    }

    if (accumWeight > 0.0) {
        color = accumColor / accumWeight;
    }

    return float4(color, coc);
}
