#version 450
// MicroVoxel primary pass, fragment stage. GLSL twin of 3d_microvoxel.metal —
// keep the traversal and binding contracts mirrored (and in sync with the CPU
// reference in voxel_world.cpp).
//
// Raymarched micro-voxel rendering, ported from the Atmospheric engine's
// MicroVoxelPass and modernized onto Vapor's sparse storage: instead of a
// dense R8UI volume texture + byte-per-brick occupancy texture, the volume is
// a page table (one uint per 8^3-voxel brick: empty / uniform-material /
// pool slot) over a 576-byte brick pool where a single linear in-brick index
// i = x + y*8 + z*64 addresses both the occupancy bitmask (bit i&31 of word
// i>>5) and the material byte. The two-level DDA survives intact: the coarse
// walk skips empty page entries one brick per step, the fine walk tests
// bitmask bits and only touches material bytes on a hit.
//
// The pass rasterizes the volume AABB (cull off), every covered fragment
// traverses, misses discard, and hits write gl_FragDepth from the true DDA
// hit point — so voxels depth-composite with the raster scene and every
// downstream depth-reading pass (sky, fog, clouds, god rays) treats them as
// ordinary geometry. Output is linear HDR into colorRT; the engine tonemap
// handles exposure, and emissive voxels feed bloom naturally.
//
// Binding convention (see rhi_vulkan.cpp): set 1 = fragment-stage buffers.

layout(location = 0) in vec3 v_worldPos;
layout(location = 0) out vec4 outColor;

// Must match Vapor::MicroVoxelRenderData (vec4-only, 256-byte stride so
// per-volume slices can be bound at aligned offsets).
layout(std430, set = 1, binding = 0) readonly buffer ParamsBuf {
    mat4 viewProj;
    vec4 cameraPosition;   // xyz; w = maxRaySteps
    vec4 volumeOrigin;     // xyz = world min corner; w = voxelSize
    vec4 gridDim;          // xyz = voxel counts; w = emissiveStrength
    vec4 sunDirection;     // xyz toward the sun; w = shadowEnabled
    vec4 sunColor;         // xyz; w = sunIntensity
    vec4 ambientSky;       // xyz; w = ambientIntensity
    vec4 ambientGround;    // xyz; w = albedo hash variation strength
    vec4 params;           // x = aoStrength, y = debugMode, z = reflectionsEnabled
};
layout(std430, set = 1, binding = 1) readonly buffer PageBuf { uint pageTable[]; };
layout(std430, set = 1, binding = 2) readonly buffer BrickBuf { uint brickPool[]; };
// 256 materials, 2 words each: word0 = R|G<<8|B<<16|emission<<24,
// word1 = reflectivity|roughness<<8 (VoxelMaterial's byte layout).
layout(std430, set = 1, binding = 3) readonly buffer PaletteBuf { uint palette[]; };

const uint PAGE_EMPTY = 0xFFFFFFFFu;
const uint PAGE_UNIFORM_BIT = 0x80000000u;
const int BRICK_DIM = 8;
const uint BRICK_WORDS = 144u;  // 16 occupancy words + 128 packed material words
const float INV_PI = 0.318309886;

// ============================================================================
// Volume access
// ============================================================================

ivec3 brickGrid() { return ivec3(gridDim.xyz) / BRICK_DIM; }

uint pageEntry(ivec3 bcell) {
    ivec3 bg = brickGrid();
    return pageTable[(bcell.z * bg.y + bcell.y) * bg.x + bcell.x];
}

int voxelIndexInBrick(ivec3 local) {
    return local.x + local.y * BRICK_DIM + local.z * BRICK_DIM * BRICK_DIM;
}

bool brickOccupied(uint slot, int i) {
    return (brickPool[slot * BRICK_WORDS + uint(i >> 5)] & (1u << (uint(i) & 31u))) != 0u;
}

uint brickMaterial(uint slot, int i) {
    uint word = brickPool[slot * BRICK_WORDS + 16u + uint(i >> 2)];
    return (word >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}

// Material of a voxel cell (0 = air / out of bounds). Used by the AO taps.
uint voxelMat(ivec3 cell) {
    ivec3 dim = ivec3(gridDim.xyz);
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, dim))) return 0u;
    uint entry = pageEntry(cell / BRICK_DIM);
    if (entry == PAGE_EMPTY) return 0u;
    if ((entry & PAGE_UNIFORM_BIT) != 0u) return entry & 0xFFu;
    return brickMaterial(entry, voxelIndexInBrick(cell % BRICK_DIM));
}

// ============================================================================
// Two-level DDA (coarse page-table walk + fine bitmask walk)
// ============================================================================

struct Hit {
    float t;
    vec3 normal;
    ivec3 cell;
    uint mat;
};

// Fine walk of one occupied (pooled) brick, starting at ray parameter tIn.
// enterNormal is the face the ray crossed to reach this brick, so a hit on
// the very first cell still gets a valid normal. Loop bound 3*8+1 covers the
// longest possible path through the brick.
bool traverseBrick(vec3 ro, vec3 rd, vec3 invD, uint slot, ivec3 bcell, float tIn,
                   vec3 enterNormal, float voxelSize, out Hit hit) {
    ivec3 lo = bcell * BRICK_DIM;
    float eps = voxelSize * 1e-3;
    ivec3 cell = clamp(ivec3(floor((ro + rd * (tIn + eps)) / voxelSize)), lo, lo + BRICK_DIM - 1);
    ivec3 stepDir = ivec3(sign(rd));
    vec3 tDelta = abs(vec3(voxelSize) * invD);
    vec3 stepPos = vec3(greaterThan(rd, vec3(0.0)));
    vec3 tMax = ((vec3(cell) + stepPos) * voxelSize - ro) * invD;
    float t = tIn;
    vec3 normal = enterNormal;
    for (int i = 0; i < 3 * BRICK_DIM + 1; i++) {
        int vi = voxelIndexInBrick(cell - lo);
        if (brickOccupied(slot, vi)) {
            hit.t = t;
            hit.normal = normal;
            hit.cell = cell;
            hit.mat = brickMaterial(slot, vi);
            return true;
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x; tMax.x += tDelta.x; cell.x += stepDir.x;
            normal = vec3(-float(stepDir.x), 0.0, 0.0);
            if (cell.x < lo.x || cell.x >= lo.x + BRICK_DIM) return false;
        } else if (tMax.y < tMax.z) {
            t = tMax.y; tMax.y += tDelta.y; cell.y += stepDir.y;
            normal = vec3(0.0, -float(stepDir.y), 0.0);
            if (cell.y < lo.y || cell.y >= lo.y + BRICK_DIM) return false;
        } else {
            t = tMax.z; tMax.z += tDelta.z; cell.z += stepDir.z;
            normal = vec3(0.0, 0.0, -float(stepDir.z));
            if (cell.z < lo.z || cell.z >= lo.z + BRICK_DIM) return false;
        }
    }
    return false;
}

// Full traversal in LOCAL space (volume min corner at the origin, meters).
bool raycast(vec3 ro, vec3 rd, float maxDist, out Hit hit) {
    float voxelSize = volumeOrigin.w;
    vec3 bmax = gridDim.xyz * voxelSize;
    vec3 invD = 1.0 / rd;  // IEEE inf on axis-parallel components works with min/max

    vec3 t0 = (vec3(0.0) - ro) * invD;
    vec3 t1 = (bmax - ro) * invD;
    vec3 tsmall = min(t0, t1);
    vec3 tbig = max(t0, t1);
    float tEnter = max(max(tsmall.x, tsmall.y), tsmall.z);
    float tExit = min(min(tbig.x, tbig.y), tbig.z);
    if (tExit <= max(tEnter, 0.0)) return false;

    // Entry-face normal: dominant tNear axis, or the dominant ray axis when
    // the ray starts inside (tEnter <= 0), so a solid first cell still gets a
    // valid normal.
    vec3 normal;
    if (tEnter > 0.0) {
        if (tsmall.x > tsmall.y && tsmall.x > tsmall.z) normal = vec3(-sign(rd.x), 0.0, 0.0);
        else if (tsmall.y > tsmall.z)                   normal = vec3(0.0, -sign(rd.y), 0.0);
        else                                            normal = vec3(0.0, 0.0, -sign(rd.z));
    } else {
        vec3 a = abs(rd);
        if (a.x > a.y && a.x > a.z) normal = vec3(-sign(rd.x), 0.0, 0.0);
        else if (a.y > a.z)         normal = vec3(0.0, -sign(rd.y), 0.0);
        else                        normal = vec3(0.0, 0.0, -sign(rd.z));
    }

    float eps = voxelSize * 1e-3;
    float t = max(tEnter, 0.0) + eps;
    if (t > maxDist) return false;

    float brickSize = voxelSize * float(BRICK_DIM);
    ivec3 bg = brickGrid();
    ivec3 bcell = clamp(ivec3(floor((ro + rd * t) / brickSize)), ivec3(0), bg - 1);
    ivec3 stepDir = ivec3(sign(rd));
    vec3 tDelta = abs(vec3(brickSize) * invD);
    vec3 stepPos = vec3(greaterThan(rd, vec3(0.0)));
    vec3 tMax = ((vec3(bcell) + stepPos) * brickSize - ro) * invD;

    int maxSteps = int(cameraPosition.w);
    for (int i = 0; i < maxSteps; i++) {
        uint entry = pageEntry(bcell);
        if (entry != PAGE_EMPTY) {
            if ((entry & PAGE_UNIFORM_BIT) != 0u) {
                // Uniform brick: every voxel solid — hit at the brick entry.
                hit.t = t;
                hit.normal = normal;
                hit.cell = clamp(ivec3(floor((ro + rd * (t + eps)) / voxelSize)),
                                 bcell * BRICK_DIM, bcell * BRICK_DIM + BRICK_DIM - 1);
                hit.mat = entry & 0xFFu;
                return t <= maxDist;
            }
            if (traverseBrick(ro, rd, invD, entry, bcell, t, normal, voxelSize, hit)) {
                return hit.t <= maxDist;
            }
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x; tMax.x += tDelta.x; bcell.x += stepDir.x;
            normal = vec3(-float(stepDir.x), 0.0, 0.0);
            if (bcell.x < 0 || bcell.x >= bg.x) break;
        } else if (tMax.y < tMax.z) {
            t = tMax.y; tMax.y += tDelta.y; bcell.y += stepDir.y;
            normal = vec3(0.0, -float(stepDir.y), 0.0);
            if (bcell.y < 0 || bcell.y >= bg.y) break;
        } else {
            t = tMax.z; tMax.z += tDelta.z; bcell.z += stepDir.z;
            normal = vec3(0.0, 0.0, -float(stepDir.z));
            if (bcell.z < 0 || bcell.z >= bg.z) break;
        }
        if (t > maxDist || t > tExit) break;
    }
    return false;
}

// ============================================================================
// Shading helpers (ported from the Atmospheric microvoxel.frag)
// ============================================================================

// Integer hash -> [0,1), for per-voxel albedo variation.
float voxelHash(ivec3 c) {
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u + uint(c.z) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFu) / 65535.0;
}

vec3 skyRadiance(vec3 dir) {
    return mix(ambientGround.xyz, ambientSky.xyz, dir.y * 0.5 + 0.5) * ambientSky.w;
}

// Classic corner AO: 8 taps in the voxel layer one step outside the hit face,
// bilinearly blended by the fractional hit position within the face.
float cornerAOTerm(bool side1, bool side2, bool corner) {
    if (side1 && side2) return 0.0;
    return 1.0 - (float(side1) + float(side2) + float(corner)) / 3.0;
}

float faceAO(ivec3 cell, vec3 normal, vec3 hitLocal, float voxelSize) {
    ivec3 n = ivec3(normal);
    ivec3 t1 = (n.x != 0) ? ivec3(0, 1, 0) : ivec3(1, 0, 0);
    ivec3 t2 = (n.z != 0) ? ivec3(0, 1, 0) : ivec3(0, 0, 1);
    ivec3 base = cell + n;

    bool sN1 = voxelMat(base - t1) != 0u, sP1 = voxelMat(base + t1) != 0u;
    bool sN2 = voxelMat(base - t2) != 0u, sP2 = voxelMat(base + t2) != 0u;
    bool cNN = voxelMat(base - t1 - t2) != 0u, cPN = voxelMat(base + t1 - t2) != 0u;
    bool cNP = voxelMat(base - t1 + t2) != 0u, cPP = voxelMat(base + t1 + t2) != 0u;

    float aoNN = cornerAOTerm(sN1, sN2, cNN);
    float aoPN = cornerAOTerm(sP1, sN2, cPN);
    float aoNP = cornerAOTerm(sN1, sP2, cNP);
    float aoPP = cornerAOTerm(sP1, sP2, cPP);

    vec3 frac = hitLocal / voxelSize - vec3(cell);
    float u = clamp(dot(frac, vec3(t1)), 0.0, 1.0);
    float v = clamp(dot(frac, vec3(t2)), 0.0, 1.0);
    return mix(mix(aoNN, aoPN, u), mix(aoNP, aoPP, u), v);
}

void decodeMaterial(uint mat, out vec3 albedo, out float emission, out float reflectivity) {
    uint w0 = palette[mat * 2u];
    uint w1 = palette[mat * 2u + 1u];
    albedo = vec3(float(w0 & 0xFFu), float((w0 >> 8u) & 0xFFu), float((w0 >> 16u) & 0xFFu)) / 255.0;
    emission = float((w0 >> 24u) & 0xFFu) / 255.0;
    reflectivity = float(w1 & 0xFFu) / 255.0;
}

// ============================================================================

void main() {
    float voxelSize = volumeOrigin.w;
    // Traverse in local space: the volume min corner sits at the origin, so
    // the DDA math matches the CPU reference exactly.
    vec3 ro = cameraPosition.xyz - volumeOrigin.xyz;
    vec3 rd = normalize(v_worldPos - cameraPosition.xyz);

    Hit hit;
    if (!raycast(ro, rd, 1e9, hit)) {
        discard;  // leave raster color/depth untouched
    }

    vec3 hitLocal = ro + rd * hit.t;
    vec3 hitWorld = hitLocal + volumeOrigin.xyz;

    vec3 albedo;
    float emission, reflectivity;
    decodeMaterial(hit.mat, albedo, emission, reflectivity);
    // Per-voxel hash variation keeps large same-material surfaces alive.
    albedo *= mix(1.0, 0.85 + 0.3 * voxelHash(hit.cell), ambientGround.w);

    // Sun light with an optional traced shadow ray (full two-level DDA).
    vec3 sunDir = sunDirection.xyz;
    float ndl = max(dot(hit.normal, sunDir), 0.0);
    float shadow = 1.0;
    if (sunDirection.w > 0.5 && ndl > 0.0) {
        Hit sh;
        vec3 so = hitLocal + hit.normal * voxelSize * 0.51;
        if (raycast(so, sunDir, 1e9, sh)) shadow = 0.0;
    }

    float ao = mix(1.0, faceAO(hit.cell, hit.normal, hitLocal, voxelSize), params.x);

    vec3 direct = sunColor.xyz * sunColor.w * INV_PI * ndl * shadow;
    vec3 indirect = skyRadiance(hit.normal);

    // AO fully attenuates the ambient term but only 30% of direct sun, so
    // corners stay readable in full sunlight (the original's stylized mix).
    vec3 color = albedo * (direct * (0.7 + 0.3 * ao) + indirect * ao);
    color += albedo * emission * gridDim.w;  // after AO: emissives stay bright

    // Mirror reflections for reflective palette entries (Fresnel-blended).
    if (params.z > 0.5 && reflectivity > 0.001) {
        vec3 rdir = reflect(rd, hit.normal);
        vec3 rorigin = hitLocal + hit.normal * voxelSize * 0.51;
        vec3 refl;
        Hit rh;
        if (raycast(rorigin, rdir, 1e9, rh)) {
            vec3 rAlbedo;
            float rEmission, rRefl;
            decodeMaterial(rh.mat, rAlbedo, rEmission, rRefl);
            float rNdl = max(dot(rh.normal, sunDir), 0.0);
            refl = rAlbedo * (sunColor.xyz * sunColor.w * INV_PI * rNdl + skyRadiance(rh.normal))
                 + rAlbedo * rEmission * gridDim.w;
        } else {
            refl = skyRadiance(rdir);
        }
        float f = reflectivity + (1.0 - reflectivity) * pow(1.0 - max(dot(-rd, hit.normal), 0.0), 5.0);
        color = mix(color, refl, clamp(f, 0.0, 1.0));
    }

    // Debug views: 1=albedo 2=normal 3=ao 4=shadow 6=material index.
    int debugMode = int(params.y);
    if (debugMode == 1) color = albedo;
    else if (debugMode == 2) color = hit.normal * 0.5 + 0.5;
    else if (debugMode == 3) color = vec3(ao);
    else if (debugMode == 4) color = vec3(shadow);
    else if (debugMode == 6) color = vec3(float(hit.mat) / 8.0);

    outColor = vec4(color, 1.0);

    // True hit depth so the hardware depth test composites voxels with the
    // raster scene (and later sky/fog/cloud passes see voxel depth). Vulkan
    // NDC z is already [0,1] — no GL-style remap. The 0.999999 clamp keeps
    // hits in front of the far-plane sky.
    vec4 clip = viewProj * vec4(hitWorld, 1.0);
    gl_FragDepth = clamp(clip.z / clip.w, 0.0, 0.999999);
}
