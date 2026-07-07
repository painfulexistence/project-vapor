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

// Must match Vapor::PostProcessParams (std430; 11 floats).
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

void main() {
    vec2 uv = tex_uv;
    vec2 toCenter = uv - vec2(0.5);
    float distFromCenter = length(toCenter);

    // Chromatic aberration: R/B channels sampled with an edge-growing offset
    // (the sun-scattering god rays get the same split, like the Metal pass).
    float caAmount = pow(distFromCenter, chromaticAberrationFalloff) * chromaticAberrationStrength;
    vec2 caDir = normalize(toCenter + 0.0001);
    vec2 uvR = uv + caDir * caAmount;
    vec2 uvB = uv - caDir * caAmount;

    vec3 color;
    color.r = texture(texScreen, uvR).r + texture(texGodRays, uvR).r;
    color.g = texture(texScreen, uv ).g + texture(texGodRays, uv ).g;
    color.b = texture(texScreen, uvB).b + texture(texGodRays, uvB).b;

    // Additive bloom (composited here on the Vulkan path).
    color += texture(texBloom, uv).rgb * 0.8;

    // HDR color grading (before tone mapping).
    color *= exposure;
    color = adjustTemperature(color, temperature);
    color = adjustTint(color, tint);
    color += brightness;

    // Tone map HDR -> LDR.
    color = aces(color);

    // LDR grading.
    color = adjustSaturation(color, saturation);
    color = adjustContrast(color, contrast);

    // Vignette.
    float vignette = smoothstep(vignetteRadius, vignetteRadius - vignetteSoftness, distFromCenter);
    vignette = mix(1.0, vignette, vignetteStrength);
    color *= vignette;

    Color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
