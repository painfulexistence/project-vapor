#include "voxel_world.hpp"

#include <algorithm>
#include <cmath>

namespace Vapor {

// ============================================================================
// Procedural noise stack — ported verbatim from the Atmospheric MicroVoxel
// generator (itself descended from this engine's early Metal renderer) so a
// given seed reproduces the same terrain. Do not "clean up" the constants.
// ============================================================================

static float MvHashNoise(int x, int y, int z, Uint32 seed) {
    Uint32 h = static_cast<Uint32>(x) * 374761393u + static_cast<Uint32>(y) * 668265263u
        + static_cast<Uint32>(z) * 2246822519u + seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

static float MvValueNoise3(glm::vec3 p, Uint32 seed) {
    glm::vec3 pf = glm::floor(p);
    glm::vec3 f = p - pf;
    f = f * f * (3.0f - 2.0f * f);  // smoothstep
    int xi = static_cast<int>(pf.x), yi = static_cast<int>(pf.y), zi = static_cast<int>(pf.z);

    auto corner = [&](int dx, int dy, int dz) { return MvHashNoise(xi + dx, yi + dy, zi + dz, seed); };
    float c000 = corner(0, 0, 0), c100 = corner(1, 0, 0), c010 = corner(0, 1, 0), c110 = corner(1, 1, 0);
    float c001 = corner(0, 0, 1), c101 = corner(1, 0, 1), c011 = corner(0, 1, 1), c111 = corner(1, 1, 1);

    float x00 = glm::mix(c000, c100, f.x), x10 = glm::mix(c010, c110, f.x);
    float x01 = glm::mix(c001, c101, f.x), x11 = glm::mix(c011, c111, f.x);
    return glm::mix(glm::mix(x00, x10, f.y), glm::mix(x01, x11, f.y), f.z);
}

static float MvFbm3(glm::vec3 p, int octaves, Uint32 seed) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * MvValueNoise3(p, seed + static_cast<Uint32>(i) * 101u);
        p *= 2.0f;
        amp *= 0.5f;
    }
    return sum;  // ~[0, 1)
}

// Gradient (Perlin-style) noise for the terrain heightfield — value noise has
// axis-aligned plateaus that read as fake terrain.
static float MvGradDot(int xi, int yi, int zi, glm::vec3 offset, Uint32 seed) {
    glm::vec3 g(
        MvHashNoise(xi, yi, zi, seed) - 0.5f,
        MvHashNoise(xi, yi, zi, seed ^ 0x9E3779B9u) - 0.5f,
        MvHashNoise(xi, yi, zi, seed ^ 0x85EBCA6Bu) - 0.5f
    );
    float len = glm::length(g);
    if (len < 1e-6f) return offset.x;
    return glm::dot(g / len, offset);
}

static float MvGradNoise3(glm::vec3 p, Uint32 seed) {
    glm::vec3 pf = glm::floor(p);
    glm::vec3 f = p - pf;
    glm::vec3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);  // quintic fade
    int xi = static_cast<int>(pf.x), yi = static_cast<int>(pf.y), zi = static_cast<int>(pf.z);

    auto corner = [&](int dx, int dy, int dz) {
        return MvGradDot(xi + dx, yi + dy, zi + dz, f - glm::vec3(dx, dy, dz), seed);
    };
    float x00 = glm::mix(corner(0, 0, 0), corner(1, 0, 0), u.x);
    float x10 = glm::mix(corner(0, 1, 0), corner(1, 1, 0), u.x);
    float x01 = glm::mix(corner(0, 0, 1), corner(1, 0, 1), u.x);
    float x11 = glm::mix(corner(0, 1, 1), corner(1, 1, 1), u.x);
    return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);  // ~[-0.7, 0.7]
}

static float MvGradFbm01(glm::vec3 p, int octaves, Uint32 seed) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * MvGradNoise3(p, seed + static_cast<Uint32>(i) * 101u);
        p *= 2.0f;
        amp *= 0.5f;
    }
    return glm::clamp(0.5f + sum * 1.2f, 0.0f, 1.0f);
}

// ============================================================================

void VoxelWorld::configure(glm::ivec3 gridDimIn, float voxelSizeIn, Uint32 brickCapacityIn) {
    gridDim = glm::max((gridDimIn / BRICK_DIM) * BRICK_DIM, glm::ivec3(BRICK_DIM));
    voxelSize = voxelSizeIn;
    brickCapacity = std::max(brickCapacityIn, 1u);
}

void VoxelWorld::setDefaultPalette() {
    palette.fill(VoxelMaterial {});
    auto set = [this](Uint8 idx, Uint8 r, Uint8 g, Uint8 b, Uint8 emission = 0) {
        palette[idx].r = r;
        palette[idx].g = g;
        palette[idx].b = b;
        palette[idx].emission = emission;
    };
    auto setParams = [this](Uint8 idx, Uint8 reflectivity, Uint8 roughness = 0) {
        palette[idx].reflectivity = reflectivity;
        palette[idx].roughness = roughness;
    };
    // Dielectric transmission (the BTDF path): iorByte encodes actual IOR as
    // 1.0 + iorByte/255 (glass 1.5 -> 128, crystal 1.55 -> 140).
    auto setGlass = [this](Uint8 idx, Uint8 transmission, Uint8 iorByte) {
        palette[idx].transmission = transmission;
        palette[idx].ior = iorByte;
    };
    for (int i = 10; i < 256; i++) set(static_cast<Uint8>(i), 128, 128, 128);
    set(MatGrass, 64, 140, 46);
    set(MatDirt, 107, 77, 46);
    set(MatStone, 122, 122, 128);
    set(MatSnow, 235, 240, 250);
    set(MatSand, 204, 184, 122);
    set(MatOre, 242, 191, 64);
    set(MatCrystal, 115, 191, 242, 160);  // cool cyan glow (the original's look)
    set(MatGlow, 255, 140, 48, 255);      // warm glowstone, full emission
    set(MatWater, 56, 130, 196);          // deep blue; Beer tints what's below
    // Reflective materials: crystals stay the original near-mirror; ore and
    // snow catch a roughness-jittered sheen. Everything else stays matte.
    setParams(MatCrystal, 210, 20);
    setParams(MatOre, 90, 60);
    setParams(MatSnow, 40, 120);
    // Water is the transmission showcase: IOR 1.33 (byte 84), lightly rippled
    // via the glossy jitter. On the glass path Fresnel comes from the IOR, so
    // the reflectivity byte is left 0.
    setParams(MatWater, 0, 25);
    setGlass(MatWater, 215, 84);
}

float VoxelWorld::terrainHeight(int x, int z) const {
    const float baseH = 0.10f * static_cast<float>(gridDim.y);
    const float varH = 0.34f * static_cast<float>(gridDim.y);
    // Sample the heightfield in WORLD space, not per-voxel. The original's
    // tuned 0.010 frequency assumed 5 cm voxels; expressing it as a physical
    // 0.2/m and multiplying by voxelSize keeps a fixed ~5 m feature wavelength
    // at ANY resolution. Otherwise a finer voxel size (e.g. the 2.5 cm centre
    // diorama) packs the same feature count into half the ground and the
    // terrain reads as horizontally squished. baseH/varH already track
    // gridDim.y, so the vertical scale is resolution-independent; only the
    // horizontal frequency needed the fix. At 5 cm this is exactly 0.010, so
    // every 5 cm world (the side dioramas, --big, the tests) is unchanged.
    const float freq = 0.2f * voxelSize;
    float h01 = MvGradFbm01(glm::vec3(x, 0.0f, z) * freq, 5, seed);
    h01 = std::pow(h01, 1.6f);  // bias toward valleys with occasional peaks
    return baseH + h01 * varH;
}

glm::ivec2 VoxelWorld::columnChunkCount() const {
    return {
        (gridDim.x + GEN_CHUNK_DIM - 1) / GEN_CHUNK_DIM,
        (gridDim.z + GEN_CHUNK_DIM - 1) / GEN_CHUNK_DIM,
    };
}

void VoxelWorld::prepareGeneration(Uint32 seedIn) {
    seed = seedIn;
    setDefaultPalette();

    const glm::ivec3 bg = brickGrid();
    pageTable.assign(static_cast<size_t>(bg.x) * bg.y * bg.z, PAGE_EMPTY);
    bricks.clear();
    bricks.reserve(brickCapacity);  // slot writes must never relocate the pool
    freeSlots.clear();
    brickDirtyFlags.clear();
    dirtyBrickSlots.clear();
    solidCount.store(0, std::memory_order_relaxed);
    droppedCount.store(0, std::memory_order_relaxed);
    pageTableDirty = true;
    paletteDirty = true;

    // Feature placements, scaled so the density per footprint area matches the
    // original 256^2 diorama (which placed 4 crystals and 6 glowstone orbs).
    features.clear();
    const float footprintScale = static_cast<float>(gridDim.x) * static_cast<float>(gridDim.z) / (256.0f * 256.0f);
    const int crystalCount = std::max(4, static_cast<int>(4.0f * footprintScale));
    const int glowCount = std::max(6, static_cast<int>(6.0f * footprintScale));

    for (int s = 0; s < crystalCount; s++) {
        FeatureSphere f;
        f.center = glm::vec3(
            (0.15f + 0.7f * MvHashNoise(s, 1, 0, seed + 23u)) * static_cast<float>(gridDim.x),
            (0.70f + 0.22f * MvHashNoise(s, 2, 0, seed + 23u)) * static_cast<float>(gridDim.y),
            (0.15f + 0.7f * MvHashNoise(s, 3, 0, seed + 23u)) * static_cast<float>(gridDim.z)
        );
        f.radius = (0.02f + 0.03f * MvHashNoise(s, 4, 0, seed + 23u)) * static_cast<float>(gridDim.y);
        f.material = MatCrystal;
        features.push_back(f);
    }
    for (int g = 0; g < glowCount; g++) {
        const int gx = static_cast<int>((0.18f + 0.64f * MvHashNoise(g, 5, 0, seed + 31u)) * static_cast<float>(gridDim.x));
        const int gz = static_cast<int>((0.18f + 0.64f * MvHashNoise(g, 6, 0, seed + 31u)) * static_cast<float>(gridDim.z));
        if (gx < 0 || gz < 0 || gx >= gridDim.x || gz >= gridDim.z) continue;
        FeatureSphere f;
        // Rest just above the terrain surface, like the original.
        f.center = glm::vec3(gx, static_cast<int>(terrainHeight(gx, gz)) + 1, gz);
        f.radius = 2.0f;
        f.material = MatGlow;
        features.push_back(f);
    }
}

void VoxelWorld::generateColumnChunk(int chunkX, int chunkZ) {
    const int x0 = chunkX * GEN_CHUNK_DIM;
    const int z0 = chunkZ * GEN_CHUNK_DIM;
    if (x0 >= gridDim.x || z0 >= gridDim.z || x0 < 0 || z0 < 0) return;
    const int xw = std::min(GEN_CHUNK_DIM, gridDim.x - x0);
    const int zw = std::min(GEN_CHUNK_DIM, gridDim.z - z0);
    const int ny = gridDim.y;

    const float snowLine = (0.10f + 0.75f * 0.34f) * static_cast<float>(ny);
    const float sandLine = (0.10f + 0.12f * 0.34f) * static_cast<float>(ny);
    // Water fills valleys up to just under the sand line, so beaches ring it.
    const int waterLevel = static_cast<int>((0.10f + 0.08f * 0.34f) * static_cast<float>(ny));

    std::vector<float> heights(static_cast<size_t>(xw) * zw);
    for (int z = 0; z < zw; z++)
        for (int x = 0; x < xw; x++) heights[static_cast<size_t>(z) * xw + x] = terrainHeight(x0 + x, z0 + z);

    // Dense scratch for this column block only; index = x + y*xw + z*xw*ny.
    std::vector<Uint8> scratch(static_cast<size_t>(xw) * ny * zw, 0);
    auto at = [&](int x, int y, int z) -> Uint8& {
        return scratch[static_cast<size_t>(z) * xw * ny + static_cast<size_t>(y) * xw + x];
    };

    // Column fill: terrain crust/biomes, caves, ore — global coordinates feed
    // the noise so chunk boundaries are seamless.
    for (int z = 0; z < zw; z++) {
        for (int x = 0; x < xw; x++) {
            const float h = heights[static_cast<size_t>(z) * xw + x];
            const int top = static_cast<int>(h);
            const int gx = x0 + x, gz = z0 + z;
            for (int y = 0; y <= top && y < ny; y++) {
                if (y + 4 < top) {
                    const float cave = MvFbm3(glm::vec3(gx, y, gz) * 0.045f, 3, seed + 7u);
                    if (cave > 0.62f) continue;  // carved
                }
                Uint8 mat = MatStone;
                const int depth = top - y;
                if (depth == 0) {
                    mat = (h > snowLine) ? MatSnow : ((h < sandLine) ? MatSand : MatGrass);
                } else if (depth <= 3) {
                    mat = (h < sandLine) ? MatSand : MatDirt;
                } else if (MvHashNoise(gx, y, gz, seed + 13u) > 0.995f) {
                    mat = MatOre;
                }
                at(x, y, z) = mat;
            }
            // Water column: from the terrain surface up to the water level.
            // Water voxels are solid to the DDA; the shader's transmission
            // path refracts through them (Beer-tinted) to the bed below.
            for (int y = top + 1; y <= waterLevel && y < ny; y++) {
                at(x, y, z) = MatWater;
            }
        }
    }

    // Stamp the features (crystals, then glowstone — later spheres overwrite)
    // where they overlap this block. Same integer center / radius-test rules
    // as the original so the shapes match voxel-for-voxel.
    for (const FeatureSphere& f : features) {
        const glm::ivec3 c(static_cast<int>(f.center.x), static_cast<int>(f.center.y), static_cast<int>(f.center.z));
        const int r = (f.material == MatGlow) ? static_cast<int>(f.radius) : static_cast<int>(f.radius) + 1;
        const int lx0 = std::max(c.x - r, x0), lx1 = std::min(c.x + r, x0 + xw - 1);
        const int lz0 = std::max(c.z - r, z0), lz1 = std::min(c.z + r, z0 + zw - 1);
        const int ly0 = std::max(c.y - r, 0), ly1 = std::min(c.y + r, ny - 1);
        if (lx0 > lx1 || lz0 > lz1 || ly0 > ly1) continue;
        const int r2 = r * r;
        for (int gz = lz0; gz <= lz1; gz++) {
            for (int gy = ly0; gy <= ly1; gy++) {
                for (int gx = lx0; gx <= lx1; gx++) {
                    const int dx = gx - c.x, dy = gy - c.y, dz = gz - c.z;
                    if (f.material == MatGlow) {
                        if (dx * dx + dy * dy + dz * dz > r2) continue;
                    } else {
                        if (glm::length(glm::vec3(dx, dy, dz)) > f.radius) continue;
                    }
                    at(gx - x0, gy, gz - z0) = f.material;
                }
            }
        }
    }

    // Pack the scratch into pool bricks. Fully-empty bricks stay PAGE_EMPTY,
    // single-material bricks collapse to a uniform page entry (no pool cost).
    Uint64 chunkSolid = 0;
    const int bx0 = x0 / BRICK_DIM, bx1 = (x0 + xw) / BRICK_DIM;
    const int bz0 = z0 / BRICK_DIM, bz1 = (z0 + zw) / BRICK_DIM;
    const int by1 = ny / BRICK_DIM;
    Brick staged;
    for (int bz = bz0; bz < bz1; bz++) {
        for (int by = 0; by < by1; by++) {
            for (int bx = bx0; bx < bx1; bx++) {
                staged.occupancy.fill(0);
                int solid = 0;
                Uint8 uniformMat = 0;
                bool uniform = true;
                for (int z = 0; z < BRICK_DIM; z++) {
                    for (int y = 0; y < BRICK_DIM; y++) {
                        for (int x = 0; x < BRICK_DIM; x++) {
                            const Uint8 v = at(bx * BRICK_DIM + x - x0, by * BRICK_DIM + y, bz * BRICK_DIM + z - z0);
                            const int i = voxelIndexInBrick({ x, y, z });
                            staged.materials[i] = v;
                            if (v != 0) {
                                staged.occupancy[static_cast<size_t>(i) >> 5] |= 1u << (static_cast<Uint32>(i) & 31u);
                                solid++;
                                if (uniformMat == 0) uniformMat = v;
                                else if (uniformMat != v) uniform = false;
                            } else {
                                uniform = false;  // any air voxel breaks uniformity
                            }
                        }
                    }
                }
                if (solid == 0) continue;  // page entry already PAGE_EMPTY
                const size_t page = pageIndex({ bx, by, bz });
                if (uniform && solid == BRICK_VOXELS) {
                    pageTable[page] = PAGE_UNIFORM_BIT | uniformMat;
                    chunkSolid += BRICK_VOXELS;
                    continue;
                }
                std::lock_guard<std::mutex> lock(poolMutex);
                const Uint32 slot = allocSlotLocked();
                if (slot == PAGE_EMPTY) {
                    droppedCount.fetch_add(1, std::memory_order_relaxed);
                    continue;  // pool exhausted: this brick's voxels are dropped
                }
                bricks[slot] = staged;
                pageTable[page] = slot;
                markBrickDirtyLocked(slot);
                chunkSolid += static_cast<Uint64>(solid);
            }
        }
    }
    solidCount.fetch_add(chunkSolid, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        pageTableDirty = true;
    }
}

void VoxelWorld::generate(Uint32 seedIn) {
    prepareGeneration(seedIn);
    const glm::ivec2 chunks = columnChunkCount();
    for (int cz = 0; cz < chunks.y; cz++)
        for (int cx = 0; cx < chunks.x; cx++) generateColumnChunk(cx, cz);
}

// ============================================================================
// Queries & editing
// ============================================================================

Uint8 VoxelWorld::voxelAt(const glm::ivec3& cell) const {
    if (!isInside(cell)) return 0;
    const Uint32 entry = pageTable[pageIndex(cell / BRICK_DIM)];
    if (entry == PAGE_EMPTY) return 0;
    if (entry & PAGE_UNIFORM_BIT) return static_cast<Uint8>(entry & 0xFFu);
    return bricks[entry].materials[voxelIndexInBrick(cell % BRICK_DIM)];
}

Uint32 VoxelWorld::residentBricks() const {
    std::lock_guard<std::mutex> lock(poolMutex);
    return static_cast<Uint32>(bricks.size() - freeSlots.size());
}

Uint32 VoxelWorld::allocSlotLocked() {
    if (!freeSlots.empty()) {
        const Uint32 slot = freeSlots.back();
        freeSlots.pop_back();
        return slot;
    }
    if (bricks.size() >= brickCapacity) return PAGE_EMPTY;
    bricks.emplace_back();
    brickDirtyFlags.push_back(false);
    return static_cast<Uint32>(bricks.size() - 1);
}

void VoxelWorld::freeSlotLocked(Uint32 slot) {
    freeSlots.push_back(slot);
}

void VoxelWorld::markBrickDirtyLocked(Uint32 slot) {
    if (slot >= brickDirtyFlags.size() || brickDirtyFlags[slot]) return;
    brickDirtyFlags[slot] = true;
    dirtyBrickSlots.push_back(slot);
}

void VoxelWorld::markPaletteDirty() {
    std::lock_guard<std::mutex> lock(poolMutex);
    paletteDirty = true;
}

Uint32 VoxelWorld::resolveForEdit(const glm::ivec3& brickCell) {
    const size_t page = pageIndex(brickCell);
    const Uint32 entry = pageTable[page];
    if (entry == PAGE_EMPTY) return PAGE_EMPTY;
    if (!(entry & PAGE_UNIFORM_BIT)) return entry;
    // Materialize a uniform brick into a pool slot so it can be edited.
    const Uint8 mat = static_cast<Uint8>(entry & 0xFFu);
    std::lock_guard<std::mutex> lock(poolMutex);
    const Uint32 slot = allocSlotLocked();
    if (slot == PAGE_EMPTY) {
        droppedCount.fetch_add(1, std::memory_order_relaxed);
        return PAGE_EMPTY;
    }
    Brick& b = bricks[slot];
    b.occupancy.fill(0xFFFFFFFFu);
    b.materials.fill(mat);
    pageTable[page] = slot;
    pageTableDirty = true;
    markBrickDirtyLocked(slot);
    return slot;
}

bool VoxelWorld::carveSphere(const glm::vec3& localCenter, float radius) {
    if (pageTable.empty() || radius <= 0.0f) return false;
    const glm::vec3 c = localCenter / voxelSize;  // sphere center in voxels
    const float rv = radius / voxelSize;
    const glm::ivec3 lo = glm::clamp(glm::ivec3(glm::floor(c - rv)), glm::ivec3(0), gridDim - 1);
    const glm::ivec3 hi = glm::clamp(glm::ivec3(glm::ceil(c + rv)), glm::ivec3(0), gridDim - 1);
    const float rv2 = rv * rv;

    bool changed = false;
    Uint64 removed = 0;
    const glm::ivec3 blo = lo / BRICK_DIM;
    const glm::ivec3 bhi = hi / BRICK_DIM;
    for (int bz = blo.z; bz <= bhi.z; bz++) {
        for (int by = blo.y; by <= bhi.y; by++) {
            for (int bx = blo.x; bx <= bhi.x; bx++) {
                const glm::ivec3 brickCell(bx, by, bz);
                // Does the sphere actually reach any voxel of this brick?
                const glm::vec3 bmin = glm::vec3(brickCell * BRICK_DIM);
                const glm::vec3 bmax = bmin + glm::vec3(BRICK_DIM);
                const glm::vec3 nearest = glm::clamp(c, bmin, bmax);
                const glm::vec3 d = nearest - c;
                if (glm::dot(d, d) > rv2) continue;

                const Uint32 slot = resolveForEdit(brickCell);
                if (slot == PAGE_EMPTY) continue;  // air (or unmaterializable)
                Brick& b = bricks[slot];

                bool brickChanged = false;
                const glm::ivec3 vlo = glm::max(lo, brickCell * BRICK_DIM);
                const glm::ivec3 vhi = glm::min(hi, brickCell * BRICK_DIM + (BRICK_DIM - 1));
                for (int z = vlo.z; z <= vhi.z; z++) {
                    for (int y = vlo.y; y <= vhi.y; y++) {
                        for (int x = vlo.x; x <= vhi.x; x++) {
                            const glm::vec3 dd = glm::vec3(x, y, z) + 0.5f - c;
                            if (glm::dot(dd, dd) > rv2) continue;
                            const int i = voxelIndexInBrick(glm::ivec3(x, y, z) % BRICK_DIM);
                            const Uint32 word = static_cast<Uint32>(i) >> 5, bit = 1u << (static_cast<Uint32>(i) & 31u);
                            if (b.occupancy[word] & bit) {
                                b.occupancy[word] &= ~bit;
                                b.materials[i] = 0;
                                removed++;
                                brickChanged = true;
                            }
                        }
                    }
                }
                if (!brickChanged) continue;
                changed = true;

                bool empty = true;
                for (Uint32 w : b.occupancy)
                    if (w != 0) {
                        empty = false;
                        break;
                    }
                std::lock_guard<std::mutex> lock(poolMutex);
                if (empty) {
                    pageTable[pageIndex(brickCell)] = PAGE_EMPTY;
                    pageTableDirty = true;
                    freeSlotLocked(slot);
                } else {
                    markBrickDirtyLocked(slot);
                }
            }
        }
    }
    if (removed > 0) solidCount.fetch_sub(removed, std::memory_order_relaxed);
    return changed;
}

// ============================================================================
// Raycast — the CPU reference of the shader's traversal: a coarse DDA over
// brick cells (page-table lookups; empty cells cost one step) descending into
// a fine DDA over the occupancy bitmask inside occupied bricks. Uniform
// bricks hit immediately at their entry point.
// ============================================================================

bool VoxelWorld::raycast(const glm::vec3& localRo, const glm::vec3& localRd, float maxDist,
                         glm::vec3& outLocalHit, glm::ivec3& outCell) const {
    if (pageTable.empty()) return false;
    const glm::vec3 bmax = extent();
    const glm::vec3 rd = localRd;
    const glm::vec3 invD = 1.0f / rd;  // view rays are never exactly axis-aligned

    const glm::vec3 t0 = (glm::vec3(0.0f) - localRo) * invD;
    const glm::vec3 t1 = (bmax - localRo) * invD;
    const glm::vec3 tsmall = glm::min(t0, t1);
    const glm::vec3 tbig = glm::max(t0, t1);
    const float tEnter = glm::max(glm::max(tsmall.x, tsmall.y), tsmall.z);
    const float tExit = glm::min(glm::min(tbig.x, tbig.y), tbig.z);
    if (tExit < glm::max(tEnter, 0.0f)) return false;

    const float eps = voxelSize * 1e-3f;
    float t = glm::max(tEnter, 0.0f) + eps;
    if (t > maxDist) return false;

    const float brickSize = voxelSize * BRICK_DIM;
    const glm::ivec3 bg = brickGrid();
    glm::ivec3 bcell = glm::clamp(glm::ivec3(glm::floor((localRo + rd * t) / brickSize)), glm::ivec3(0), bg - 1);

    const glm::ivec3 step = glm::ivec3(glm::sign(rd));
    const glm::vec3 tDelta = glm::abs(glm::vec3(brickSize) * invD);
    const glm::vec3 stepPos = glm::vec3(glm::greaterThan(rd, glm::vec3(0.0f)));
    glm::vec3 tMax = ((glm::vec3(bcell) + stepPos) * brickSize - localRo) * invD;

    // Fine walk of one occupied brick's bitmask, starting at ray parameter tIn.
    auto walkBrick = [&](const glm::ivec3& brickCell, const Brick& b, float tIn, float& tHit,
                         glm::ivec3& cellHit) -> bool {
        const glm::ivec3 lo = brickCell * BRICK_DIM;
        glm::ivec3 cell =
            glm::clamp(glm::ivec3(glm::floor((localRo + rd * (tIn + eps)) / voxelSize)), lo, lo + (BRICK_DIM - 1));
        const glm::vec3 vDelta = glm::abs(glm::vec3(voxelSize) * invD);
        glm::vec3 vMax = ((glm::vec3(cell) + stepPos) * voxelSize - localRo) * invD;
        float tv = tIn;
        for (int i = 0; i < 3 * BRICK_DIM + 1; i++) {
            const int vi = voxelIndexInBrick(cell - lo);
            if (occupancyBit(b, vi)) {
                tHit = tv;
                cellHit = cell;
                return true;
            }
            if (vMax.x < vMax.y && vMax.x < vMax.z) {
                tv = vMax.x;
                vMax.x += vDelta.x;
                cell.x += step.x;
                if (cell.x < lo.x || cell.x >= lo.x + BRICK_DIM) return false;
            } else if (vMax.y < vMax.z) {
                tv = vMax.y;
                vMax.y += vDelta.y;
                cell.y += step.y;
                if (cell.y < lo.y || cell.y >= lo.y + BRICK_DIM) return false;
            } else {
                tv = vMax.z;
                vMax.z += vDelta.z;
                cell.z += step.z;
                if (cell.z < lo.z || cell.z >= lo.z + BRICK_DIM) return false;
            }
            if (tv > maxDist || tv > tExit) return false;
        }
        return false;
    };

    const int maxSteps = 3 * std::max(bg.x, std::max(bg.y, bg.z)) + 1;
    for (int i = 0; i < maxSteps; i++) {
        const Uint32 entry = pageTable[pageIndex(bcell)];
        if (entry != PAGE_EMPTY) {
            if (entry & PAGE_UNIFORM_BIT) {
                outLocalHit = localRo + rd * t;
                outCell = glm::clamp(glm::ivec3(glm::floor(outLocalHit / voxelSize)), bcell * BRICK_DIM,
                                     bcell * BRICK_DIM + (BRICK_DIM - 1));
                return t <= maxDist;
            }
            float tHit;
            glm::ivec3 cellHit;
            if (walkBrick(bcell, bricks[entry], t, tHit, cellHit)) {
                outLocalHit = localRo + rd * tHit;
                outCell = cellHit;
                return tHit <= maxDist;
            }
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x;
            tMax.x += tDelta.x;
            bcell.x += step.x;
            if (bcell.x < 0 || bcell.x >= bg.x) break;
        } else if (tMax.y < tMax.z) {
            t = tMax.y;
            tMax.y += tDelta.y;
            bcell.y += step.y;
            if (bcell.y < 0 || bcell.y >= bg.y) break;
        } else {
            t = tMax.z;
            tMax.z += tDelta.z;
            bcell.z += step.z;
            if (bcell.z < 0 || bcell.z >= bg.z) break;
        }
        if (t > maxDist || t > tExit) break;
    }
    return false;
}

VoxelWorld::DirtyBatch VoxelWorld::takeDirty() {
    std::lock_guard<std::mutex> lock(poolMutex);
    DirtyBatch batch;
    batch.pageTable = pageTableDirty;
    batch.palette = paletteDirty;
    batch.brickSlots = std::move(dirtyBrickSlots);
    dirtyBrickSlots.clear();
    for (Uint32 slot : batch.brickSlots) brickDirtyFlags[slot] = false;
    pageTableDirty = false;
    paletteDirty = false;
    return batch;
}

bool VoxelWorld::hasDirty() const {
    std::lock_guard<std::mutex> lock(poolMutex);
    return pageTableDirty || paletteDirty || !dirtyBrickSlots.empty();
}

}  // namespace Vapor
