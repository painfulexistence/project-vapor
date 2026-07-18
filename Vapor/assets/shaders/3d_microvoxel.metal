#include <metal_stdlib>
using namespace metal;

// ============================================================================
// MicroVoxel primary pass — MSL twin of MicroVoxel.vert / MicroVoxel.frag.
// Keep the traversal and binding contracts mirrored (and in sync with the CPU
// reference in voxel_world.cpp).
//
// Raymarched micro-voxel rendering over Vapor's sparse storage: a page table
// (one uint per 8^3-voxel brick: empty / uniform-material / pool slot) over a
// 576-byte brick pool where a single linear in-brick index i = x + y*8 + z*64
// addresses both the occupancy bitmask (bit i&31 of word i>>5) and the
// material byte. Two-level DDA: the coarse walk skips empty page entries one
// brick per step, the fine walk tests bitmask bits and only touches material
// bytes on a hit. Hits return [[depth(any)]] from the true DDA hit point so
// voxels depth-composite with the raster scene; misses discard.
//
// Bindings: vertex params buffer(0); fragment params buffer(0),
// pageTable buffer(1), brickPool buffer(2), palette buffer(3).
// ============================================================================

// Must match Vapor::MicroVoxelRenderData (vec4-only, 256-byte stride).
struct MicroVoxelData {
    float4x4 viewProj;
    float4 cameraPosition;   // xyz; w = maxRaySteps
    float4 volumeOrigin;     // xyz = world min corner; w = voxelSize
    float4 gridDim;          // xyz = voxel counts; w = emissiveStrength
    float4 sunDirection;     // xyz toward the sun; w = shadowEnabled
    float4 sunColor;         // xyz; w = sunIntensity
    float4 ambientSky;       // xyz; w = ambientIntensity
    float4 ambientGround;    // xyz; w = albedo hash variation strength
    float4 params;           // x = aoStrength, y = debugMode, z = reflectionsEnabled, w = giStrength
    float4 extra0;           // x = volumeIndex, y = pageTableOffset, z = brickPoolBase, w = paletteBase
    float4 _pad[3];
};

constant uint MV_PAGE_EMPTY = 0xFFFFFFFFu;
constant uint MV_PAGE_UNIFORM_BIT = 0x80000000u;
constant int MV_BRICK_DIM = 8;
constant uint MV_BRICK_WORDS = 144u;  // 16 occupancy words + 128 packed material words
constant float MV_INV_PI = 0.318309886;

struct MicroVoxelVertexOut {
    float4 position [[position]];
    float3 worldPos;
};

// Unit cube [0,1]^3 as a 36-vertex triangle list.
constant float3 mvCubeVerts[36] = {
    float3(0, 0, 0), float3(1, 1, 0), float3(1, 0, 0),
    float3(0, 0, 0), float3(0, 1, 0), float3(1, 1, 0),
    float3(0, 0, 1), float3(1, 0, 1), float3(1, 1, 1),
    float3(0, 0, 1), float3(1, 1, 1), float3(0, 1, 1),
    float3(0, 0, 0), float3(0, 0, 1), float3(0, 1, 1),
    float3(0, 0, 0), float3(0, 1, 1), float3(0, 1, 0),
    float3(1, 0, 0), float3(1, 1, 1), float3(1, 0, 1),
    float3(1, 0, 0), float3(1, 1, 0), float3(1, 1, 1),
    float3(0, 0, 0), float3(1, 0, 0), float3(1, 0, 1),
    float3(0, 0, 0), float3(1, 0, 1), float3(0, 0, 1),
    float3(0, 1, 0), float3(1, 1, 1), float3(1, 1, 0),
    float3(0, 1, 0), float3(0, 1, 1), float3(1, 1, 1)
};

vertex MicroVoxelVertexOut microVoxelVertex(uint vertexID [[vertex_id]],
                                            constant MicroVoxelData& u [[buffer(0)]]) {
    float3 extent = u.gridDim.xyz * u.volumeOrigin.w;
    float3 wp = u.volumeOrigin.xyz + mvCubeVerts[vertexID] * extent;
    MicroVoxelVertexOut out;
    out.worldPos = wp;
    out.position = u.viewProj * float4(wp, 1.0);
    return out;
}

// ============================================================================
// Volume access
// ============================================================================

static inline int3 mvBrickGrid(constant MicroVoxelData& u) {
    return int3(u.gridDim.xyz) / MV_BRICK_DIM;
}

// The shared buffers hold every volume's data; this volume's ranges start at
// the offsets carried in extra0 (y = page entries, z = pool slots, w =
// palette entries).
static inline uint mvPageEntry(constant MicroVoxelData& u, device const uint* pageTable, int3 bcell) {
    int3 bg = mvBrickGrid(u);
    return pageTable[uint(u.extra0.y) + uint((bcell.z * bg.y + bcell.y) * bg.x + bcell.x)];
}

static inline int mvVoxelIndexInBrick(int3 local) {
    return local.x + local.y * MV_BRICK_DIM + local.z * MV_BRICK_DIM * MV_BRICK_DIM;
}

static inline bool mvBrickOccupied(constant MicroVoxelData& u, device const uint* brickPool, uint slot, int i) {
    uint g = uint(u.extra0.z) + slot;
    return (brickPool[g * MV_BRICK_WORDS + uint(i >> 5)] & (1u << (uint(i) & 31u))) != 0u;
}

static inline uint mvBrickMaterial(constant MicroVoxelData& u, device const uint* brickPool, uint slot, int i) {
    uint g = uint(u.extra0.z) + slot;
    uint word = brickPool[g * MV_BRICK_WORDS + 16u + uint(i >> 2)];
    return (word >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}

static inline uint mvVoxelMat(constant MicroVoxelData& u, device const uint* pageTable,
                              device const uint* brickPool, int3 cell) {
    int3 dim = int3(u.gridDim.xyz);
    if (any(cell < 0) || any(cell >= dim)) return 0u;
    uint entry = mvPageEntry(u, pageTable, cell / MV_BRICK_DIM);
    if (entry == MV_PAGE_EMPTY) return 0u;
    if ((entry & MV_PAGE_UNIFORM_BIT) != 0u) return entry & 0xFFu;
    return mvBrickMaterial(u, brickPool, entry, mvVoxelIndexInBrick(cell % MV_BRICK_DIM));
}

// ============================================================================
// Two-level DDA (coarse page-table walk + fine bitmask walk)
// ============================================================================

struct MvHit {
    float t;
    float3 normal;
    int3 cell;
    uint mat;
};

static bool mvTraverseBrick(constant MicroVoxelData& u, float3 ro, float3 rd, float3 invD,
                            device const uint* brickPool, uint slot, int3 bcell, float tIn,
                            float3 enterNormal, float voxelSize, thread MvHit& hit) {
    int3 lo = bcell * MV_BRICK_DIM;
    float eps = voxelSize * 1e-3;
    int3 cell = clamp(int3(floor((ro + rd * (tIn + eps)) / voxelSize)), lo, lo + MV_BRICK_DIM - 1);
    int3 stepDir = int3(sign(rd));
    float3 tDelta = abs(float3(voxelSize) * invD);
    float3 stepPos = float3(rd.x > 0.0, rd.y > 0.0, rd.z > 0.0);
    float3 tMax = ((float3(cell) + stepPos) * voxelSize - ro) * invD;
    float t = tIn;
    float3 normal = enterNormal;
    for (int i = 0; i < 3 * MV_BRICK_DIM + 1; i++) {
        int vi = mvVoxelIndexInBrick(cell - lo);
        if (mvBrickOccupied(u, brickPool, slot, vi)) {
            hit.t = t;
            hit.normal = normal;
            hit.cell = cell;
            hit.mat = mvBrickMaterial(u, brickPool, slot, vi);
            return true;
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x; tMax.x += tDelta.x; cell.x += stepDir.x;
            normal = float3(-float(stepDir.x), 0.0, 0.0);
            if (cell.x < lo.x || cell.x >= lo.x + MV_BRICK_DIM) return false;
        } else if (tMax.y < tMax.z) {
            t = tMax.y; tMax.y += tDelta.y; cell.y += stepDir.y;
            normal = float3(0.0, -float(stepDir.y), 0.0);
            if (cell.y < lo.y || cell.y >= lo.y + MV_BRICK_DIM) return false;
        } else {
            t = tMax.z; tMax.z += tDelta.z; cell.z += stepDir.z;
            normal = float3(0.0, 0.0, -float(stepDir.z));
            if (cell.z < lo.z || cell.z >= lo.z + MV_BRICK_DIM) return false;
        }
    }
    return false;
}

// Full traversal in LOCAL space (volume min corner at the origin, meters).
static bool mvRaycast(constant MicroVoxelData& u, device const uint* pageTable,
                      device const uint* brickPool, float3 ro, float3 rd, float maxDist,
                      thread MvHit& hit) {
    float voxelSize = u.volumeOrigin.w;
    float3 bmax = u.gridDim.xyz * voxelSize;
    float3 invD = 1.0 / rd;

    float3 t0 = (float3(0.0) - ro) * invD;
    float3 t1 = (bmax - ro) * invD;
    float3 tsmall = min(t0, t1);
    float3 tbig = max(t0, t1);
    float tEnter = max(max(tsmall.x, tsmall.y), tsmall.z);
    float tExit = min(min(tbig.x, tbig.y), tbig.z);
    if (tExit <= max(tEnter, 0.0f)) return false;

    float3 normal;
    if (tEnter > 0.0f) {
        if (tsmall.x > tsmall.y && tsmall.x > tsmall.z) normal = float3(-sign(rd.x), 0.0, 0.0);
        else if (tsmall.y > tsmall.z)                   normal = float3(0.0, -sign(rd.y), 0.0);
        else                                            normal = float3(0.0, 0.0, -sign(rd.z));
    } else {
        float3 a = abs(rd);
        if (a.x > a.y && a.x > a.z) normal = float3(-sign(rd.x), 0.0, 0.0);
        else if (a.y > a.z)         normal = float3(0.0, -sign(rd.y), 0.0);
        else                        normal = float3(0.0, 0.0, -sign(rd.z));
    }

    float eps = voxelSize * 1e-3;
    float t = max(tEnter, 0.0f) + eps;
    if (t > maxDist) return false;

    float brickSize = voxelSize * float(MV_BRICK_DIM);
    int3 bg = mvBrickGrid(u);
    int3 bcell = clamp(int3(floor((ro + rd * t) / brickSize)), int3(0), bg - 1);
    int3 stepDir = int3(sign(rd));
    float3 tDelta = abs(float3(brickSize) * invD);
    float3 stepPos = float3(rd.x > 0.0, rd.y > 0.0, rd.z > 0.0);
    float3 tMax = ((float3(bcell) + stepPos) * brickSize - ro) * invD;

    int maxSteps = int(u.cameraPosition.w);
    for (int i = 0; i < maxSteps; i++) {
        uint entry = mvPageEntry(u, pageTable, bcell);
        if (entry != MV_PAGE_EMPTY) {
            if ((entry & MV_PAGE_UNIFORM_BIT) != 0u) {
                hit.t = t;
                hit.normal = normal;
                hit.cell = clamp(int3(floor((ro + rd * (t + eps)) / voxelSize)),
                                 bcell * MV_BRICK_DIM, bcell * MV_BRICK_DIM + MV_BRICK_DIM - 1);
                hit.mat = entry & 0xFFu;
                return t <= maxDist;
            }
            if (mvTraverseBrick(u, ro, rd, invD, brickPool, entry, bcell, t, normal, voxelSize, hit)) {
                return hit.t <= maxDist;
            }
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x; tMax.x += tDelta.x; bcell.x += stepDir.x;
            normal = float3(-float(stepDir.x), 0.0, 0.0);
            if (bcell.x < 0 || bcell.x >= bg.x) break;
        } else if (tMax.y < tMax.z) {
            t = tMax.y; tMax.y += tDelta.y; bcell.y += stepDir.y;
            normal = float3(0.0, -float(stepDir.y), 0.0);
            if (bcell.y < 0 || bcell.y >= bg.y) break;
        } else {
            t = tMax.z; tMax.z += tDelta.z; bcell.z += stepDir.z;
            normal = float3(0.0, 0.0, -float(stepDir.z));
            if (bcell.z < 0 || bcell.z >= bg.z) break;
        }
        if (t > maxDist || t > tExit) break;
    }
    return false;
}

// ============================================================================
// Shading helpers (ported from the Atmospheric microvoxel.frag)
// ============================================================================

static inline float mvVoxelHash(int3 c) {
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u + uint(c.z) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFu) / 65535.0;
}

static inline float3 mvSkyRadiance(constant MicroVoxelData& u, float3 dir) {
    return mix(u.ambientGround.xyz, u.ambientSky.xyz, dir.y * 0.5 + 0.5) * u.ambientSky.w;
}

static inline float mvCornerAOTerm(bool side1, bool side2, bool corner) {
    if (side1 && side2) return 0.0;
    return 1.0 - (float(side1) + float(side2) + float(corner)) / 3.0;
}

static float mvFaceAO(constant MicroVoxelData& u, device const uint* pageTable,
                      device const uint* brickPool, int3 cell, float3 normal,
                      float3 hitLocal, float voxelSize) {
    int3 n = int3(normal);
    int3 t1 = (n.x != 0) ? int3(0, 1, 0) : int3(1, 0, 0);
    int3 t2 = (n.z != 0) ? int3(0, 1, 0) : int3(0, 0, 1);
    int3 base = cell + n;

    bool sN1 = mvVoxelMat(u, pageTable, brickPool, base - t1) != 0u;
    bool sP1 = mvVoxelMat(u, pageTable, brickPool, base + t1) != 0u;
    bool sN2 = mvVoxelMat(u, pageTable, brickPool, base - t2) != 0u;
    bool sP2 = mvVoxelMat(u, pageTable, brickPool, base + t2) != 0u;
    bool cNN = mvVoxelMat(u, pageTable, brickPool, base - t1 - t2) != 0u;
    bool cPN = mvVoxelMat(u, pageTable, brickPool, base + t1 - t2) != 0u;
    bool cNP = mvVoxelMat(u, pageTable, brickPool, base - t1 + t2) != 0u;
    bool cPP = mvVoxelMat(u, pageTable, brickPool, base + t1 + t2) != 0u;

    float aoNN = mvCornerAOTerm(sN1, sN2, cNN);
    float aoPN = mvCornerAOTerm(sP1, sN2, cPN);
    float aoNP = mvCornerAOTerm(sN1, sP2, cNP);
    float aoPP = mvCornerAOTerm(sP1, sP2, cPP);

    float3 frac = hitLocal / voxelSize - float3(cell);
    float uu = clamp(dot(frac, float3(t1)), 0.0f, 1.0f);
    float vv = clamp(dot(frac, float3(t2)), 0.0f, 1.0f);
    return mix(mix(aoNN, aoPN, uu), mix(aoNP, aoPP, uu), vv);
}

static inline void mvDecodeMaterial(constant MicroVoxelData& u, device const uint* palette, uint mat,
                                    thread float3& albedo, thread float& emission,
                                    thread float& reflectivity) {
    uint base = (uint(u.extra0.w) + mat) * 2u;
    uint w0 = palette[base];
    uint w1 = palette[base + 1u];
    albedo = float3(float(w0 & 0xFFu), float((w0 >> 8u) & 0xFFu), float((w0 >> 16u) & 0xFFu)) / 255.0;
    emission = float((w0 >> 24u) & 0xFFu) / 255.0;
    reflectivity = float(w1 & 0xFFu) / 255.0;
}

// ============================================================================

struct MicroVoxelFragOut {
    float4 color [[color(0)]];
    // Voxel G-buffer, consumed by the GI kernels (3d_microvoxel_gi.metal) so
    // they never re-trace primary visibility, and by the GI composite.
    float hitT [[color(1)]];         // camera distance along the ray; 0 = miss
    float4 albedoAO [[color(2)]];    // rgb = hash-varied albedo, a = corner AO
    float4 normalMat [[color(3)]];   // r = face-normal index/255, g = material/255, b = volumeIndex/255
    float depth [[depth(any)]];
};

fragment MicroVoxelFragOut microVoxelFragment(
    MicroVoxelVertexOut in [[stage_in]],
    constant MicroVoxelData& u [[buffer(0)]],
    device const uint* pageTable [[buffer(1)]],
    device const uint* brickPool [[buffer(2)]],
    device const uint* palette [[buffer(3)]]
) {
    float voxelSize = u.volumeOrigin.w;
    float3 ro = u.cameraPosition.xyz - u.volumeOrigin.xyz;
    float3 rd = normalize(in.worldPos - u.cameraPosition.xyz);

    MvHit hit;
    if (!mvRaycast(u, pageTable, brickPool, ro, rd, 1e9f, hit)) {
        discard_fragment();
    }

    float3 hitLocal = ro + rd * hit.t;
    float3 hitWorld = hitLocal + u.volumeOrigin.xyz;

    float3 albedo;
    float emission, reflectivity;
    mvDecodeMaterial(u, palette, hit.mat, albedo, emission, reflectivity);
    albedo *= mix(1.0, 0.85 + 0.3 * mvVoxelHash(hit.cell), u.ambientGround.w);

    float3 sunDir = u.sunDirection.xyz;
    float ndl = max(dot(hit.normal, sunDir), 0.0f);
    float shadow = 1.0;
    if (u.sunDirection.w > 0.5f && ndl > 0.0f) {
        MvHit sh;
        float3 so = hitLocal + hit.normal * voxelSize * 0.51f;
        if (mvRaycast(u, pageTable, brickPool, so, sunDir, 1e9f, sh)) shadow = 0.0;
    }

    float ao = mix(1.0f, mvFaceAO(u, pageTable, brickPool, hit.cell, hit.normal, hitLocal, voxelSize),
                   u.params.x);

    float3 direct = u.sunColor.xyz * u.sunColor.w * MV_INV_PI * ndl * shadow;
    // With GI on (params.w > 0) the traced-GI composite supplies the indirect
    // term (albedo * gi * ao); the flat sky ambient is the fallback.
    float3 indirect = (u.params.w > 0.0f) ? float3(0.0) : mvSkyRadiance(u, hit.normal);

    float3 color = albedo * (direct * (0.7 + 0.3 * ao) + indirect * ao);
    color += albedo * emission * u.gridDim.w;

    if (u.params.z > 0.5f && reflectivity > 0.001f) {
        float3 rdir = reflect(rd, hit.normal);
        float3 rorigin = hitLocal + hit.normal * voxelSize * 0.51f;
        float3 refl;
        MvHit rh;
        if (mvRaycast(u, pageTable, brickPool, rorigin, rdir, 1e9f, rh)) {
            float3 rAlbedo;
            float rEmission, rRefl;
            mvDecodeMaterial(u, palette, rh.mat, rAlbedo, rEmission, rRefl);
            float rNdl = max(dot(rh.normal, sunDir), 0.0f);
            refl = rAlbedo * (u.sunColor.xyz * u.sunColor.w * MV_INV_PI * rNdl + mvSkyRadiance(u, rh.normal))
                 + rAlbedo * rEmission * u.gridDim.w;
        } else {
            refl = mvSkyRadiance(u, rdir);
        }
        float f = reflectivity + (1.0 - reflectivity) * pow(1.0 - max(dot(-rd, hit.normal), 0.0f), 5.0);
        color = mix(color, refl, clamp(f, 0.0f, 1.0f));
    }

    int debugMode = int(u.params.y);
    if (debugMode == 1) color = albedo;
    else if (debugMode == 2) color = hit.normal * 0.5 + 0.5;
    else if (debugMode == 3) color = float3(ao);
    else if (debugMode == 4) color = float3(shadow);
    else if (debugMode == 6) color = float3(float(hit.mat) / 8.0);

    // Metal NDC z is already [0,1]; the clamp keeps hits in front of the sky.
    float4 clip = u.viewProj * float4(hitWorld, 1.0);

    // Voxel G-buffer. Face-normal index: axis*2 + (negative ? 1 : 0).
    int normalIdx = (hit.normal.x != 0.0f) ? (hit.normal.x < 0.0f ? 1 : 0)
                  : (hit.normal.y != 0.0f) ? (hit.normal.y < 0.0f ? 3 : 2)
                                           : (hit.normal.z < 0.0f ? 5 : 4);

    MicroVoxelFragOut out;
    out.color = float4(color, 1.0);
    out.hitT = hit.t;
    out.albedoAO = float4(albedo, ao);
    out.normalMat = float4(float(normalIdx) / 255.0, float(hit.mat) / 255.0, u.extra0.x / 255.0, 0.0);
    out.depth = clamp(clip.z / clip.w, 0.0f, 0.999999f);
    return out;
}
