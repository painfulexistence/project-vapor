#version 450
// RHI water surface — vertex stage. GLSL twin of 3d_water_rhi.metal.
//
// Gerstner wave sum (GPU Gems ch.1 form, same math as the legacy
// 3d_water.metal) over a dual-UV grid built by MeshBuilder::buildWaterGrid:
// uv0 tiles the normal maps, uv1 spans the whole grid 0..1 and drives edge
// dampening so the surface lies flat where it meets the pool walls.
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
    float _wpad;   // std430 packs the next float into a vec3's tail; the CPU
                   // WaveData has an explicit pad here, so mirror it
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

// Full CPU WaterData layout (graphics_effects.hpp) — keep in exact sync.
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
} water;

// WaterVertexData: position.xyz, uv0.xy, uv1.xy — 7 floats, tightly packed.
layout(std430, set = 0, binding = 2) readonly buffer WaterVerts {
    float verts[];
};

layout(location = 0) out vec3 vNormalView;
layout(location = 1) out vec3 vTangentView;
layout(location = 2) out vec3 vBinormalView;
layout(location = 3) out vec4 vPositionView;
layout(location = 4) out vec4 vTexCoord0;      // xy: tiled UV, zw: grid UV
layout(location = 5) out vec4 vScreenPosition;
layout(location = 6) out vec4 vPositionWorld;
layout(location = 7) out vec4 vWorldNormalAndHeight;

struct WaveResult {
    vec3 position;
    vec3 normal;
    vec3 binormal;
    vec3 tangent;
};

WaveResult calculateWave(Wave wave, vec3 wavePosition, float edgeDampen, float time, uint numWaves) {
    WaveResult result;

    float frequency = 2.0 / wave.waveLength;
    float phaseConstant = wave.speed * frequency;
    float qi = wave.steepness / (wave.amplitude * frequency * float(numWaves));
    float rad = frequency * dot(wave.direction.xz, wavePosition.xz) + time * phaseConstant;
    float sinR = sin(rad);
    float cosR = cos(rad);

    result.position.x = wavePosition.x + qi * wave.amplitude * wave.direction.x * cosR * edgeDampen;
    result.position.z = wavePosition.z + qi * wave.amplitude * wave.direction.z * cosR * edgeDampen;
    result.position.y = wave.amplitude * sinR * edgeDampen;

    float waFactor = frequency * wave.amplitude;
    float radN = frequency * dot(wave.direction, result.position) + time * phaseConstant;
    float sinN = sin(radN);
    float cosN = cos(radN);

    result.binormal.x = 1.0 - (qi * wave.direction.x * wave.direction.x * waFactor * sinN);
    result.binormal.z = -1.0 * (qi * wave.direction.x * wave.direction.z * waFactor * sinN);
    result.binormal.y = wave.direction.x * waFactor * cosN;

    result.tangent.x = -1.0 * (qi * wave.direction.x * wave.direction.z * waFactor * sinN);
    result.tangent.z = 1.0 - (qi * wave.direction.z * wave.direction.z * waFactor * sinN);
    result.tangent.y = wave.direction.z * waFactor * cosN;

    result.normal.x = -1.0 * (wave.direction.x * waFactor * cosN);
    result.normal.z = -1.0 * (wave.direction.z * waFactor * cosN);
    result.normal.y = 1.0 - (qi * waFactor * sinN);

    result.binormal = normalize(result.binormal);
    result.tangent = normalize(result.tangent);
    result.normal = normalize(result.normal);

    return result;
}

void main() {
    uint vid = uint(gl_VertexIndex) * 7u;
    vec4 position = vec4(verts[vid + 0u], verts[vid + 1u], verts[vid + 2u], 1.0);
    vec4 texCoord0 = vec4(verts[vid + 3u], verts[vid + 4u], verts[vid + 5u], verts[vid + 6u]);

    // Edge dampening clamps wave motion where the grid meets its border.
    float dampening = 1.0 - pow(clamp(abs(texCoord0.z - 0.5) / 0.5, 0.0, 1.0), water.dampeningFactor);
    dampening *= 1.0 - pow(clamp(abs(texCoord0.w - 0.5) / 0.5, 0.0, 1.0), water.dampeningFactor);

    WaveResult finalWave;
    finalWave.position = vec3(0.0);
    finalWave.normal = vec3(0.0);
    finalWave.tangent = vec3(0.0);
    finalWave.binormal = vec3(0.0);

    uint numWaves = min(water.waveCount, 4u);
    for (uint waveId = 0u; waveId < numWaves; ++waveId) {
        WaveResult w = calculateWave(water.waves[waveId], position.xyz, dampening, water.time, numWaves);
        finalWave.position += w.position;
        finalWave.normal += w.normal;
        finalWave.tangent += w.tangent;
        finalWave.binormal += w.binormal;
    }

    if (numWaves > 0u) {
        finalWave.position -= position.xyz * float(numWaves - 1u);
        finalWave.normal = normalize(finalWave.normal);
        finalWave.tangent = normalize(finalWave.tangent);
        finalWave.binormal = normalize(finalWave.binormal);
    } else {
        finalWave.position = position.xyz;
        finalWave.normal = vec3(0.0, 1.0, 0.0);
        finalWave.tangent = vec3(1.0, 0.0, 0.0);
        finalWave.binormal = vec3(0.0, 0.0, 1.0);
    }

    vWorldNormalAndHeight.w = finalWave.position.y - position.y;

    position = vec4(finalWave.position, 1.0);
    vPositionWorld = water.modelMatrix * position;
    vPositionView = view * vPositionWorld;
    gl_Position = proj * vPositionView;
    vScreenPosition = gl_Position;

    mat3 normalMatrix = mat3(water.modelMatrix);
    vWorldNormalAndHeight.xyz = normalize(normalMatrix * finalWave.normal);
    vNormalView = normalize((view * vec4(vWorldNormalAndHeight.xyz, 0.0)).xyz);
    vTangentView = normalize((view * vec4(normalMatrix * finalWave.tangent, 0.0)).xyz);
    vBinormalView = normalize((view * vec4(normalMatrix * finalWave.binormal, 0.0)).xyz);

    vTexCoord0 = texCoord0;
}
