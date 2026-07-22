#version 450
// Froxel fog composite (Vulkan RHI). Samples the pre-integrated volume at the
// pixel's exponential depth slice and composites scene * transmittance +
// in-scatter. GLSL twin of volumetricFogFragment (3d_volumetric_fog.metal), using
// Vulkan ZO depth. Sky pixels (depth ~1) are skipped — the grid ends at the far
// plane, so there's no aerial perspective.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;
layout(set = 2, binding = 2) uniform sampler3D integratedVolume;

// Only nearPlane/farPlane are read; the full VolumetricFogData std430 layout is
// declared so those offsets line up.
layout(std430, set = 1, binding = 0) readonly buffer GlobalsBuf {
    mat4 invViewProj;
    mat4 prevViewProj;
    vec3 cameraPosition; float _gp1;
    vec3 sunDirection;   float _gp2;
    vec3 sunColor;       float _gp3;
    float sunIntensity;
    float fogDensity;
    float fogHeightFalloff;
    float fogBaseHeight;
    float fogMaxHeight;
    float scatteringCoeff;
    float extinctionCoeff;
    float anisotropy;
    float ambientIntensity;
    float nearPlane;
    float farPlane;
    uint  volumeCount;
    vec2  screenSize;
    vec2  _gp5;
    uint  frameIndex;
    float temporalBlend;
    float noiseScale;
    float noiseIntensity;
    float windSpeed;
    float gtime;
    vec2  _gp6;
    vec3  windDirection; float _gp7;
};

void main() {
    vec4 color = texture(sceneColor, tex_uv);
    float depth = texture(sceneDepth, tex_uv).r;
    if (depth >= 0.9999) { outColor = color; return; }  // sky

    // Planar view depth from Vulkan ZO depth, then the exponential slice mapping
    // that inverts the injection's froxel Z distribution.
    float linViewDepth = nearPlane * farPlane / (farPlane - depth * (farPlane - nearPlane));
    float slice = clamp(log(max(linViewDepth, nearPlane) / nearPlane) / log(farPlane / nearPlane),
                        0.0, 1.0);

    vec4 fog = texture(integratedVolume, vec3(tex_uv, slice));
    outColor = vec4(color.rgb * fog.a + fog.rgb, color.a);
}
