#version 450
// Simple screen-space height/distance fog. GLSL port of the Metal
// simpleFogFragment (3d_volumetric_fog.metal): reconstruct world position from
// depth, apply exponential height fog with a Henyey-Greenstein sun phase.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;

// Must match Vapor::FogRenderData (std430).
layout(std430, set = 1, binding = 0) readonly buffer FogBuf {
    mat4 invViewProj;
    vec3 cameraPosition; float _p0;
    vec3 sunDirection;   float _p1;
    vec3 sunColor;       float sunIntensity;
    float fogDensity;
    float fogHeightFalloff;
    float anisotropy;
    float ambientIntensity;
};

const float INV_4PI = 0.07957747;

float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float d = 1.0 + g2 - 2.0 * g * cosTheta;
    return INV_4PI * (1.0 - g2) / pow(d, 1.5);
}

float exponentialHeightFog(float dist, float height, float density, float falloffIn) {
    float falloff = max(falloffIn, 0.0001);
    float heightFactor = exp(-max(0.0, height) * falloff);
    float distFactor = (1.0 - exp(-dist * falloff)) / falloff;
    float amt = density * heightFactor * distFactor;
    return 1.0 - exp(-amt);
}

void main() {
    vec4 color = texture(sceneColor, tex_uv);
    float depth = texture(sceneDepth, tex_uv).r;

    if (depth >= 0.9999) { outColor = color; return; }  // sky: no fog

    // Reconstruct world position (GL-convention +Y-up NDC, Vulkan ZO depth).
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 wp = invViewProj * vec4(ndc, depth, 1.0);
    vec3 worldPos = wp.xyz / wp.w;

    vec3 rayDir = normalize(worldPos - cameraPosition);
    float dist = length(worldPos - cameraPosition);
    float height = worldPos.y;

    float fogAmount = clamp(exponentialHeightFog(dist, height, fogDensity, fogHeightFalloff), 0.0, 1.0);

    float cosTheta = dot(rayDir, sunDirection);
    float phase = phaseHG(cosTheta, anisotropy);
    vec3 fogColor = sunColor * sunIntensity * phase * 0.1 + vec3(0.5, 0.6, 0.7) * ambientIntensity;

    outColor = vec4(mix(color.rgb, fogColor, fogAmount), color.a);
}
