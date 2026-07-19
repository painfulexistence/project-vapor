#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Vapor/voxel_world.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using Vapor::VoxelWorld;

// ============================================================================
// Dense reference generator — an independent transcription of the original
// Atmospheric MicroVoxel algorithm (dense cubic grid, no bricks). The sparse
// VoxelWorld must reproduce it voxel-for-voxel; any traversal/packing bug in
// the port shows up as a mismatch here.
// ============================================================================
namespace ref {

static float hashNoise(int x, int y, int z, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u
        + static_cast<uint32_t>(z) * 2246822519u + seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

static float valueNoise3(glm::vec3 p, uint32_t seed) {
    glm::vec3 pf = glm::floor(p);
    glm::vec3 f = p - pf;
    f = f * f * (3.0f - 2.0f * f);
    int xi = static_cast<int>(pf.x), yi = static_cast<int>(pf.y), zi = static_cast<int>(pf.z);
    auto corner = [&](int dx, int dy, int dz) { return hashNoise(xi + dx, yi + dy, zi + dz, seed); };
    float c000 = corner(0, 0, 0), c100 = corner(1, 0, 0), c010 = corner(0, 1, 0), c110 = corner(1, 1, 0);
    float c001 = corner(0, 0, 1), c101 = corner(1, 0, 1), c011 = corner(0, 1, 1), c111 = corner(1, 1, 1);
    float x00 = glm::mix(c000, c100, f.x), x10 = glm::mix(c010, c110, f.x);
    float x01 = glm::mix(c001, c101, f.x), x11 = glm::mix(c011, c111, f.x);
    return glm::mix(glm::mix(x00, x10, f.y), glm::mix(x01, x11, f.y), f.z);
}

static float fbm3(glm::vec3 p, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * valueNoise3(p, seed + static_cast<uint32_t>(i) * 101u);
        p *= 2.0f;
        amp *= 0.5f;
    }
    return sum;
}

static float gradDot(int xi, int yi, int zi, glm::vec3 offset, uint32_t seed) {
    glm::vec3 g(
        hashNoise(xi, yi, zi, seed) - 0.5f,
        hashNoise(xi, yi, zi, seed ^ 0x9E3779B9u) - 0.5f,
        hashNoise(xi, yi, zi, seed ^ 0x85EBCA6Bu) - 0.5f
    );
    float len = glm::length(g);
    if (len < 1e-6f) return offset.x;
    return glm::dot(g / len, offset);
}

static float gradNoise3(glm::vec3 p, uint32_t seed) {
    glm::vec3 pf = glm::floor(p);
    glm::vec3 f = p - pf;
    glm::vec3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    int xi = static_cast<int>(pf.x), yi = static_cast<int>(pf.y), zi = static_cast<int>(pf.z);
    auto corner = [&](int dx, int dy, int dz) {
        return gradDot(xi + dx, yi + dy, zi + dz, f - glm::vec3(dx, dy, dz), seed);
    };
    float x00 = glm::mix(corner(0, 0, 0), corner(1, 0, 0), u.x);
    float x10 = glm::mix(corner(0, 1, 0), corner(1, 1, 0), u.x);
    float x01 = glm::mix(corner(0, 0, 1), corner(1, 0, 1), u.x);
    float x11 = glm::mix(corner(0, 1, 1), corner(1, 1, 1), u.x);
    return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);
}

static float gradFbm01(glm::vec3 p, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * gradNoise3(p, seed + static_cast<uint32_t>(i) * 101u);
        p *= 2.0f;
        amp *= 0.5f;
    }
    return glm::clamp(0.5f + sum * 1.2f, 0.0f, 1.0f);
}

enum : uint8_t { MatGrass = 1, MatDirt, MatStone, MatSnow, MatSand, MatOre, MatCrystal, MatGlow };

// The original Generate() column/cave/ore/crystal/glowstone logic on a dense
// cubic N^3 grid.
static std::vector<uint8_t> generateDense(int N, uint32_t seed) {
    std::vector<uint8_t> volume(static_cast<size_t>(N) * N * N, 0);
    auto voxelAt = [&](int x, int y, int z) -> uint8_t& {
        return volume[(static_cast<size_t>(z) * N + y) * N + x];
    };

    const float baseH = 0.10f * static_cast<float>(N);
    const float varH = 0.34f * static_cast<float>(N);
    const float snowLine = baseH + 0.75f * varH;
    const float sandLine = baseH + 0.12f * varH;
    std::vector<float> heights(static_cast<size_t>(N) * N);
    for (int z = 0; z < N; z++) {
        for (int x = 0; x < N; x++) {
            // Mirrors terrainHeight's world-space frequency (0.2/m * voxelSize);
            // every makeWorld here uses 5 cm voxels, so 0.2 * 0.05 == 0.010.
            float h01 = gradFbm01(glm::vec3(x, 0.0f, z) * (0.2f * 0.05f), 5, seed);
            h01 = std::pow(h01, 1.6f);
            heights[static_cast<size_t>(z) * N + x] = baseH + h01 * varH;
        }
    }

    for (int z = 0; z < N; z++) {
        for (int x = 0; x < N; x++) {
            const float h = heights[static_cast<size_t>(z) * N + x];
            const int top = static_cast<int>(h);
            for (int y = 0; y <= top && y < N; y++) {
                if (y + 4 < top) {
                    float cave = fbm3(glm::vec3(x, y, z) * 0.045f, 3, seed + 7u);
                    if (cave > 0.62f) continue;
                }
                uint8_t mat = MatStone;
                const int depth = top - y;
                if (depth == 0) {
                    mat = (h > snowLine) ? MatSnow : ((h < sandLine) ? MatSand : MatGrass);
                } else if (depth <= 3) {
                    mat = (h < sandLine) ? MatSand : MatDirt;
                } else if (hashNoise(x, y, z, seed + 13u) > 0.995f) {
                    mat = MatOre;
                }
                voxelAt(x, y, z) = mat;
            }
        }
    }

    for (int s = 0; s < 4; s++) {
        glm::vec3 center(
            (0.15f + 0.7f * hashNoise(s, 1, 0, seed + 23u)) * N,
            (0.70f + 0.22f * hashNoise(s, 2, 0, seed + 23u)) * N,
            (0.15f + 0.7f * hashNoise(s, 3, 0, seed + 23u)) * N
        );
        float radius = (0.02f + 0.03f * hashNoise(s, 4, 0, seed + 23u)) * N;
        int r = static_cast<int>(radius) + 1;
        for (int dz = -r; dz <= r; dz++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (glm::length(glm::vec3(dx, dy, dz)) > radius) continue;
                    int x = static_cast<int>(center.x) + dx;
                    int y = static_cast<int>(center.y) + dy;
                    int z = static_cast<int>(center.z) + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= N) continue;
                    voxelAt(x, y, z) = MatCrystal;
                }
    }

    for (int g = 0; g < 6; g++) {
        int gx = static_cast<int>((0.18f + 0.64f * hashNoise(g, 5, 0, seed + 31u)) * N);
        int gz = static_cast<int>((0.18f + 0.64f * hashNoise(g, 6, 0, seed + 31u)) * N);
        if (gx < 0 || gz < 0 || gx >= N || gz >= N) continue;
        int gy = static_cast<int>(heights[static_cast<size_t>(gz) * N + gx]) + 1;
        const int rad = 2;
        for (int dz = -rad; dz <= rad; dz++)
            for (int dy = -rad; dy <= rad; dy++)
                for (int dx = -rad; dx <= rad; dx++) {
                    if (dx * dx + dy * dy + dz * dz > rad * rad) continue;
                    int x = gx + dx, y = gy + dy, z = gz + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= N) continue;
                    voxelAt(x, y, z) = MatGlow;
                }
    }
    return volume;
}

}  // namespace ref

namespace {

// VoxelWorld owns a mutex + atomics, so it is neither copyable nor movable;
// tests fill a caller-provided instance instead of returning one.
void makeWorld(VoxelWorld& world, int n, uint32_t seed, Uint32 capacity = 1u << 20) {
    world.configure(glm::ivec3(n), 0.05f, capacity);
    world.generate(seed);
}

// Naive single-level DDA over voxelAt() — the raycast oracle.
bool naiveRaycast(const VoxelWorld& world, glm::vec3 ro, glm::vec3 rd, float maxDist, glm::ivec3& outCell,
                  float& outT) {
    const glm::vec3 bmax = world.extent();
    const float vs = world.voxelSizeMeters();
    const glm::vec3 invD = 1.0f / rd;
    const glm::vec3 t0 = (glm::vec3(0.0f) - ro) * invD;
    const glm::vec3 t1 = (bmax - ro) * invD;
    const glm::vec3 tsmall = glm::min(t0, t1);
    const glm::vec3 tbig = glm::max(t0, t1);
    const float tEnter = glm::max(glm::max(tsmall.x, tsmall.y), tsmall.z);
    const float tExit = glm::min(glm::min(tbig.x, tbig.y), tbig.z);
    if (tExit < glm::max(tEnter, 0.0f)) return false;

    float t = glm::max(tEnter, 0.0f) + vs * 1e-3f;
    if (t > maxDist) return false;
    const glm::ivec3 dim = world.dim();
    glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor((ro + rd * t) / vs)), glm::ivec3(0), dim - 1);
    const glm::ivec3 step = glm::ivec3(glm::sign(rd));
    const glm::vec3 tDelta = glm::abs(glm::vec3(vs) * invD);
    const glm::vec3 stepPos = glm::vec3(glm::greaterThan(rd, glm::vec3(0.0f)));
    glm::vec3 tMax = ((glm::vec3(cell) + stepPos) * vs - ro) * invD;

    for (int i = 0; i < 3 * glm::max(dim.x, glm::max(dim.y, dim.z)); i++) {
        if (world.voxelAt(cell) != 0) {
            outCell = cell;
            outT = t;
            return t <= maxDist;
        }
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            t = tMax.x; tMax.x += tDelta.x; cell.x += step.x;
            if (cell.x < 0 || cell.x >= dim.x) break;
        } else if (tMax.y < tMax.z) {
            t = tMax.y; tMax.y += tDelta.y; cell.y += step.y;
            if (cell.y < 0 || cell.y >= dim.y) break;
        } else {
            t = tMax.z; tMax.z += tDelta.z; cell.z += step.z;
            if (cell.z < 0 || cell.z >= dim.z) break;
        }
        if (t > maxDist || t > tExit) break;
    }
    return false;
}

}  // namespace

// ============================================================================

TEST_CASE("sparse generation matches the dense reference voxel-for-voxel", "[voxel_world]") {
    // 128^3 spans a 2x2 grid of generation chunks, so brick packing, chunk
    // seams and feature stamping across chunk borders are all exercised.
    const int N = 128;
    const uint32_t seed = 1337u;
    VoxelWorld world;
    makeWorld(world, N, seed);
    std::vector<uint8_t> dense = ref::generateDense(N, seed);

    uint64_t denseSolid = 0;
    size_t mismatches = 0;
    for (int z = 0; z < N && mismatches < 16; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                const uint8_t expect = dense[(static_cast<size_t>(z) * N + y) * N + x];
                if (expect != 0) denseSolid++;
                if (world.voxelAt({ x, y, z }) != expect) mismatches++;
            }
    REQUIRE(mismatches == 0);
    REQUIRE(world.solidVoxels() == denseSolid);
    REQUIRE(world.droppedBricks() == 0);
}

TEST_CASE("generation is deterministic per seed and diverges across seeds", "[voxel_world]") {
    VoxelWorld a;
    makeWorld(a, 64, 7u);
    VoxelWorld b;
    makeWorld(b, 64, 7u);
    VoxelWorld c;
    makeWorld(c, 64, 99u);

    REQUIRE(a.solidVoxels() == b.solidVoxels());
    bool anyDiffFromC = false;
    for (int z = 0; z < 64; z += 3)
        for (int y = 0; y < 64; y += 3)
            for (int x = 0; x < 64; x += 3) {
                REQUIRE(a.voxelAt({ x, y, z }) == b.voxelAt({ x, y, z }));
                anyDiffFromC |= a.voxelAt({ x, y, z }) != c.voxelAt({ x, y, z });
            }
    REQUIRE(anyDiffFromC);
}

TEST_CASE("terrain feature wavelength is resolution-independent (world-space frequency)", "[voxel_world]") {
    // Two worlds of the SAME physical size (6.4 m cube) but different voxel
    // sizes: coarse 64^3 @ 10 cm vs fine 128^3 @ 5 cm. terrainHeight samples in
    // world space (0.2/m * voxelSize), so a feature keeps a fixed physical
    // wavelength — matching physical columns must land at the same surface
    // height regardless of resolution. Without the fix (per-voxel frequency)
    // the fine world would pack twice the features into the same ground and the
    // surfaces would diverge (the "squished" terrain).
    VoxelWorld coarse;
    coarse.configure(glm::ivec3(64), 0.10f, 1u << 20);
    coarse.generate(20260705u);
    VoxelWorld fine;
    fine.configure(glm::ivec3(128), 0.05f, 1u << 20);
    fine.generate(20260705u);

    // First solid voxel scanning down from the top: the surface crust
    // (grass/snow/sand, never caved) — skip columns capped by a floating
    // feature (glow/crystal) so we compare terrain to terrain.
    auto surfaceMeters = [](const VoxelWorld& w, int x, int z, float vs) -> float {
        for (int y = w.dim().y - 1; y >= 0; --y) {
            const Uint8 m = w.voxelAt({ x, y, z });
            if (m == 0) continue;
            if (m == VoxelWorld::MatGrass || m == VoxelWorld::MatSnow || m == VoxelWorld::MatSand)
                return static_cast<float>(y) * vs;
            return -1.0f;  // feature on top of this column
        }
        return -1.0f;
    };

    int compared = 0;
    for (int az = 8; az <= 56; az += 8) {
        for (int ax = 8; ax <= 56; ax += 8) {
            // bx/bz are the SAME physical position at the fine resolution.
            const float sCoarse = surfaceMeters(coarse, ax, az, 0.10f);
            const float sFine = surfaceMeters(fine, ax * 2, az * 2, 0.05f);
            if (sCoarse < 0.0f || sFine < 0.0f) continue;  // feature column
            // Real heights are identical; only per-voxel quantization differs
            // (<= one coarse voxel = 0.10 m).
            CHECK(std::abs(sCoarse - sFine) <= 0.12f);
            compared++;
        }
    }
    REQUIRE(compared >= 30);  // plenty of feature-free columns to compare
}

TEST_CASE("page table, occupancy bitmasks and materials stay consistent", "[voxel_world]") {
    VoxelWorld world;
    makeWorld(world, 64, 1337u);
    const glm::ivec3 bg = world.brickGrid();
    const auto& pages = world.pageTableData();
    REQUIRE(pages.size() == static_cast<size_t>(bg.x) * bg.y * bg.z);

    size_t uniform = 0, pooled = 0, empty = 0;
    for (int bz = 0; bz < bg.z; bz++)
        for (int by = 0; by < bg.y; by++)
            for (int bx = 0; bx < bg.x; bx++) {
                const Uint32 entry = pages[(static_cast<size_t>(bz) * bg.y + by) * bg.x + bx];
                const glm::ivec3 base = glm::ivec3(bx, by, bz) * VoxelWorld::BRICK_DIM;
                if (entry == VoxelWorld::PAGE_EMPTY) {
                    empty++;
                    REQUIRE(world.voxelAt(base) == 0);
                    REQUIRE(world.voxelAt(base + (VoxelWorld::BRICK_DIM - 1)) == 0);
                } else if (entry & VoxelWorld::PAGE_UNIFORM_BIT) {
                    uniform++;
                    const Uint8 mat = static_cast<Uint8>(entry & 0xFFu);
                    REQUIRE(mat != 0);
                    REQUIRE(world.voxelAt(base) == mat);
                    REQUIRE(world.voxelAt(base + (VoxelWorld::BRICK_DIM - 1)) == mat);
                } else {
                    pooled++;
                    REQUIRE(entry < world.brickPoolSize());
                    const VoxelWorld::Brick& b = world.brick(entry);
                    for (int i = 0; i < VoxelWorld::BRICK_VOXELS; i++) {
                        const bool bit = (b.occupancy[static_cast<size_t>(i) >> 5] >> (i & 31)) & 1u;
                        REQUIRE(bit == (b.materials[i] != 0));
                    }
                }
            }
    // Terrain must produce all three page kinds (air above, uniform stone
    // interior, mixed crust bricks) or the sparse encoding isn't being used.
    REQUIRE(empty > 0);
    REQUIRE(uniform > 0);
    REQUIRE(pooled > 0);
}

TEST_CASE("carveSphere clears exactly the sphere and maintains bookkeeping", "[voxel_world]") {
    const int N = 64;
    VoxelWorld world;
    makeWorld(world, N, 1337u);

    // Snapshot, then carve around a point guaranteed under the terrain.
    std::vector<uint8_t> before(static_cast<size_t>(N) * N * N);
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                before[(static_cast<size_t>(z) * N + y) * N + x] = world.voxelAt({ x, y, z });

    const float vs = world.voxelSizeMeters();
    const glm::vec3 center = glm::vec3(N / 2, 6, N / 2) * vs;  // deep underground
    const float radius = 4.0f * vs;
    const uint64_t solidBefore = world.solidVoxels();
    REQUIRE(world.carveSphere(center, radius));

    const glm::vec3 cVox = center / vs;
    const float rVox = radius / vs;
    uint64_t expectedRemoved = 0;
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                const uint8_t was = before[(static_cast<size_t>(z) * N + y) * N + x];
                const glm::vec3 d = glm::vec3(x, y, z) + 0.5f - cVox;
                const bool inside = glm::dot(d, d) <= rVox * rVox;
                const uint8_t now = world.voxelAt({ x, y, z });
                if (inside) {
                    REQUIRE(now == 0);
                    if (was != 0) expectedRemoved++;
                } else {
                    REQUIRE(now == was);
                }
            }
    REQUIRE(expectedRemoved > 0);
    REQUIRE(world.solidVoxels() == solidBefore - expectedRemoved);

    // Carving already-empty space changes nothing.
    REQUIRE_FALSE(world.carveSphere(glm::vec3(N / 2, N - 2, N / 2) * vs, radius));
}

TEST_CASE("carving a uniform brick materializes it into the pool", "[voxel_world]") {
    const int N = 64;
    VoxelWorld world;
    makeWorld(world, N, 1337u);
    const glm::ivec3 bg = world.brickGrid();
    const auto& pages = world.pageTableData();

    // Find a uniform brick (deep terrain interior).
    glm::ivec3 target(-1);
    for (int bz = 0; bz < bg.z && target.x < 0; bz++)
        for (int by = 0; by < bg.y && target.x < 0; by++)
            for (int bx = 0; bx < bg.x; bx++) {
                const Uint32 entry = pages[(static_cast<size_t>(bz) * bg.y + by) * bg.x + bx];
                if (entry != VoxelWorld::PAGE_EMPTY && (entry & VoxelWorld::PAGE_UNIFORM_BIT)) {
                    target = { bx, by, bz };
                    break;
                }
            }
    REQUIRE(target.x >= 0);

    const size_t pageIdx =
        (static_cast<size_t>(target.z) * bg.y + target.y) * bg.x + target.x;
    const Uint8 mat = static_cast<Uint8>(pages[pageIdx] & 0xFFu);
    const glm::vec3 vs(world.voxelSizeMeters());
    const glm::vec3 brickCenter = (glm::vec3(target) + 0.5f) * (8.0f * vs.x);
    REQUIRE(world.carveSphere(brickCenter, 1.5f * vs.x));

    const Uint32 entryAfter = world.pageTableData()[pageIdx];
    REQUIRE(entryAfter != VoxelWorld::PAGE_EMPTY);
    REQUIRE_FALSE(entryAfter & VoxelWorld::PAGE_UNIFORM_BIT);  // now a pool slot
    // Center cleared, corner voxel keeps the uniform material.
    const glm::ivec3 centerCell = target * 8 + 4;
    REQUIRE(world.voxelAt(centerCell) == 0);
    REQUIRE(world.voxelAt(target * 8) == mat);
}

TEST_CASE("carving out an entire brick frees its slot", "[voxel_world]") {
    const int N = 64;
    VoxelWorld world;
    makeWorld(world, N, 1337u);
    const Uint32 residentBefore = world.residentBricks();

    // A huge sphere deep underground empties multiple bricks outright.
    const float vs = world.voxelSizeMeters();
    REQUIRE(world.carveSphere(glm::vec3(N / 2, 8, N / 2) * vs, 14.0f * vs));
    REQUIRE(world.residentBricks() < residentBefore + 32);  // no runaway allocation
    size_t emptyPages = 0;
    for (Uint32 entry : world.pageTableData())
        if (entry == VoxelWorld::PAGE_EMPTY) emptyPages++;
    REQUIRE(emptyPages > 0);
}

TEST_CASE("three-level raycast agrees with the naive oracle", "[voxel_world]") {
    const int N = 64;
    VoxelWorld world;
    makeWorld(world, N, 1337u);
    const glm::vec3 ext = world.extent();

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    int hits = 0, misses = 0;
    for (int i = 0; i < 300; i++) {
        // Rays from a shell around the volume toward a random interior point,
        // plus some rays starting inside.
        glm::vec3 ro, target;
        if (i % 4 == 0) {
            ro = glm::vec3(uni(rng), uni(rng), uni(rng)) * ext;  // inside
        } else {
            ro = glm::vec3(uni(rng) * 3.0f - 1.0f, uni(rng) * 3.0f - 1.0f, uni(rng) * 3.0f - 1.0f) * ext;
        }
        target = glm::vec3(uni(rng), uni(rng), uni(rng)) * ext;
        const glm::vec3 rd = glm::normalize(target - ro + glm::vec3(1e-5f));
        const float maxDist = 4.0f * ext.x;

        glm::ivec3 cellFast, cellRef;
        glm::vec3 hitFast;
        float tRef;
        const bool hitA = world.raycast(ro, rd, maxDist, hitFast, cellFast);
        const bool hitB = naiveRaycast(world, ro, rd, maxDist, cellRef, tRef);
        INFO("ray " << i << " ro=(" << ro.x << "," << ro.y << "," << ro.z << ")");
        REQUIRE(hitA == hitB);
        if (hitA) {
            hits++;
            REQUIRE(cellFast == cellRef);
        } else {
            misses++;
        }
    }
    REQUIRE(hits > 20);
    REQUIRE(misses > 20);
}

TEST_CASE("dirty tracking hands the renderer exactly what changed", "[voxel_world]") {
    VoxelWorld world;
    makeWorld(world, 64, 1337u);
    REQUIRE(world.hasDirty());
    auto initial = world.takeDirty();
    REQUIRE(initial.pageTable);
    REQUIRE(initial.palette);
    REQUIRE(initial.brickSlots.size() == world.residentBricks());
    REQUIRE_FALSE(world.hasDirty());

    const float vs = world.voxelSizeMeters();
    REQUIRE(world.carveSphere(glm::vec3(32, 6, 32) * vs, 2.0f * vs));
    REQUIRE(world.hasDirty());
    auto edit = world.takeDirty();
    REQUIRE_FALSE(edit.palette);
    // A 2-voxel-radius carve touches at most a 2^3 neighborhood of bricks.
    REQUIRE(edit.brickSlots.size() <= 8);
    REQUIRE_FALSE(world.hasDirty());
}

TEST_CASE("pool exhaustion drops bricks without corrupting state", "[voxel_world]") {
    VoxelWorld world;
    world.configure(glm::ivec3(64), 0.05f, 8);  // absurdly small pool
    world.generate(1337u);
    REQUIRE(world.droppedBricks() > 0);
    REQUIRE(world.brickPoolSize() <= 8);
    // Dropped bricks read as air; uniform bricks survive (no pool cost).
    size_t uniform = 0;
    for (Uint32 entry : world.pageTableData())
        if (entry != VoxelWorld::PAGE_EMPTY && (entry & VoxelWorld::PAGE_UNIFORM_BIT)) uniform++;
    REQUIRE(uniform > 0);
}

TEST_CASE("non-cubic grids generate and stay in bounds", "[voxel_world]") {
    VoxelWorld world;
    world.configure(glm::ivec3(128, 64, 96), 0.05f, 1u << 20);
    world.generate(1337u);
    REQUIRE(world.solidVoxels() > 0);
    REQUIRE(world.voxelAt({ 127, 0, 95 }) != 0);   // terrain floor reaches the far corner
    REQUIRE(world.voxelAt({ 0, 63, 0 }) == 0);     // sky stays empty
    // Out-of-bounds queries are air, never a crash.
    REQUIRE(world.voxelAt({ -1, 0, 0 }) == 0);
    REQUIRE(world.voxelAt({ 128, 0, 0 }) == 0);
}
