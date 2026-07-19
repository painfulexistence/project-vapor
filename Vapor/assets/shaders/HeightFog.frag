#version 450
// Cheap analytic exponential height fog (the pre-raymarch "Height Fog"): a
// single evaluation per pixel — no raymarch, no shadows, no per-light loops.
// Reconstructs world position from depth, applies a distance/height fog opacity
// and mixes toward a sun-tinted ambient fog color. The expensive per-light
// volumetric variant lives in VolumetricFog.frag (now ECS-driven).
// Uses FullScreen.vert (tex_uv in [0,1]).

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;

// vec4-packed so std430 matches the C++ HeightFogRenderData / the MSL twin.
struct HeightFogData {
    mat4 invViewProj;
    vec4 cameraPosition;     // xyz
    vec4 sunDirection;       // xyz
    vec4 sunColorIntensity;  // rgb + a=intensity
    vec4 fogColorDensity;    // rgb + a=density
    vec4 params;             // x=falloff y=baseHeight z=anisotropy w=ambient
};
layout(std430, set = 1, binding = 0) readonly buffer FogBuf { HeightFogData d; };

// Henyey-Greenstein phase (matches the Metal phaseHenyeyGreenstein).
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

// Analytic exponential height fog opacity (twin of exponentialHeightFog).
float exponentialHeightFog(float dist, float height, float density, float falloffIn) {
    float falloff = max(falloffIn, 0.0001);
    float heightFactor = exp(-max(0.0, height) * falloff);
    float distFactor = (1.0 - exp(-dist * falloff)) / falloff;
    float fogAmount = density * heightFactor * distFactor;
    return 1.0 - exp(-fogAmount);   // Beer-Lambert opacity
}

void main() {
    vec4 color = texture(sceneColor, tex_uv);
    float depth = texture(sceneDepth, tex_uv).r;
    // Skip sky pixels (fogging infinity needs a far-plane march — a follow-up).
    if (depth >= 0.9999) { outColor = color; return; }

    // Reconstruct world position (same convention as VolumetricFog.frag).
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 clip = vec4(ndc, depth, 1.0);
    vec4 worldH = d.invViewProj * clip;
    vec3 worldPos = worldH.xyz / worldH.w;

    vec3 camPos = d.cameraPosition.xyz;
    vec3 rayDir = normalize(worldPos - camPos);
    float dist = length(worldPos - camPos);
    float height = worldPos.y - d.params.y;   // relative to base height

    float fogAmount = clamp(exponentialHeightFog(dist, height, d.fogColorDensity.a, d.params.x), 0.0, 1.0);

    float phase = phaseHG(dot(rayDir, normalize(d.sunDirection.xyz)), d.params.z);
    vec3 fogColor = d.sunColorIntensity.rgb * d.sunColorIntensity.a * phase * 0.1
                  + d.fogColorDensity.rgb * d.params.w;

    outColor = vec4(mix(color.rgb, fogColor, fogAmount), color.a);
}
