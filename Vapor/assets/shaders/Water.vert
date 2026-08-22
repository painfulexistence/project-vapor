#version 450
// RHI water surface — vertex stage. GLSL twin of 3d_water_rhi.metal.
//
// Displaces the pool grid by the FFT simulation output (WaterFFT* kernels):
// the assemble kernel mirrors the displacement field into a storage buffer,
// and this stage does a manual bilinear fetch from it (a plain SSBO read
// works identically on both backends; the RHI has no vertex-stage texture
// binding on Metal). uv1 spans the whole grid 0..1 and drives edge dampening
// so the surface stays glued to the pool walls.
//
// Vertices are pulled from a storage buffer (no vertex attributes): the CPU
// WaterVertexData is 7 tightly packed floats and std430 would pad a vec3
// member to 16 bytes, so the buffer is read as a raw float array.

layout(std430, set = 0, binding = 0) readonly buffer CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float near;
    float far;
    vec3 cameraPosition;
    float _camPad;
};

struct Wave {
    vec3 direction;
    float _wpad;
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

// Full CPU WaterData layout (graphics_effects.hpp) — keep in exact sync.
// The waves[] block is legacy-only; the RHI path displaces from the FFT.
layout(std430, set = 0, binding = 1) readonly buffer WaterBuf {
    mat4 modelMatrix;
    vec4 surfaceColor;
    vec4 refractionColor;
    vec4 ssrSettings;
    vec4 normalMapScroll;
    vec2 normalMapScrollSpeed;
    vec2 _pad1;
    float refractionDistortionFactor;
    float refractionHeightFactor;
    float refractionDistanceFactor;
    float depthSofteningDistance;
    float foamHeightStart;
    float foamFadeDistance;
    float foamTiling;
    float foamAngleExponent;
    float roughness;
    float reflectance;
    float specIntensity;
    float foamBrightness;
    Wave waves[4];
    uint waveCount;
    float dampeningFactor;
    float time;
    float _pad2;
    vec4 sunDirection;
    vec4 sunColorIntensity;
    vec4 causticsParams;
    vec4 causticsBoundsMin;
    vec4 causticsBoundsMax;
    mat4 reflectionViewProj;
    vec4 fftParams;         // x=patch size y=detail tiling z=detail strength w=displacement scale
    vec4 reflectionParams;  // x=planar strength y=distortion z=whitecap scale w=FFT resolution
} water;

// WaterVertexData: position.xyz, uv0.xy, uv1.xy — 7 floats, tightly packed.
layout(std430, set = 0, binding = 2) readonly buffer WaterVerts {
    float verts[];
};

// FFT displacement field (dx, height, dz, 0) per lattice cell.
layout(std430, set = 0, binding = 3) readonly buffer WaterDispBuf {
    vec4 disp[];
};

layout(location = 0) out vec4 vPositionView;
layout(location = 1) out vec4 vTexCoord0;      // xy: patch UV, zw: grid UV
layout(location = 2) out vec4 vScreenPosition;
layout(location = 3) out vec4 vPositionWorld;  // w: edge dampening
layout(location = 4) out vec4 vDispSample;     // xyz: sampled displacement

// Manual bilinear over the wrapped FFT lattice.
vec3 sampleDisplacement(vec2 patchUV) {
    uint n = uint(water.reflectionParams.w + 0.5);
    if (n == 0u) return vec3(0.0);
    vec2 f = fract(patchUV) * float(n) - 0.5;
    vec2 fl = floor(f);
    vec2 fr = f - fl;
    ivec2 i0 = ivec2(fl);
    uint x0 = uint((i0.x % int(n) + int(n)) % int(n));
    uint y0 = uint((i0.y % int(n) + int(n)) % int(n));
    uint x1 = (x0 + 1u) % n;
    uint y1 = (y0 + 1u) % n;
    vec3 d00 = disp[y0 * n + x0].xyz;
    vec3 d10 = disp[y0 * n + x1].xyz;
    vec3 d01 = disp[y1 * n + x0].xyz;
    vec3 d11 = disp[y1 * n + x1].xyz;
    return mix(mix(d00, d10, fr.x), mix(d01, d11, fr.x), fr.y);
}

void main() {
    uint vid = uint(gl_VertexIndex) * 7u;
    vec4 position = vec4(verts[vid + 0u], verts[vid + 1u], verts[vid + 2u], 1.0);
    vec4 texCoord0 = vec4(verts[vid + 3u], verts[vid + 4u], verts[vid + 5u], verts[vid + 6u]);

    // Edge dampening: 1 in the middle of the grid, 0 at the border.
    float dampening = 1.0 - pow(clamp(abs(texCoord0.z - 0.5) / 0.5, 0.0, 1.0), water.dampeningFactor);
    dampening *= 1.0 - pow(clamp(abs(texCoord0.w - 0.5) / 0.5, 0.0, 1.0), water.dampeningFactor);

    vec4 baseWorld = water.modelMatrix * position;
    vec2 patchUV = baseWorld.xz / max(water.fftParams.x, 1e-3);

    vec3 d = sampleDisplacement(patchUV) * (dampening * water.fftParams.w);

    vPositionWorld = vec4(baseWorld.xyz + d, dampening);
    vPositionView = view * vec4(vPositionWorld.xyz, 1.0);
    gl_Position = proj * vPositionView;
    vScreenPosition = gl_Position;
    vTexCoord0 = vec4(patchUV, texCoord0.zw);
    vDispSample = vec4(d, 0.0);
}
