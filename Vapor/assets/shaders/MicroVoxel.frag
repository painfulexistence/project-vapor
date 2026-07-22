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
// Voxel G-buffer, consumed by the GI passes (MicroVoxelGI.comp) so they never
// re-trace primary visibility (the original GL implementation re-marched every
// GI pixel), and by the GI composite for albedo/AO.
layout(location = 1) out float outHitT;        // camera distance along the ray; 0 = miss
layout(location = 2) out vec4 outAlbedoAO;     // rgb = hash-varied albedo, a = corner AO
layout(location = 3) out vec4 outNormalMat;    // r = face-normal index/255, g = material/255, b = volumeIndex/255

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
    vec4 params;           // x = aoStrength, y = debugMode, z = reflectionsEnabled, w = giStrength
    vec4 extra0;           // x = volumeIndex, y = pageTableOffset, z = brickPoolBase, w = paletteBase
};
layout(std430, set = 1, binding = 1) readonly buffer PageBuf { uint pageTable[]; };
layout(std430, set = 1, binding = 2) readonly buffer BrickBuf { uint brickPool[]; };
// 256 materials, 2 words each: word0 = R|G<<8|B<<16|emission<<24,
// word1 = reflectivity|roughness<<8|transmission<<16|ior<<24
// (VoxelMaterial's byte layout; ior decodes to 1.0 + b/255).
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

// The shared buffers hold every volume's data; this volume's ranges start at
// the offsets carried in extra0 (y = page entries, z = pool slots, w =
// palette entries).
uint pageEntry(ivec3 bcell) {
    ivec3 bg = brickGrid();
    return pageTable[uint(extra0.y) + uint((bcell.z * bg.y + bcell.y) * bg.x + bcell.x)];
}

int voxelIndexInBrick(ivec3 local) {
    return local.x + local.y * BRICK_DIM + local.z * BRICK_DIM * BRICK_DIM;
}

bool brickOccupied(uint slot, int i) {
    uint g = uint(extra0.z) + slot;
    return (brickPool[g * BRICK_WORDS + uint(i >> 5)] & (1u << (uint(i) & 31u))) != 0u;
}

uint brickMaterial(uint slot, int i) {
    uint g = uint(extra0.z) + slot;
    uint word = brickPool[g * BRICK_WORDS + 16u + uint(i >> 2)];
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

void decodeMaterial(uint mat, out vec3 albedo, out float emission, out float reflectivity,
                    out float roughness, out float transmission, out float ior) {
    uint base = (uint(extra0.w) + mat) * 2u;
    uint w0 = palette[base];
    uint w1 = palette[base + 1u];
    albedo = vec3(float(w0 & 0xFFu), float((w0 >> 8u) & 0xFFu), float((w0 >> 16u) & 0xFFu)) / 255.0;
    emission = float((w0 >> 24u) & 0xFFu) / 255.0;
    reflectivity = float(w1 & 0xFFu) / 255.0;
    roughness = float((w1 >> 8u) & 0xFFu) / 255.0;
    transmission = float((w1 >> 16u) & 0xFFu) / 255.0;
    ior = 1.0 + float((w1 >> 24u) & 0xFFu) / 255.0;  // encoded range 1.0..2.0
}

// Transmission byte of a cell's material, without the full decode — the glass
// march below asks this per voxel to know when it leaves the medium.
float voxelTransmission(uint mat) {
    if (mat == 0u) return -1.0;  // air, not a medium
    return float((palette[(uint(extra0.w) + mat) * 2u + 1u] >> 16u) & 0xFFu) / 255.0;
}

// Interleaved gradient noise — a stable per-pixel [0,1) that trades temporal
// sizzle for a fixed dither pattern (there is no temporal accumulation over
// the primary pass, so a static pattern reads better than white noise).
float mvIGN(vec2 px) {
    return fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y));
}

// Glossy jitter: tilt `dir` inside a roughness^2-scaled cone (the byte the
// palette always reserved for this). Clamped back above the surface so a
// jittered reflection never dives through its own face.
vec3 glossyDir(vec3 dir, vec3 faceN, float roughness, vec2 px) {
    if (roughness < 0.02) return dir;
    float u1 = mvIGN(px) - 0.5;
    float u2 = mvIGN(px + vec2(17.0, 31.0)) - 0.5;
    vec3 t1 = normalize(cross(dir, abs(dir.y) < 0.98 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(dir, t1);
    float cone = roughness * roughness * 0.6;
    vec3 j = normalize(dir + (t1 * u1 + t2 * u2) * cone);
    float below = dot(j, faceN);
    if (below < 0.02) j = normalize(j + faceN * (0.02 - below));
    return j;
}

// Radiance along a secondary ray (reflection or the scene behind glass):
// opaque hit shaded with the same stylized sun + sky + emission the primary
// surface uses; miss returns sky. One bounce only — secondary hits do not
// recurse into their own reflections/transmission.
vec3 secondaryRadiance(vec3 ro, vec3 rd) {
    Hit h;
    if (raycast(ro, rd, 1e9, h)) {
        vec3 a; float e, refl, rough, trans, ior;
        decodeMaterial(h.mat, a, e, refl, rough, trans, ior);
        float ndl = max(dot(h.normal, sunDirection.xyz), 0.0);
        return a * (sunColor.xyz * sunColor.w * INV_PI * ndl + skyRadiance(h.normal))
             + a * e * gridDim.w;
    }
    return skyRadiance(rd);
}

// The BTDF: march THROUGH the transmissive medium voxel-by-voxel from the
// entry hit, accumulating the in-glass path length for Beer-Lambert, then
// bend out at the first glass->air face (total internal reflection continues
// straight — the one-bounce approximation) and gather the scene behind with
// the normal two-level DDA. An opaque voxel inside the medium terminates the
// march and is shaded directly. `mediumAlbedo` tints the absorption.
vec3 transmitRadiance(vec3 entryPos, vec3 tdir, float ior, vec3 mediumAlbedo, float voxelSize) {
    const int MAX_GLASS_STEPS = 128;  // 6.4 m of glass at 5 cm voxels
    const float BEER_K = 3.0;         // absorption strength per meter

    // Voxel-level DDA inside the medium (brick skipping is useless here — every
    // step is inside solid transmissive voxels).
    vec3 p = entryPos + tdir * (voxelSize * 1e-3);
    ivec3 cell = ivec3(floor(p / voxelSize));
    ivec3 stepDir = ivec3(sign(tdir));
    vec3 invD = 1.0 / tdir;
    vec3 tDelta = abs(vec3(voxelSize) * invD);
    vec3 stepPos = vec3(greaterThan(tdir, vec3(0.0)));
    vec3 tMax = ((vec3(cell) + stepPos) * voxelSize - entryPos) * invD;
    float t = 0.0;
    vec3 exitN = vec3(0.0);

    for (int i = 0; i < MAX_GLASS_STEPS; i++) {
        // Advance to the next voxel boundary.
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x; tMax.x += tDelta.x; cell.x += stepDir.x;
            exitN = vec3(-float(stepDir.x), 0.0, 0.0);
        } else if (tMax.y < tMax.z) {
            t = tMax.y; tMax.y += tDelta.y; cell.y += stepDir.y;
            exitN = vec3(0.0, -float(stepDir.y), 0.0);
        } else {
            t = tMax.z; tMax.z += tDelta.z; cell.z += stepDir.z;
            exitN = vec3(0.0, 0.0, -float(stepDir.z));
        }
        uint mat = voxelMat(cell);
        float trans = voxelTransmission(mat);
        if (trans > 0.001) continue;  // still inside the medium (stacked glass blends)

        vec3 beer = exp(-(vec3(1.0) - mediumAlbedo) * BEER_K * t);
        if (mat != 0u) {
            // Opaque voxel embedded in / behind the glass: shade it at this face.
            vec3 a; float e, refl, rough, tr, io;
            decodeMaterial(mat, a, e, refl, rough, tr, io);
            float ndl = max(dot(exitN, sunDirection.xyz), 0.0);
            vec3 lit = a * (sunColor.xyz * sunColor.w * INV_PI * ndl + skyRadiance(exitN))
                     + a * e * gridDim.w;
            return lit * beer;
        }
        // Glass -> air: bend out (eta = ior/1.0). TIR keeps the direction — the
        // cheap approximation instead of bouncing back into the medium.
        vec3 outDir = refract(tdir, exitN, ior);
        if (dot(outDir, outDir) < 1e-6) outDir = tdir;
        vec3 exitPos = entryPos + tdir * t;
        return secondaryRadiance(exitPos + outDir * (voxelSize * 0.51), outDir) * beer;
    }
    // Step cap: medium thicker than the budget — fully absorbed sky.
    return skyRadiance(tdir) * exp(-(vec3(1.0) - mediumAlbedo) * BEER_K * t);
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
    float emission, reflectivity, roughness, transmission, ior;
    decodeMaterial(hit.mat, albedo, emission, reflectivity, roughness, transmission, ior);
    vec3 rawAlbedo = albedo;  // pre-hash palette color; tints Beer absorption
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
    // With GI on (params.w > 0) the traced-GI composite pass supplies the
    // indirect term (albedo * gi * ao); the flat sky ambient is the fallback.
    vec3 indirect = (params.w > 0.0) ? vec3(0.0) : skyRadiance(hit.normal);

    // AO fully attenuates the ambient term but only 30% of direct sun, so
    // corners stay readable in full sunlight (the original's stylized mix).
    vec3 color = albedo * (direct * (0.7 + 0.3 * ao) + indirect * ao);

    // Secondary rays (params.z toggles both): roughness-jittered glossy
    // reflection, and for transmissive palette entries the full BTDF —
    // Fresnel-split reflection + refraction with Beer-Lambert absorption.
    if (params.z > 0.5 && (transmission > 0.001 || reflectivity > 0.001)) {
        vec2 px = gl_FragCoord.xy;
        float cosI = max(dot(-rd, hit.normal), 0.0);
        vec3 rdir = glossyDir(reflect(rd, hit.normal), hit.normal, roughness, px);
        vec3 refl = secondaryRadiance(hitLocal + hit.normal * voxelSize * 0.51, rdir);
        if (transmission > 0.001) {
            // Dielectric: Fresnel from the IOR splits the energy.
            float f0 = (ior - 1.0) / (ior + 1.0);
            f0 *= f0;
            float F = f0 + (1.0 - f0) * pow(1.0 - cosI, 5.0);
            vec3 tdir = refract(rd, hit.normal, 1.0 / ior);
            vec3 trans;
            if (dot(tdir, tdir) < 1e-6) {
                trans = refl;  // grazing entry TIR: everything reflects
            } else {
                tdir = glossyDir(tdir, -hit.normal, roughness, px + vec2(7.0, 13.0));  // frosted
                trans = transmitRadiance(hitLocal, tdir, ior, rawAlbedo, voxelSize);
            }
            vec3 glass = F * refl + (1.0 - F) * trans;
            color = mix(color, glass, transmission);
        } else {
            float f = reflectivity + (1.0 - reflectivity) * pow(1.0 - cosI, 5.0);
            color = mix(color, refl, clamp(f, 0.0, 1.0));
        }
    }
    // Emission after the secondary mix: an emissive surface glows regardless of
    // how reflective/transmissive it is (the crystal keeps its inner light).
    color += albedo * emission * gridDim.w;

    // Debug views: 1=albedo 2=normal 3=ao 4=shadow 6=material index.
    int debugMode = int(params.y);
    if (debugMode == 1) color = albedo;
    else if (debugMode == 2) color = hit.normal * 0.5 + 0.5;
    else if (debugMode == 3) color = vec3(ao);
    else if (debugMode == 4) color = vec3(shadow);
    else if (debugMode == 6) color = vec3(float(hit.mat) / 8.0);

    outColor = vec4(color, 1.0);

    // Voxel G-buffer. Face-normal index: axis*2 + (negative ? 1 : 0).
    int normalIdx = (hit.normal.x != 0.0) ? (hit.normal.x < 0.0 ? 1 : 0)
                  : (hit.normal.y != 0.0) ? (hit.normal.y < 0.0 ? 3 : 2)
                                          : (hit.normal.z < 0.0 ? 5 : 4);
    outHitT = hit.t;
    outAlbedoAO = vec4(albedo, ao);
    outNormalMat = vec4(float(normalIdx) / 255.0, float(hit.mat) / 255.0, extra0.x / 255.0, 0.0);

    // True hit depth so the hardware depth test composites voxels with the
    // raster scene (and later sky/fog/cloud passes see voxel depth). Vulkan
    // NDC z is already [0,1] — no GL-style remap. The 0.999999 clamp keeps
    // hits in front of the far-plane sky.
    vec4 clip = viewProj * vec4(hitWorld, 1.0);
    gl_FragDepth = clamp(clip.z / clip.w, 0.0, 0.999999);
}
