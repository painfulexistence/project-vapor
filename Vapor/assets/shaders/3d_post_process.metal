#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal" // TODO: use more robust include path

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

// Post-processing parameters (must match C++ struct)
struct PostProcessParams {
    // Chromatic Aberration
    float chromaticAberrationStrength;
    float chromaticAberrationFalloff;

    // Vignette
    float vignetteStrength;
    float vignetteRadius;
    float vignetteSoftness;

    // Color Grading
    float saturation;
    float contrast;
    float brightness;
    float temperature;
    float tint;

    // Tone Mapping
    float exposure;
};

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]]) {
    RasterizerData vert;
    vert.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    vert.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    vert.uv.y = 1.0 - vert.uv.y; // flip Y axis because Metal uses Y-down UV space
    return vert;
}

// ACES Filmic Tone Mapping
float3 aces(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Color temperature adjustment (approximation)
float3 adjustTemperature(float3 color, float temperature) {
    // Warm (positive) adds red/yellow, cool (negative) adds blue
    float3 warm = float3(1.0 + temperature * 0.1, 1.0, 1.0 - temperature * 0.1);
    return color * warm;
}

// Tint adjustment (green-magenta axis)
float3 adjustTint(float3 color, float tint) {
    float3 tintColor = float3(1.0 + tint * 0.05, 1.0 - abs(tint) * 0.05, 1.0 - tint * 0.05);
    return color * tintColor;
}

// Saturation adjustment
float3 adjustSaturation(float3 color, float saturation) {
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    return mix(float3(luminance), color, saturation);
}

// Contrast adjustment
float3 adjustContrast(float3 color, float contrast) {
    return (color - 0.5) * contrast + 0.5;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texScreen [[texture(0)]],
    texture2d<float, access::sample> texAO [[texture(1)]],
    texture2d<float, access::sample> texNormal [[texture(2)]],
    texture2d<float, access::sample> texGodRays [[texture(3)]],
    constant PostProcessParams& params [[buffer(0)]]
) {
    constexpr sampler s(address::clamp_to_edge, filter::linear, mip_filter::linear);

    float2 uv = in.uv;
    float2 center = float2(0.5, 0.5);
    float2 toCenter = uv - center;
    float distFromCenter = length(toCenter);

    // ========================================================================
    // Chromatic Aberration & God Rays
    // ========================================================================
    float caStrength = params.chromaticAberrationStrength;
    float caFalloff = params.chromaticAberrationFalloff;

    // Chromatic aberration increases toward edges
    float caAmount = pow(distFromCenter, caFalloff) * caStrength;
    float2 caDirection = normalize(toCenter + 0.0001); // Direction from center

    // Sample RGB channels with offset
    float2 uvR = uv + caDirection * caAmount;
    float2 uvB = uv - caDirection * caAmount;

    float3 color;
    color.r = texScreen.sample(s, uvR).r + texGodRays.sample(s, uvR).r;
    color.g = texScreen.sample(s, uv).g + texGodRays.sample(s, uv).g;
    color.b = texScreen.sample(s, uvB).b + texGodRays.sample(s, uvB).b;

    // Get AO
    float ao = texAO.sample(s, uv).r;

    // ========================================================================
    // Color Grading (in HDR space, before tone mapping)
    // ========================================================================

    // Apply exposure
    color *= params.exposure;

    // Temperature and tint
    color = adjustTemperature(color, params.temperature);
    color = adjustTint(color, params.tint);

    // Brightness (additive)
    color += params.brightness;

    // ========================================================================
    // Tone Mapping (HDR -> LDR)
    // ========================================================================
    color = aces(color);

    // ========================================================================
    // Post Tone Mapping Effects (in LDR space)
    // ========================================================================

    // Saturation (better in LDR space)
    color = adjustSaturation(color, params.saturation);

    // Contrast (better in LDR space)
    color = adjustContrast(color, params.contrast);

    // ========================================================================
    // Vignette (final effect)
    // ========================================================================
    float vignetteRadius = params.vignetteRadius;
    float vignetteSoftness = params.vignetteSoftness;
    float vignetteStrength = params.vignetteStrength;

    float vignette = smoothstep(vignetteRadius, vignetteRadius - vignetteSoftness, distFromCenter);
    vignette = mix(1.0, vignette, vignetteStrength);
    color *= vignette;

    // Ensure color is in valid range
    color = saturate(color);

    return float4(color, 1.0);
}
