#version 450
// Full post-process to swapchain: chromatic aberration, bloom + god-ray
// composite, HDR color grading, ACES tone map, saturation/contrast, vignette.
// GLSL twin of 3d_post_process.metal — same operations, same order, driven by
// the shared Vapor::PostProcessParams. (Metal composites bloom in a prior pass;
// here texScreen is the raw HDR scene and bloom is added below.)

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 Color;

layout(set = 2, binding = 0) uniform sampler2D texScreen;
layout(set = 2, binding = 1) uniform sampler2D texBloom;    // accumulated bloom pyramid[0]
layout(set = 2, binding = 2) uniform sampler2D texGodRays;  // half-res light scattering

// Must match Vapor::PostProcessParams (std430; 21 floats, append-only).
layout(std430, set = 1, binding = 0) readonly buffer PostBuf {
    float chromaticAberrationStrength;
    float chromaticAberrationFalloff;
    float vignetteStrength;
    float vignetteRadius;
    float vignetteSoftness;
    float saturation;
    float contrast;
    float brightness;
    float temperature;
    float tint;
    float exposure;
    // Per-effect enable flags (1=on, 0=off).
    float enableChromaticAberration;
    float enableVignette;
    float enableColorGrading;
    float enableToneMapping;
    // Stylized effects ported from Atmospheric (default off).
    float enableVHS;
    float enableCRT;
    float enableSobel;
    float enablePosterize;
    float posterizeLevels;
    float time;
    // Film grain (independent of VHS).
    float enableFilmGrain;
    float filmGrainStrength;
    float filmGrainAnimated;
    // TV signal glitch.
    float enableGlitch;
    float glitchIntensity;
};

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 adjustTemperature(vec3 col, float t) {
    return col * vec3(1.0 + t * 0.1, 1.0, 1.0 - t * 0.1);
}
vec3 adjustTint(vec3 col, float t) {
    return col * vec3(1.0 + t * 0.05, 1.0 - abs(t) * 0.05, 1.0 - t * 0.05);
}
vec3 adjustSaturation(vec3 col, float s) {
    float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(l), col, s);
}
vec3 adjustContrast(vec3 col, float k) {
    return (col - 0.5) * k + 0.5;
}

// ─── Stylized-effect helpers ────────────────────────────────────────────────

// Hash noise in [0,1). Classic sin-hash: sin() bounds the value before the
// large multiply, so it stays well-conditioned for big pixel-coordinate inputs
// (film grain) unlike a fract(p*k) hash, which bands at high resolution.
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Smooth 1D value noise built from the hash (used by the VHS emulation).
float vhsSmoothNoise(float x, float seed) {
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash21(vec2(i, seed)), hash21(vec2(i + 1.0, seed)), f);
}

// Per-scanline horizontal jitter: two sway frequencies plus a random per-band
// drift, giving the unstable-head wobble.
float vhsLineJitter(float y, float t) {
    return sin(y * 11.0  - t * 2.3) * 0.0035
         + sin(y * 140.0 + t * 7.0) * 0.0012
         + (vhsSmoothNoise(y * 9.0 + t * 0.8, 17.0) - 0.5) * 0.02;
}

// Scrolling tape dropout: a soft band sweeping down the frame, broken by
// speckle so it reads as signal loss rather than a clean bar.
float vhsDropout(vec2 uv, float t) {
    float sweep = fract(t * 0.19 + vhsSmoothNoise(t, 5.0) * 0.3);
    float band  = smoothstep(0.05, 0.0, abs(uv.y - sweep));
    float speckle = step(0.55, hash21(vec2(floor(uv.x * 120.0), floor(uv.y * 240.0) + floor(t * 24.0))));
    return band * speckle;
}

// TV signal glitch: bursty blocky horizontal displacement + RGB tearing.
// Returns (horizontal uv shift, extra RGB channel split). Runs before sampling
// so it stacks with the VHS jitter and the CRT barrel. Scales with intensity.
vec2 tvGlitch(float y, float t, float intensity) {
    // Storm envelope — glitches arrive in occasional bursts, quiet between.
    float storm = smoothstep(0.55, 0.9, vhsSmoothNoise(t * 1.3, 21.0));
    float tick  = floor(t * 12.0);
    // Coarse blocks that jump left/right (only a sparse subset is active).
    float blk    = floor(y * 20.0);
    float hit    = step(0.80, hash21(vec2(blk, tick))) * storm;
    float coarse = (hash21(vec2(blk, tick + 7.0)) - 0.5) * 0.12 * hit;
    // Fine scanline-scale tearing during storms.
    float fine   = (hash21(vec2(floor(y * 220.0), tick)) - 0.5) * 0.02 * storm;
    float shift  = (coarse + fine) * intensity;
    float split  = (hit + storm * 0.3) * 0.015 * intensity;
    return vec2(shift, split);
}

// 3x3 Sobel gradient magnitude of the screen texture's luminance.
float sobelMagnitude(vec2 uv) {
    vec2 ts = 1.0 / vec2(textureSize(texScreen, 0));
    float tl = length(texture(texScreen, uv + vec2(-ts.x,  ts.y)).rgb);
    float tp = length(texture(texScreen, uv + vec2( 0.0,   ts.y)).rgb);
    float tr = length(texture(texScreen, uv + vec2( ts.x,  ts.y)).rgb);
    float l  = length(texture(texScreen, uv + vec2(-ts.x,  0.0 )).rgb);
    float r  = length(texture(texScreen, uv + vec2( ts.x,  0.0 )).rgb);
    float bl = length(texture(texScreen, uv + vec2(-ts.x, -ts.y)).rgb);
    float bt = length(texture(texScreen, uv + vec2( 0.0,  -ts.y)).rgb);
    float br = length(texture(texScreen, uv + vec2( ts.x, -ts.y)).rgb);
    float gx = -tl + tr - 2.0 * l + 2.0 * r - bl + br;
    float gy = -tl - 2.0 * tp - tr + bl + 2.0 * bt + br;
    return sqrt(gx * gx + gy * gy);
}

void main() {
    vec2 uv = tex_uv;

    // VHS (original implementation): per-line horizontal jitter warps the uv
    // before sampling; the tape artifacts (scanlines, dropout, hiss, chroma
    // bleed, desat, vignette) are gathered here and composited post-tonemap.
    float vhsScan   = 1.0;         // scanline brightness modulation
    vec3  vhsAdd    = vec3(0.0);   // additive tape artifacts (dropout/hiss)
    float vhsVig    = 1.0;         // soft edge vignette
    float vhsDesat  = 0.0;         // desaturation amount
    float vhsChroma = 0.0;         // horizontal chroma-bleed offset
    if (enableVHS > 0.5) {
        float t = time;
        uv.x += vhsLineJitter(uv.y, t);
        vhsScan = 0.88 + 0.12 * sin(uv.y * 720.0);
        float drop = vhsDropout(uv, t);
        // Fine tape "snow": ~2px cells horizontally (real VHS luma noise streaks
        // along the scanline), per-scanline vertically, reseeded each frame —
        // finer and more authentic than a coarse square-cell hash.
        vec2 px = uv * vec2(textureSize(texScreen, 0));
        float hiss = hash21(vec2(floor(px.x * 0.5), floor(px.y)) + floor(t * 60.0)) - 0.5;
        // Head-switching noise stays coarse — that band really is blocky on tape.
        float headSwitch = smoothstep(0.05, 0.0, uv.y) *
                           (hash21(vec2(uv.x * 200.0, floor(t * 30.0))) - 0.2);
        vhsAdd = vec3(drop * 0.5 + hiss * 0.10 + headSwitch * 0.6);
        vhsDesat = 0.15;
        vec2 vd = uv - 0.5;
        vhsVig = 1.0 - dot(vd, vd) * 0.5;
        vhsChroma = 0.004;
    }

    // TV signal glitch: blocky horizontal displacement + RGB tearing. Warps uv
    // here (after VHS, before CRT) so it stacks with both.
    float glitchSplit = 0.0;
    if (enableGlitch > 0.5) {
        vec2 gd = tvGlitch(uv.y, time, glitchIntensity);
        uv.x += gd.x;
        glitchSplit = gd.y;
    }

    // CRT: barrel distortion; samples pushed off-screen render black.
    bool offScreen = false;
    if (enableCRT > 0.5) {
        vec2 c = (uv - 0.5) * 2.0;
        vec2 crtOff = abs(c.yx) * vec2(0.2, 0.25);
        c += c * crtOff * crtOff;
        uv = c * 0.5 + 0.5;
        offScreen = uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;
    }

    vec2 toCenter = uv - vec2(0.5);
    float distFromCenter = length(toCenter);

    // Chromatic aberration (+ CRT phosphor split): R/B channels sampled with an
    // edge-growing offset (the sun-scattering god rays get the same split).
    float caAmount = (enableChromaticAberration > 0.5)
        ? pow(distFromCenter, chromaticAberrationFalloff) * chromaticAberrationStrength
        : 0.0;
    vec2 caDir = normalize(toCenter + 0.0001);
    vec2 rOff = caDir * caAmount;
    vec2 bOff = -caDir * caAmount;
    if (enableCRT > 0.5) { rOff += vec2(0.001, 0.0); bOff += vec2(-0.001, 0.0); }
    // VHS chroma bleed: a purely horizontal R/B smear (low chroma bandwidth).
    if (enableVHS > 0.5) { rOff.x += vhsChroma; bOff.x -= vhsChroma; }
    // TV-glitch RGB tearing: stacks on top of the CA/CRT/VHS channel offsets.
    if (enableGlitch > 0.5) { rOff.x += glitchSplit; bOff.x -= glitchSplit; }
    vec2 uvR = uv + rOff;
    vec2 uvB = uv + bOff;

    vec3 color;
    color.r = texture(texScreen, uvR).r + texture(texGodRays, uvR).r;
    color.g = texture(texScreen, uv ).g + texture(texGodRays, uv ).g;
    color.b = texture(texScreen, uvB).b + texture(texGodRays, uvB).b;

    // Additive bloom (composited here on the Vulkan path). texBloom is bound to
    // a black texture when bloom is disabled, so this adds nothing then.
    color += texture(texBloom, uv).rgb * 0.8;

    // Sobel edge overlay (HDR domain; bright pixels suppress the overlay).
    if (enableSobel > 0.5) {
        float lum = length(color * exposure);
        float edgeStrength = 1.0 - smoothstep(2.0, 4.0, lum);
        float edge = smoothstep(0.1, 0.5, sobelMagnitude(uv));
        color = mix(color, vec3(0.1), edge * edgeStrength);
    }

    // HDR color grading + tone mapping.
    if (enableToneMapping > 0.5) color *= exposure;
    if (enableColorGrading > 0.5) {
        color = adjustTemperature(color, temperature);
        color = adjustTint(color, tint);
        color += brightness;
    }
    if (enableToneMapping > 0.5) color = aces(color);
    if (enableColorGrading > 0.5) {
        color = adjustSaturation(color, saturation);
        color = adjustContrast(color, contrast);
    }

    // Posterize (Atmospheric port; quantize LDR into N steps).
    if (enablePosterize > 0.5) {
        float lv = max(posterizeLevels, 1.0);
        color = floor(color * lv) / lv;
    }

    // Vignette.
    if (enableVignette > 0.5) {
        float vignette = smoothstep(vignetteRadius, vignetteRadius - vignetteSoftness, distFromCenter);
        vignette = mix(1.0, vignette, vignetteStrength);
        color *= vignette;
    }

    // VHS overlays (LDR): desaturate, scanlines, additive tape artifacts, edge
    // vignette. All factors are neutral (no-ops) when VHS is disabled.
    float vhsLum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(color, vec3(vhsLum), vhsDesat);
    color *= vhsScan;
    color += vhsAdd;
    color *= vhsVig;
    if (enableCRT > 0.5) color += sin(uv.y * 800.0 + time * 10.0) * 0.04;

    // Film grain (LDR, screen-space). Static by default; Animated reseeds each
    // frame for a flickering grain. Uses the undistorted screen uv (tex_uv).
    if (enableFilmGrain > 0.5) {
        vec2 gseed = tex_uv * vec2(textureSize(texScreen, 0));
        if (filmGrainAnimated > 0.5) gseed += vec2(time * 91.7, time * 47.3);
        float g = hash21(gseed) - 0.5;
        color += g * filmGrainStrength;
    }

    color = clamp(color, 0.0, 1.0);
    Color = offScreen ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(color, 1.0);
}
