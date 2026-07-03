#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// ============================================================================
// Micro Voxel Raymarch (experimental)
// ============================================================================
// Fullscreen-triangle pass that raymarches a sparse brickmap with a two-level
// DDA: a coarse pass over 8^3-voxel bricks and a fine pass over individual
// voxels inside stored bricks. Hits output hardware depth so voxels composite
// correctly with rasterized geometry, the sky pass, and all downstream
// post-processing (fog, bloom, tone mapping).
//
// Brickmap layout: the coarse grid holds one uint32 per brick cell —
//   0xFFFFFFFF            empty brick (skipped in one coarse DDA step)
//   0x80000000 | material uniform solid brick (immediate hit, no fine data)
//   otherwise             index of a 512-byte brick in the brick pool
// Only mixed (surface) bricks occupy pool memory; fully-buried terrain
// collapses to uniform entries.
//
// Buffers:
//   0 = VoxelVolumeData (per-frame uniforms)
//   1 = CameraData
//   2 = brick pool       (brickDim^3 uint8 palette indices per stored brick)
//   3 = brick index grid (uint32 per brick cell, see layout above)
//   4 = palette          (256 x float4 albedo)

constant uint VOXEL_BRICK_EMPTY = 0xFFFFFFFFu;
constant uint VOXEL_BRICK_UNIFORM_FLAG = 0x80000000u;

struct VoxelVolumeData {
    float4x4 invViewProj;
    float3 cameraPosition;
    float3 sunDirection;        // normalized, points toward the sun
    float3 sunColor;
    float3 volumeOrigin;        // world-space min corner of the volume
    float sunIntensity;
    float voxelSize;            // world-space edge length of one voxel
    uint brickDim;              // voxels per brick edge (8)
    uint maxRaySteps;           // cap on brick-level DDA iterations
    uint3 gridDim;              // voxels per volume edge
    uint shadowEnabled;
    float ambientIntensity;
    float2 _pad;
};

struct RayHit {
    float t;
    float3 normal;
    uint material;
    bool hit;
};

static uint voxelIndex(int3 c, uint3 dim) {
    return (uint(c.z) * dim.y + uint(c.y)) * dim.x + uint(c.x);
}

// Small deterministic hash for per-voxel albedo variation.
static float voxelHash(int3 c) {
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u + uint(c.z) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h & 0xFFFFu) / 65535.0;
}

// Fine DDA over individual voxels inside one stored brick. Traverses from
// tStart until leaving the brick's cell range. enterNormal is the surface
// normal to report if the very first voxel tested is already solid.
static RayHit traverseBrick(
    float3 ro,
    float3 rd,
    float3 invDir,
    float tStart,
    int3 brickCell,
    uint brickIndex,
    float3 enterNormal,
    constant VoxelVolumeData& data,
    const device uchar* brickPool
) {
    RayHit result;
    result.hit = false;
    result.t = 0.0;
    result.normal = enterNormal;
    result.material = 0;

    int bd = int(data.brickDim);
    int3 lo = brickCell * bd;
    int3 hi = lo + bd - 1;

    float eps = data.voxelSize * 1e-3;
    float3 p = ro + rd * (tStart + eps);
    int3 cell = clamp(int3(floor((p - data.volumeOrigin) / data.voxelSize)), lo, hi);

    int3 stepDir = int3(sign(rd));
    float3 tDelta = abs(float3(data.voxelSize) * invDir);
    // Distance along the ray to the next voxel boundary on each axis.
    float3 stepPos = select(float3(0.0), float3(1.0), rd > 0.0);
    float3 boundary = data.volumeOrigin + (float3(cell) + stepPos) * data.voxelSize;
    float3 tMax = select(float3(INFINITY), (boundary - ro) * invDir, rd != 0.0);

    float t = tStart;
    float3 normal = enterNormal;
    const device uchar* brick = brickPool + brickIndex * uint(bd * bd * bd);

    for (int i = 0; i < 3 * bd + 1; i++) {
        int3 local = cell - lo;
        uchar mat = brick[(uint(local.z) * uint(bd) + uint(local.y)) * uint(bd) + uint(local.x)];
        if (mat != 0) {
            result.hit = true;
            result.t = t;
            result.normal = normal;
            result.material = uint(mat);
            return result;
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x;
            tMax.x += tDelta.x;
            cell.x += stepDir.x;
            normal = float3(-float(stepDir.x), 0.0, 0.0);
            if (cell.x < lo.x || cell.x > hi.x) break;
        } else if (tMax.y < tMax.z) {
            t = tMax.y;
            tMax.y += tDelta.y;
            cell.y += stepDir.y;
            normal = float3(0.0, -float(stepDir.y), 0.0);
            if (cell.y < lo.y || cell.y > hi.y) break;
        } else {
            t = tMax.z;
            tMax.z += tDelta.z;
            cell.z += stepDir.z;
            normal = float3(0.0, 0.0, -float(stepDir.z));
            if (cell.z < lo.z || cell.z > hi.z) break;
        }
    }
    return result;
}

// Coarse DDA over bricks; uniform bricks hit immediately, stored bricks
// descend into traverseBrick.
static RayHit raycastVoxels(
    float3 ro,
    float3 rd,
    constant VoxelVolumeData& data,
    const device uchar* brickPool,
    const device uint* brickIndexGrid,
    uint maxSteps
) {
    RayHit result;
    result.hit = false;
    result.t = 0.0;
    result.normal = float3(0.0);
    result.material = 0;

    float3 invDir = select(float3(INFINITY), 1.0 / rd, rd != 0.0);
    float3 bmin = data.volumeOrigin;
    float3 bmax = bmin + float3(data.gridDim) * data.voxelSize;

    // Slab test against the volume AABB.
    float3 tA = (bmin - ro) * invDir;
    float3 tB = (bmax - ro) * invDir;
    float3 tNear = min(tA, tB);
    float3 tFar = max(tA, tB);
    float tEnter = max(max(tNear.x, tNear.y), tNear.z);
    float tExit = min(min(tFar.x, tFar.y), tFar.z);
    if (tExit <= max(tEnter, 0.0)) return result;

    // Normal of the AABB face the ray enters through (used when the first
    // voxel tested is solid). For rays starting inside, fall back to -rd's
    // dominant axis.
    float3 enterNormal;
    if (tEnter > 0.0) {
        if (tNear.x > tNear.y && tNear.x > tNear.z) {
            enterNormal = float3(-sign(rd.x), 0.0, 0.0);
        } else if (tNear.y > tNear.z) {
            enterNormal = float3(0.0, -sign(rd.y), 0.0);
        } else {
            enterNormal = float3(0.0, 0.0, -sign(rd.z));
        }
    } else {
        float3 ard = abs(rd);
        if (ard.x > ard.y && ard.x > ard.z) {
            enterNormal = float3(-sign(rd.x), 0.0, 0.0);
        } else if (ard.y > ard.z) {
            enterNormal = float3(0.0, -sign(rd.y), 0.0);
        } else {
            enterNormal = float3(0.0, 0.0, -sign(rd.z));
        }
    }

    float brickSize = data.voxelSize * float(data.brickDim);
    int3 brickGrid = int3(data.gridDim / data.brickDim);

    float t = max(tEnter, 0.0);
    float eps = data.voxelSize * 1e-3;
    float3 p = ro + rd * (t + eps);
    int3 cell = clamp(int3(floor((p - bmin) / brickSize)), int3(0), brickGrid - 1);

    int3 stepDir = int3(sign(rd));
    float3 tDelta = abs(float3(brickSize) * invDir);
    float3 stepPos = select(float3(0.0), float3(1.0), rd > 0.0);
    float3 boundary = bmin + (float3(cell) + stepPos) * brickSize;
    float3 tMax = select(float3(INFINITY), (boundary - ro) * invDir, rd != 0.0);

    float3 normal = enterNormal;
    uint3 bgDim = uint3(brickGrid);

    for (uint i = 0; i < maxSteps; i++) {
        uint entry = brickIndexGrid[voxelIndex(cell, bgDim)];
        if (entry != VOXEL_BRICK_EMPTY) {
            if ((entry & VOXEL_BRICK_UNIFORM_FLAG) != 0) {
                // Fully solid brick: the ray hits its entry face directly.
                result.hit = true;
                result.t = t;
                result.normal = normal;
                result.material = entry & 0xFFu;
                return result;
            }
            RayHit hit = traverseBrick(ro, rd, invDir, t, cell, entry, normal, data, brickPool);
            if (hit.hit) return hit;
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x;
            tMax.x += tDelta.x;
            cell.x += stepDir.x;
            normal = float3(-float(stepDir.x), 0.0, 0.0);
            if (cell.x < 0 || cell.x >= brickGrid.x) break;
        } else if (tMax.y < tMax.z) {
            t = tMax.y;
            tMax.y += tDelta.y;
            cell.y += stepDir.y;
            normal = float3(0.0, -float(stepDir.y), 0.0);
            if (cell.y < 0 || cell.y >= brickGrid.y) break;
        } else {
            t = tMax.z;
            tMax.z += tDelta.z;
            cell.z += stepDir.z;
            normal = float3(0.0, 0.0, -float(stepDir.z));
            if (cell.z < 0 || cell.z >= brickGrid.z) break;
        }
        if (t > tExit) break;
    }
    return result;
}

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 fsTriVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut out;
    out.position = float4(fsTriVerts[vertexID], 0.0, 1.0);
    out.uv = fsTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;
    return out;
}

struct FragmentOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

fragment FragmentOut fragmentMain(
    VertexOut in [[stage_in]],
    constant VoxelVolumeData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    const device uchar* brickPool [[buffer(2)]],
    const device uint* brickIndexGrid [[buffer(3)]],
    constant float4* palette [[buffer(4)]]
) {
    FragmentOut out;

    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;// Metal Y-flip
    float4 farH = data.invViewProj * float4(ndc, 1.0, 1.0);
    float3 ro = data.cameraPosition;
    float3 rd = normalize(farH.xyz / farH.w - ro);

    RayHit hit = raycastVoxels(ro, rd, data, brickPool, brickIndexGrid, data.maxRaySteps);
    if (!hit.hit) {
        discard_fragment();
        out.color = float4(0.0);
        out.depth = 1.0;
        return out;
    }

    float3 hitPos = ro + rd * hit.t;
    int3 cell = int3(floor((hitPos + rd * data.voxelSize * 0.01 - data.volumeOrigin) / data.voxelSize));
    cell = clamp(cell, int3(0), int3(data.gridDim) - 1);

    float3 albedo = palette[hit.material].rgb;
    // Per-voxel value variation so individual micro voxels stay readable.
    albedo *= 0.85 + 0.3 * voxelHash(cell);

    float3 L = normalize(data.sunDirection);
    float ndl = max(dot(hit.normal, L), 0.0);

    float shadow = 1.0;
    if (data.shadowEnabled != 0 && ndl > 0.0) {
        float3 shadowOrigin = hitPos + hit.normal * data.voxelSize * 0.51;
        RayHit shadowHit = raycastVoxels(shadowOrigin, L, data, brickPool, brickIndexGrid, data.maxRaySteps);
        if (shadowHit.hit) shadow = 0.0;
    }

    // Simple hemisphere ambient: sky-tinted from above, ground bounce below.
    float3 skyAmbient = mix(float3(0.20, 0.22, 0.28), float3(0.45, 0.55, 0.75), hit.normal.y * 0.5 + 0.5);
    float3 direct = data.sunColor * data.sunIntensity * (1.0 / PI) * ndl * shadow;
    float3 color = albedo * (direct + skyAmbient * data.ambientIntensity);

    out.color = float4(color, 1.0);

    float4 clipPos = camera.proj * camera.view * float4(hitPos, 1.0);
    out.depth = clamp(clipPos.z / clipPos.w, 0.0, 0.999999);
    return out;
}
