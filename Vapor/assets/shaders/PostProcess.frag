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

// ─── Stylized-effect helpers (ported from Atmospheric post_composite.frag) ───

float onOff(float a, float b, float c, float t) {
    return step(c, sin(t + a * cos(t * b)));
}

vec2 vhsScreenDistort(vec2 uv) {
    uv -= 0.5;
    uv = uv * 1.2 * (1.0 / 1.2 + 2.0 * uv.x * uv.x * uv.y * uv.y);
    return uv + 0.5;
}

vec2 vhsTracking(vec2 uv, float t) {
    vec2 look = uv;
    float wy = look.y - mod(t / 4.0, 1.0);
    float window = 1.0 / (1.0 + 20.0 * wy * wy);
    look.x += sin(look.y * 10.0 + t) / 50.0 * onOff(4.0, 4.0, 0.3, t) * (1.0 + cos(t * 80.0)) * window;
    float vShift = 0.4 * onOff(2.0, 3.0, 0.9, t) *
                   (sin(t) * sin(t * 20.0) + (0.5 + 0.1 * sin(t * 200.0) * cos(t)));
    look.y = mod(look.y + vShift, 1.0);
    return look;
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

    // VHS: screen curvature + tracking wobble (Atmospheric port). The
    // vignette/stripe factors come from the curved uv and apply post-tonemap.
    float vhsVignette = 1.0;
    float vhsStripe   = 1.0;
    if (enableVHS > 0.5) {
        uv = vhsScreenDistort(uv);
        float vigAmt = 3.0 + 0.3 * sin(time + 5.0 * cos(time * 5.0));
        vhsVignette = (1.0 - vigAmt * (uv.y - 0.5) * (uv.y - 0.5)) *
                      (1.0 - vigAmt * (uv.x - 0.5) * (uv.x - 0.5));
        vhsStripe   = (12.0 + mod(uv.y * 30.0 + time, 1.0)) / 13.0;
        uv = vhsTracking(uv, time);
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

    // VHS overlays + CRT scanlines (final stylized layer).
    color *= vhsVignette * vhsStripe;
    if (enableCRT > 0.5) color += sin(uv.y * 800.0 + time * 10.0) * 0.04;

    color = clamp(color, 0.0, 1.0);
    Color = offScreen ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(color, 1.0);
}
