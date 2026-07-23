#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

// Post-processing parameters (must match Vapor::Renderer::PostProcessParams —
// same field order, all floats). Appended fields are the per-effect enable
// flags (1=on, 0=off) and the stylized-effect params.
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

    // Per-effect enable flags
    float enableChromaticAberration;
    float enableVignette;
    float enableColorGrading;
    float enableToneMapping;

    // Stylized effects (ported from Atmospheric), default off
    float enableVHS;
    float enableCRT;
    float enableSobel;
    float enablePosterize;
    float posterizeLevels;
    float time;
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

// ─── Stylized-effect helpers (ported from Atmospheric post_composite.frag) ───

// GLSL-style mod: matches glsl mod() for negative operands (fmod does not).
static inline float glmod(float x, float y) { return x - y * floor(x / y); }

static inline float onOff(float a, float b, float c, float t) {
    return step(c, sin(t + a * cos(t * b)));
}

static inline float2 vhsScreenDistort(float2 uv) {
    uv -= 0.5;
    uv = uv * 1.2 * (1.0 / 1.2 + 2.0 * uv.x * uv.x * uv.y * uv.y);
    return uv + 0.5;
}

static inline float2 vhsTracking(float2 uv, float t) {
    float2 look = uv;
    float wy = look.y - glmod(t / 4.0, 1.0);
    float window = 1.0 / (1.0 + 20.0 * wy * wy);
    look.x += sin(look.y * 10.0 + t) / 50.0 * onOff(4.0, 4.0, 0.3, t) * (1.0 + cos(t * 80.0)) * window;
    float vShift = 0.4 * onOff(2.0, 3.0, 0.9, t) *
                   (sin(t) * sin(t * 20.0) + (0.5 + 0.1 * sin(t * 200.0) * cos(t)));
    look.y = glmod(look.y + vShift, 1.0);
    return look;
}

// 3x3 Sobel gradient magnitude of the screen texture's luminance.
static inline float sobelMagnitude(texture2d<float, access::sample> tex, sampler s, float2 uv) {
    float2 ts = 1.0 / float2(tex.get_width(), tex.get_height());
    float tl = length(tex.sample(s, uv + float2(-ts.x,  ts.y)).rgb);
    float tp = length(tex.sample(s, uv + float2( 0.0,   ts.y)).rgb);
    float tr = length(tex.sample(s, uv + float2( ts.x,  ts.y)).rgb);
    float l  = length(tex.sample(s, uv + float2(-ts.x,  0.0 )).rgb);
    float r  = length(tex.sample(s, uv + float2( ts.x,  0.0 )).rgb);
    float bl = length(tex.sample(s, uv + float2(-ts.x, -ts.y)).rgb);
    float bt = length(tex.sample(s, uv + float2( 0.0,  -ts.y)).rgb);
    float br = length(tex.sample(s, uv + float2( ts.x, -ts.y)).rgb);
    float gx = -tl + tr - 2.0 * l + 2.0 * r - bl + br;
    float gy = -tl - 2.0 * tp - tr + bl + 2.0 * bt + br;
    return sqrt(gx * gx + gy * gy);
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

    // ========================================================================
    // VHS: screen curvature + tracking wobble (Atmospheric port). The
    // vignette/stripe factors come from the curved uv and apply post-tonemap.
    // ========================================================================
    float vhsVignette = 1.0;
    float vhsStripe   = 1.0;
    if (params.enableVHS > 0.5) {
        uv = vhsScreenDistort(uv);
        float vigAmt = 3.0 + 0.3 * sin(params.time + 5.0 * cos(params.time * 5.0));
        vhsVignette = (1.0 - vigAmt * (uv.y - 0.5) * (uv.y - 0.5)) *
                      (1.0 - vigAmt * (uv.x - 0.5) * (uv.x - 0.5));
        vhsStripe   = (12.0 + glmod(uv.y * 30.0 + params.time, 1.0)) / 13.0;
        uv = vhsTracking(uv, params.time);
    }

    // ========================================================================
    // CRT: barrel distortion; samples pushed off-screen render black.
    // ========================================================================
    bool offScreen = false;
    if (params.enableCRT > 0.5) {
        float2 c = (uv - 0.5) * 2.0;
        float2 crtOff = abs(c.yx) * float2(0.2, 0.25);
        c += c * crtOff * crtOff;
        uv = c * 0.5 + 0.5;
        offScreen = uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;
    }

    float2 center = float2(0.5, 0.5);
    float2 toCenter = uv - center;
    float distFromCenter = length(toCenter);

    // ========================================================================
    // Chromatic Aberration (+ CRT phosphor split) & God Rays
    // ========================================================================
    float caAmount = (params.enableChromaticAberration > 0.5)
        ? pow(distFromCenter, params.chromaticAberrationFalloff) * params.chromaticAberrationStrength
        : 0.0;
    float2 caDirection = normalize(toCenter + 0.0001); // Direction from center
    float2 rOff = caDirection * caAmount;
    float2 bOff = -caDirection * caAmount;
    if (params.enableCRT > 0.5) { rOff += float2(0.001, 0.0); bOff += float2(-0.001, 0.0); }

    // Sample RGB channels with offset
    float2 uvR = uv + rOff;
    float2 uvB = uv + bOff;

    float3 color;
    color.r = texScreen.sample(s, uvR).r + texGodRays.sample(s, uvR).r;
    color.g = texScreen.sample(s, uv).g + texGodRays.sample(s, uv).g;
    color.b = texScreen.sample(s, uvB).b + texGodRays.sample(s, uvB).b;

    // (Bloom is composited into colorRT by the BloomComposite pass before this
    // shader on the Metal path, so no bloom sample here. AO is applied to the
    // ambient/IBL term in lighting — a whole-image multiply would darken direct
    // light too.)

    // ========================================================================
    // Sobel edge overlay (HDR domain; bright pixels suppress the overlay)
    // ========================================================================
    if (params.enableSobel > 0.5) {
        float lum = length(color * params.exposure);
        float edgeStrength = 1.0 - smoothstep(2.0, 4.0, lum);
        float edge = smoothstep(0.1, 0.5, sobelMagnitude(texScreen, s, uv));
        color = mix(color, float3(0.1), edge * edgeStrength);
    }

    // ========================================================================
    // Color Grading (HDR) + Tone Mapping (HDR -> LDR)
    // ========================================================================
    if (params.enableToneMapping > 0.5) color *= params.exposure;
    if (params.enableColorGrading > 0.5) {
        color = adjustTemperature(color, params.temperature);
        color = adjustTint(color, params.tint);
        color += params.brightness;
    }
    if (params.enableToneMapping > 0.5) color = aces(color);
    if (params.enableColorGrading > 0.5) {
        color = adjustSaturation(color, params.saturation);
        color = adjustContrast(color, params.contrast);
    }

    // ========================================================================
    // Posterize (Atmospheric port; quantize LDR into N steps)
    // ========================================================================
    if (params.enablePosterize > 0.5) {
        float lv = max(params.posterizeLevels, 1.0);
        color = floor(color * lv) / lv;
    }

    // ========================================================================
    // Vignette
    // ========================================================================
    if (params.enableVignette > 0.5) {
        float vignette = smoothstep(params.vignetteRadius, params.vignetteRadius - params.vignetteSoftness, distFromCenter);
        vignette = mix(1.0, vignette, params.vignetteStrength);
        color *= vignette;
    }

    // VHS overlays + CRT scanlines (final stylized layer)
    color *= vhsVignette * vhsStripe;
    if (params.enableCRT > 0.5) color += sin(uv.y * 800.0 + params.time * 10.0) * 0.04;

    // Ensure color is in valid range
    color = saturate(color);

    return offScreen ? float4(0.0, 0.0, 0.0, 1.0) : float4(color, 1.0);
}
