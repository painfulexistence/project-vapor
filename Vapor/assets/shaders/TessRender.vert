#version 450
// Adaptive GPU tessellation — instanced draw path, vertex stage (Vulkan twin
// of tessVertexMain in 3d_tess_render.metal). One instance per CBT leaf
// (instanceCount GPU-written by TessPrepareArgs, consumed via
// drawIndexedIndirect): pull a grid vertex's barycentrics from b0 and lerp
// the leaf corners the TessLeafPrep kernel cached. Terrain instances
// (TESS_FLAG_TERRAIN) lift the vertex onto the OpenSimplex2-FBm heightfield
// (normal + palette color derived from it); others keep the sin/cos
// displacement placeholder.
//
// Binding contract (RHI::setVertexBuffer; matches the Metal slots, except
// TessParams rides the per-instance buffer at b3 instead of setVertexBytes):
//   b0 = grid barycentrics (vec2: w1, w2)
//   b1 = CameraData
//   b2 = TessLeafData[]
//   b3 = TessParams

const uint TESS_FLAG_TERRAIN = 2u;

struct CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float nearPlane;
    float farPlane;
    vec2 _pad;
    vec3 position;
    float _pad2;
    vec4 frustumPlanes[6];
};

struct TessLeafData {
    vec4 posU[3];
    vec4 nrmV[3];
    uint visible;
    uint depth;
    uint node;
    uint pad;
};

layout(std430, binding = 0) readonly buffer GridBuf { vec2 gridVerts[]; };
layout(std430, binding = 1) readonly buffer CameraBuf { CameraData cam; };
layout(std430, binding = 2) readonly buffer LeafBuf { TessLeafData leaves[]; };
layout(std430, binding = 3) readonly buffer ParamsBuf {
    mat4 model;
    uint maxDepth;
    uint rootDepth;
    uint rootCount;
    uint maxLeaves;
    float splitPixels;
    float screenHeight;
    float displacementScale;  // terrain instances: heightScale
    uint flags;
    uint gridIndexCount;
    float terrainFrequency;
    uint terrainSeed;
    uint terrainOctaves;
} params;

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec3 vWorldPosition;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec3 vTerrainColor;
layout(location = 4) flat out float vTerrainMix;
layout(location = 5) flat out uint vDepth;

// ── Terrain height field — the same FastNoiseLite v1.1.1 OpenSimplex2 FBm
// GLSL port as RHIMain.frag / TessClassify.comp (TerrainWorld::heightAt).
const int kFnlPrimeX = 501125321;
const int kFnlPrimeY = 1136930381;
const vec2 kFnlFan[24] = vec2[24](
    vec2(0.130526192220052, 0.99144486137381),   vec2(0.38268343236509, 0.923879532511287),
    vec2(0.608761429008721, 0.793353340291235),  vec2(0.793353340291235, 0.608761429008721),
    vec2(0.923879532511287, 0.38268343236509),   vec2(0.99144486137381, 0.130526192220051),
    vec2(0.99144486137381, -0.130526192220051),  vec2(0.923879532511287, -0.38268343236509),
    vec2(0.793353340291235, -0.60876142900872),  vec2(0.608761429008721, -0.793353340291235),
    vec2(0.38268343236509, -0.923879532511287),  vec2(0.130526192220052, -0.99144486137381),
    vec2(-0.130526192220052, -0.99144486137381), vec2(-0.38268343236509, -0.923879532511287),
    vec2(-0.608761429008721, -0.793353340291235),vec2(-0.793353340291235, -0.608761429008721),
    vec2(-0.923879532511287, -0.38268343236509), vec2(-0.99144486137381, -0.130526192220052),
    vec2(-0.99144486137381, 0.130526192220051),  vec2(-0.923879532511287, 0.38268343236509),
    vec2(-0.793353340291235, 0.608761429008721), vec2(-0.608761429008721, 0.793353340291235),
    vec2(-0.38268343236509, 0.923879532511287),  vec2(-0.130526192220052, 0.99144486137381));
vec2 fnlGradient2(int pairIndex) {
    return pairIndex < 120 ? kFnlFan[pairIndex % 24] : kFnlFan[1 + 3 * (pairIndex - 120)];
}
float fnlGradCoord(int seed, int xPrimed, int yPrimed, float xd, float yd) {
    int hash = (seed ^ xPrimed ^ yPrimed) * 0x27d4eb2d;
    hash ^= hash >> 15;
    hash &= 127 << 1;
    vec2 g = fnlGradient2(hash >> 1);
    return xd * g.x + yd * g.y;
}
float fnlSimplex2(int seed, vec2 p) {
    const float SQRT3 = 1.7320508075688772935;
    const float G2 = (3.0 - SQRT3) / 6.0;
    int i = int(floor(p.x)), j = int(floor(p.y));
    float xi = p.x - float(i), yi = p.y - float(j);
    float t = (xi + yi) * G2;
    float x0 = xi - t, y0 = yi - t;
    i *= kFnlPrimeX;
    j *= kFnlPrimeY;
    float n0 = 0.0, n1 = 0.0, n2 = 0.0;
    float a = 0.5 - x0 * x0 - y0 * y0;
    if (a > 0.0) n0 = (a * a) * (a * a) * fnlGradCoord(seed, i, j, x0, y0);
    float c = (2.0 * (1.0 - 2.0 * G2) * (1.0 / G2 - 2.0)) * t
            + ((-2.0 * (1.0 - 2.0 * G2) * (1.0 - 2.0 * G2)) + a);
    if (c > 0.0) {
        float x2 = x0 + (2.0 * G2 - 1.0), y2 = y0 + (2.0 * G2 - 1.0);
        n2 = (c * c) * (c * c) * fnlGradCoord(seed, i + kFnlPrimeX, j + kFnlPrimeY, x2, y2);
    }
    if (y0 > x0) {
        float x1 = x0 + G2, y1 = y0 + (G2 - 1.0);
        float b = 0.5 - x1 * x1 - y1 * y1;
        if (b > 0.0) n1 = (b * b) * (b * b) * fnlGradCoord(seed, i, j + kFnlPrimeY, x1, y1);
    } else {
        float x1 = x0 + (G2 - 1.0), y1 = y0 + G2;
        float b = 0.5 - x1 * x1 - y1 * y1;
        if (b > 0.0) n1 = (b * b) * (b * b) * fnlGradCoord(seed, i + kFnlPrimeX, j, x1, y1);
    }
    return (n0 + n1 + n2) * 99.83685446303647;
}
float trHeightAt(vec2 xz, float noiseFreq, int octaves, uint seed, float heightScale) {
    vec2 p = xz * noiseFreq;
    const float SQRT3 = 1.7320508075688772935;
    const float F2 = 0.5 * (SQRT3 - 1.0);
    p += vec2((p.x + p.y) * F2);
    const float gain = 0.5;
    float amp = gain, ampFractal = 1.0;
    for (int i = 1; i < octaves; ++i) { ampFractal += amp; amp *= gain; }
    int s = int(seed);
    float sum = 0.0;
    amp = 1.0 / ampFractal;  // fractalBounding
    for (int i = 0; i < octaves; ++i) {
        sum += fnlSimplex2(s++, p) * amp;
        p *= 2.0;
        amp *= gain;
    }
    return clamp(sum * 0.5 + 0.5, 0.0, 1.0) * heightScale;
}

float tessTerrainHeight(vec3 p) {
    return trHeightAt(p.xz, params.terrainFrequency, int(params.terrainOctaves),
                      params.terrainSeed, params.displacementScale);
}

// buildPaletteLUT's bands (terrain_world.cpp), transcribed — same fallback
// look as the MSL twin in 3d_tess_lib.metal.
vec3 tessTerrainPalette(float h01, float slope01) {
    const vec3 sand = vec3(0.76, 0.70, 0.50), grassC = vec3(0.22, 0.42, 0.16);
    const vec3 dirt = vec3(0.42, 0.32, 0.20), snow = vec3(0.92, 0.94, 0.97);
    const vec3 rock = vec3(0.44, 0.43, 0.41);
    vec3 c;
    if (h01 < 0.12) c = sand;
    else if (h01 < 0.20) c = mix(sand, grassC, (h01 - 0.12) / 0.08);
    else if (h01 < 0.50) c = grassC;
    else if (h01 < 0.62) c = mix(grassC, dirt, (h01 - 0.50) / 0.12);
    else if (h01 < 0.70) c = mix(dirt, snow, (h01 - 0.62) / 0.08);
    else c = snow;
    return mix(c, rock, smoothstep(0.35, 0.75, slope01));
}

// Generic procedural displacement placeholder (non-terrain instances).
float tessDisplaceAmount(vec3 p, float scale) {
    if (scale == 0.0) return 0.0;
    float h = sin(p.x * 3.1) * cos(p.z * 2.7) +
              0.35 * sin(p.x * 9.3 + p.z * 7.1) * cos(p.y * 4.3);
    return h * scale;
}

void main() {
    TessLeafData leaf = leaves[gl_InstanceIndex];
    if (leaf.visible == 0u) {
        // Frustum-culled leaf: collapse the whole instance to one point so
        // the rasterizer drops it (kept instead of compacted — the leaf list
        // stays in deterministic heap order).
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vWorldNormal = vec3(0, 1, 0);
        vWorldPosition = vec3(0);
        vUv = vec2(0);
        vTerrainColor = vec3(0);
        vTerrainMix = 0.0;
        vDepth = 0u;
        return;
    }

    vec2 g = gridVerts[gl_VertexIndex];
    vec3 w = vec3(1.0 - g.x - g.y, g.x, g.y);  // (w0, w1, w2)

    vec3 pos = w.x * leaf.posU[0].xyz + w.y * leaf.posU[1].xyz + w.z * leaf.posU[2].xyz;
    vec3 nrm = w.x * leaf.nrmV[0].xyz + w.y * leaf.nrmV[1].xyz + w.z * leaf.nrmV[2].xyz;
    vec2 uv = w.x * vec2(leaf.posU[0].w, leaf.nrmV[0].w) +
              w.y * vec2(leaf.posU[1].w, leaf.nrmV[1].w) +
              w.z * vec2(leaf.posU[2].w, leaf.nrmV[2].w);

    // Displacement is a function of the undisplaced object-space position
    // only, so leaves sharing an edge displace its vertices identically.
    nrm = normalize(nrm);
    vTerrainColor = vec3(0);
    vTerrainMix = 0.0;
    if ((params.flags & TESS_FLAG_TERRAIN) != 0u) {
        // Same math as tessTerrainDisplace in 3d_tess_lib.metal: d = 1 m
        // matches the LOD0 heightmap texel spacing of the original demo.
        const float d = 1.0;
        float h  = tessTerrainHeight(pos);
        float hl = tessTerrainHeight(pos - vec3(d, 0.0, 0.0));
        float hr = tessTerrainHeight(pos + vec3(d, 0.0, 0.0));
        float hb = tessTerrainHeight(pos - vec3(0.0, 0.0, d));
        float ht = tessTerrainHeight(pos + vec3(0.0, 0.0, d));
        pos = vec3(pos.x, pos.y + h, pos.z);
        nrm = normalize(vec3(hl - hr, 2.0 * d, hb - ht));
        float slope01 = clamp(sqrt((hr - hl) * (hr - hl) + (ht - hb) * (ht - hb)) / (4.0 * d), 0.0, 1.0);
        vTerrainColor = tessTerrainPalette(h / max(params.displacementScale, 1e-3), slope01);
        vTerrainMix = 1.0;
    } else {
        pos += nrm * tessDisplaceAmount(pos, params.displacementScale);
    }

    vec4 world = params.model * vec4(pos, 1.0);
    gl_Position = cam.proj * cam.view * world;
    vWorldPosition = world.xyz;
    // Uniform-scale assumption for the normal (matches the debug shading;
    // proper inverse-transpose comes with material integration).
    vWorldNormal = normalize((params.model * vec4(nrm, 0.0)).xyz);
    vUv = uv;
    vDepth = leaf.depth;
}
