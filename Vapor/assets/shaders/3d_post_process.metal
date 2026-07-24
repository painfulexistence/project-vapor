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

    // Film grain (independent of VHS)
    float enableFilmGrain;
    float filmGrainStrength;
    float filmGrainAnimated;

    // TV signal glitch
    float enableGlitch;
    float glitchIntensity;
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

// ─── Stylized-effect helpers ────────────────────────────────────────────────

// Hash noise in [0,1). Classic sin-hash: sin() bounds the value before the
// large multiply, so it stays well-conditioned for big pixel-coordinate inputs
// (film grain) unlike a fract(p*k) hash, which bands at high resolution.
static inline float hash21(float2 p) {
    return fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

// Smooth 1D value noise built from the hash (used by the VHS emulation).
static inline float vhsSmoothNoise(float x, float seed) {
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash21(float2(i, seed)), hash21(float2(i + 1.0, seed)), f);
}

// Per-scanline horizontal jitter: two sway frequencies plus a random per-band
// drift, giving the unstable-head wobble.
static inline float vhsLineJitter(float y, float t) {
    return sin(y * 11.0  - t * 2.3) * 0.0035
         + sin(y * 140.0 + t * 7.0) * 0.0012
         + (vhsSmoothNoise(y * 9.0 + t * 0.8, 17.0) - 0.5) * 0.02;
}

// Scrolling tape dropout: a soft band sweeping down the frame, broken by
// speckle so it reads as signal loss rather than a clean bar.
static inline float vhsDropout(float2 uv, float t) {
    float sweep = fract(t * 0.19 + vhsSmoothNoise(t, 5.0) * 0.3);
    float band  = smoothstep(0.05, 0.0, abs(uv.y - sweep));
    float speckle = step(0.55, hash21(float2(floor(uv.x * 120.0), floor(uv.y * 240.0) + floor(t * 24.0))));
    return band * speckle;
}

// TV signal glitch: bursty blocky horizontal displacement + RGB tearing.
// Returns (horizontal uv shift, extra RGB channel split). Runs before sampling
// so it stacks with the VHS jitter and the CRT barrel. Scales with intensity.
static inline float2 tvGlitch(float y, float t, float intensity) {
    // Storm envelope — glitches arrive in occasional bursts, quiet between.
    float storm = smoothstep(0.55, 0.9, vhsSmoothNoise(t * 1.3, 21.0));
    float tick  = floor(t * 12.0);
    // Coarse blocks that jump left/right (only a sparse subset is active).
    float blk    = floor(y * 20.0);
    float hit    = step(0.80, hash21(float2(blk, tick))) * storm;
    float coarse = (hash21(float2(blk, tick + 7.0)) - 0.5) * 0.12 * hit;
    // Fine scanline-scale tearing during storms.
    float fine   = (hash21(float2(floor(y * 220.0), tick)) - 0.5) * 0.02 * storm;
    float shift  = (coarse + fine) * intensity;
    float split  = (hit + storm * 0.3) * 0.015 * intensity;
    return float2(shift, split);
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
    // VHS (original implementation): per-line horizontal jitter warps the uv
    // before sampling; the tape artifacts (scanlines, dropout, hiss, chroma
    // bleed, desat, vignette) are gathered here and composited post-tonemap.
    // ========================================================================
    float  vhsScan   = 1.0;            // scanline brightness modulation
    float3 vhsAdd    = float3(0.0);    // additive tape artifacts (dropout/hiss)
    float  vhsVig    = 1.0;            // soft edge vignette
    float  vhsDesat  = 0.0;            // desaturation amount
    float  vhsChroma = 0.0;            // horizontal chroma-bleed offset
    if (params.enableVHS > 0.5) {
        float t = params.time;
        uv.x += vhsLineJitter(uv.y, t);
        vhsScan = 0.88 + 0.12 * sin(uv.y * 720.0);
        float drop = vhsDropout(uv, t);
        // Fine tape "snow": ~2px cells horizontally (real VHS luma noise streaks
        // along the scanline), per-scanline vertically, reseeded each frame —
        // finer and more authentic than a coarse square-cell hash.
        float2 px = uv * float2(texScreen.get_width(), texScreen.get_height());
        float hiss = hash21(float2(floor(px.x * 0.5), floor(px.y)) + floor(t * 60.0)) - 0.5;
        // Head-switching noise stays coarse — that band really is blocky on tape.
        float headSwitch = smoothstep(0.05, 0.0, uv.y) *
                           (hash21(float2(uv.x * 200.0, floor(t * 30.0))) - 0.2);
        vhsAdd = float3(drop * 0.5 + hiss * 0.10 + headSwitch * 0.6);
        vhsDesat = 0.15;
        float2 vd = uv - 0.5;
        vhsVig = 1.0 - dot(vd, vd) * 0.5;
        vhsChroma = 0.004;
    }

    // ========================================================================
    // TV signal glitch: blocky horizontal displacement + RGB tearing. Warps uv
    // here (after VHS, before CRT) so it stacks with both.
    // ========================================================================
    float glitchSplit = 0.0;
    if (params.enableGlitch > 0.5) {
        float2 gd = tvGlitch(uv.y, params.time, params.glitchIntensity);
        uv.x += gd.x;
        glitchSplit = gd.y;
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
    // VHS chroma bleed: a purely horizontal R/B smear (low chroma bandwidth).
    if (params.enableVHS > 0.5) { rOff.x += vhsChroma; bOff.x -= vhsChroma; }
    // TV-glitch RGB tearing: stacks on top of the CA/CRT/VHS channel offsets.
    if (params.enableGlitch > 0.5) { rOff.x += glitchSplit; bOff.x -= glitchSplit; }

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

    // VHS overlays (LDR): desaturate, scanlines, additive tape artifacts, edge
    // vignette. All factors are neutral (no-ops) when VHS is disabled.
    float vhsLum = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = mix(color, float3(vhsLum), vhsDesat);
    color *= vhsScan;
    color += vhsAdd;
    color *= vhsVig;
    if (params.enableCRT > 0.5) color += sin(uv.y * 800.0 + params.time * 10.0) * 0.04;

    // Film grain (LDR, screen-space). Static by default; Animated reseeds each
    // frame for a flickering grain. Uses the undistorted screen uv (in.uv).
    if (params.enableFilmGrain > 0.5) {
        float2 gseed = in.uv * float2(texScreen.get_width(), texScreen.get_height());
        if (params.filmGrainAnimated > 0.5) gseed += float2(params.time * 91.7, params.time * 47.3);
        float g = hash21(gseed) - 0.5;
        color += g * params.filmGrainStrength;
    }

    // Ensure color is in valid range
    color = saturate(color);

    return offScreen ? float4(0.0, 0.0, 0.0, 1.0) : float4(color, 1.0);
}
