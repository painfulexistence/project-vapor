// ============================================================================
// TerrainWorld unit tests — the pure-logic core of the streamed terrain
// subsystem (Vapor/terrain_world.hpp). No GPU, no ECS: geometry builds,
// height/slope queries, scatter placement and LOD retargeting are all
// deterministic functions, so every streaming invariant the renderer relies
// on is checked here directly:
//
//   * vertex/index counts depend only on the LOD — the contract that makes
//     in-place slot rewrites (IRenderer::updateMeshGeometry) possible
//   * adjacent tiles sample identical edge positions — crack-free seams
//   * scatter placements sit exactly on the heightfield and obey the
//     slope/height rules
//   * computeTargets produces the concentric Chebyshev rings and reports
//     exactly the tiles whose fine slots must be freed
// ============================================================================

#include "Vapor/terrain_world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <set>

using Vapor::TerrainConfig;
using Vapor::TerrainWorld;

namespace {

// Small, fast world used by most tests: 8x8 tiles of 64 m.
TerrainConfig smallConfig() {
    TerrainConfig cfg;
    cfg.worldSize = 512.0f;
    cfg.tileSize = 64.0f;
    cfg.heightScale = 120.0f;
    cfg.noiseFrequency = 0.01f;
    cfg.noiseOctaves = 5;
    cfg.seed = 1234u;
    return cfg;
}

}  // namespace

TEST_CASE("configure rounds the world to whole tiles and resets state", "[terrain]") {
    TerrainWorld world;

    TerrainConfig cfg = smallConfig();
    cfg.worldSize = 500.0f;  // not a multiple of 64
    world.configure(cfg);
    CHECK(world.tilesPerAxis() == 7);  // floor(500 / 64)
    CHECK(world.config().worldSize == 7 * 64.0f);
    CHECK(world.tileCount() == 49);
    CHECK(world.tiles.size() == 49);
    CHECK(world.baseSlots.size() == 49);

    // Every tile boots at the base coat with no fine slot and no pending job.
    for (const auto& tile : world.tiles) {
        CHECK(tile.currentLod == TerrainWorld::LOD_COUNT - 1);
        CHECK(tile.targetLod == TerrainWorld::LOD_COUNT - 1);
        CHECK(tile.fineSlot == -1);
        CHECK_FALSE(tile.buildPending);
    }

    // A world smaller than one tile still gets one tile.
    cfg.worldSize = 10.0f;
    world.configure(cfg);
    CHECK(world.tilesPerAxis() == 1);
    CHECK(world.config().worldSize == 64.0f);

    // Reconfiguring drops queued results and resets the camera tile.
    world.pushResult(TerrainWorld::BuildResult {});
    world.lastCamTile = { 3, 3 };
    world.configure(smallConfig());
    TerrainWorld::BuildResult unused;
    CHECK_FALSE(world.popResult(unused));
    CHECK(world.lastCamTile.x == INT32_MIN);
    CHECK(world.stats().queuedResults == 0);
}

TEST_CASE("heightAt is deterministic, bounded, and seed-dependent", "[terrain]") {
    TerrainWorld a, b;
    a.configure(smallConfig());
    b.configure(smallConfig());

    bool anyPositive = false;
    for (int i = 0; i < 200; i++) {
        const float x = -256.0f + i * 2.63f;
        const float z = 256.0f - i * 1.91f;
        const float h = a.heightAt(x, z);
        CHECK(h == b.heightAt(x, z));  // same config => identical field
        CHECK(h >= 0.0f);
        CHECK(h <= smallConfig().heightScale);
        anyPositive = anyPositive || h > 0.0f;
    }
    CHECK(anyPositive);

    TerrainConfig other = smallConfig();
    other.seed = 999u;
    b.configure(other);
    bool anyDiff = false;
    for (int i = 0; i < 50 && !anyDiff; i++) {
        const float x = i * 17.0f, z = i * -13.0f;
        anyDiff = a.heightAt(x, z) != b.heightAt(x, z);
    }
    CHECK(anyDiff);

    // slopeAt is the central-difference gradient magnitude — never negative.
    CHECK(a.slopeAt(31.0f, -47.0f) >= 0.0f);
}

TEST_CASE("worldToTile maps and clamps world positions", "[terrain]") {
    TerrainWorld world;
    world.configure(smallConfig());  // 8x8 tiles, world spans [-256, 256)

    CHECK(world.worldToTile(-256.0f, -256.0f) == glm::ivec2(0, 0));
    CHECK(world.worldToTile(0.0f, 0.0f) == glm::ivec2(4, 4));
    CHECK(world.worldToTile(-0.01f, -0.01f) == glm::ivec2(3, 3));
    CHECK(world.worldToTile(255.9f, 255.9f) == glm::ivec2(7, 7));
    // Outside the world clamps to the border tiles.
    CHECK(world.worldToTile(-9999.0f, 0.0f) == glm::ivec2(0, 4));
    CHECK(world.worldToTile(9999.0f, 9999.0f) == glm::ivec2(7, 7));
}

TEST_CASE("buildTileGeometry counts depend only on the LOD", "[terrain]") {
    TerrainWorld world;
    world.configure(smallConfig());

    std::vector<Vapor::VertexData> verts;
    std::vector<Uint32> inds;
    glm::vec3 mn, mx;
    for (int lod = 0; lod < TerrainWorld::LOD_COUNT; lod++) {
        const int res = TerrainWorld::kLodRes[lod];
        const size_t expectVerts = static_cast<size_t>(res + 1) * (res + 1) + 4 * (res + 1);
        const size_t expectInds = static_cast<size_t>(res) * res * 6 + 4 * res * 12;

        world.buildTileGeometry(0, 0, lod, verts, inds, mn, mx);
        CHECK(verts.size() == expectVerts);
        CHECK(inds.size() == expectInds);

        // The in-place-rewrite contract: every tile at this LOD has the SAME
        // counts, so a pool slot's GPU buffers can be reused verbatim.
        std::vector<Vapor::VertexData> verts2;
        std::vector<Uint32> inds2;
        world.buildTileGeometry(world.tilesPerAxis() - 1, world.tilesPerAxis() / 2, lod, verts2, inds2, mn, mx);
        CHECK(verts2.size() == expectVerts);
        CHECK(inds2.size() == expectInds);
    }
}

TEST_CASE("buildTileGeometry vertices lie on the heightfield inside the AABB", "[terrain]") {
    TerrainWorld world;
    world.configure(smallConfig());
    const TerrainConfig& cfg = world.config();

    std::vector<Vapor::VertexData> verts;
    std::vector<Uint32> inds;
    glm::vec3 mn, mx;
    const int tileX = 2, tileZ = 5, lod = 1;
    world.buildTileGeometry(tileX, tileZ, lod, verts, inds, mn, mx);

    const float half = 0.5f * cfg.worldSize;
    const float minX = tileX * cfg.tileSize - half;
    const float minZ = tileZ * cfg.tileSize - half;

    for (Uint32 idx : inds) REQUIRE(idx < verts.size());

    const int res = TerrainWorld::kLodRes[lod];
    const float step = cfg.tileSize / res;
    for (const auto& v : verts) {
        // Inside the tile footprint and the reported AABB.
        CHECK(v.position.x >= minX - 1e-3f);
        CHECK(v.position.x <= minX + cfg.tileSize + 1e-3f);
        CHECK(v.position.z >= minZ - 1e-3f);
        CHECK(v.position.z <= minZ + cfg.tileSize + 1e-3f);
        CHECK(v.position.y >= mn.y - 1e-3f);
        CHECK(v.position.y <= mx.y + 1e-3f);
        // (height01, slope01) UVs feed the palette LUT — both normalized.
        CHECK(v.uv.x >= 0.0f);
        CHECK(v.uv.x <= 1.0f);
        CHECK(v.uv.y >= 0.0f);
        CHECK(v.uv.y <= 1.0f);
        CHECK_THAT(glm::length(v.normal), Catch::Matchers::WithinAbs(1.0f, 1e-3f));
    }

    // Grid vertices (the first (res+1)^2) sample heightAt exactly.
    for (int gz = 0; gz <= res; gz += res / 4) {
        for (int gx = 0; gx <= res; gx += res / 4) {
            const auto& v = verts[static_cast<size_t>(gz) * (res + 1) + gx];
            CHECK(v.position.x == minX + gx * step);
            CHECK(v.position.z == minZ + gz * step);
            CHECK(v.position.y == world.heightAt(v.position.x, v.position.z));
        }
    }

    // Skirt vertices (the remainder) hang strictly below their source edge —
    // and the AABB accounts for them.
    const size_t gridCount = static_cast<size_t>(res + 1) * (res + 1);
    for (size_t i = gridCount; i < verts.size(); i++) {
        CHECK(verts[i].position.y < world.heightAt(verts[i].position.x, verts[i].position.z));
        CHECK(verts[i].position.y >= mn.y - 1e-3f);
    }
}

TEST_CASE("adjacent tiles at the same LOD share identical edge heights", "[terrain]") {
    TerrainWorld world;
    world.configure(smallConfig());

    // East edge of tile (2,3) must equal the west edge of tile (3,3) — the
    // heightfield is pure, so shared world positions produce shared heights
    // and the seam is crack-free (before skirts even enter the picture).
    const int lod = 2;
    const int res = TerrainWorld::kLodRes[lod];
    std::vector<Vapor::VertexData> a, b;
    std::vector<Uint32> inds;
    glm::vec3 mn, mx;
    world.buildTileGeometry(2, 3, lod, a, inds, mn, mx);
    world.buildTileGeometry(3, 3, lod, b, inds, mn, mx);

    auto gridAt = [res](const std::vector<Vapor::VertexData>& v, int gx, int gz) {
        return v[static_cast<size_t>(gz) * (res + 1) + gx].position;
    };
    for (int i = 0; i <= res; i++) {
        const glm::vec3 east = gridAt(a, res, i);
        const glm::vec3 west = gridAt(b, 0, i);
        CHECK(east.x == west.x);
        CHECK(east.z == west.z);
        CHECK(east.y == west.y);
    }
}

TEST_CASE("palette LUT is a full 256x256 RGBA image with the expected bands", "[terrain]") {
    TerrainWorld world;
    world.configure(smallConfig());
    auto lut = world.buildPaletteLUT();
    REQUIRE(lut);
    CHECK(lut->uri == "terrain_palette_lut");
    CHECK(lut->width == 256);
    CHECK(lut->height == 256);
    CHECK(lut->channelCount == 4);
    REQUIRE(lut->byteArray.size() == 256u * 256u * 4u);

    auto texel = [&](int x, int y) {
        const size_t i = (static_cast<size_t>(y) * 256 + x) * 4;
        return glm::ivec4(lut->byteArray[i], lut->byteArray[i + 1], lut->byteArray[i + 2],
                          lut->byteArray[i + 3]);
    };
    for (int y = 0; y < 256; y += 51) {
        for (int x = 0; x < 256; x += 51) CHECK(texel(x, y).w == 255);
    }
    // u = height01, v = slope01. Low/flat corner is sand, high/flat is snow,
    // fully steep is rock regardless of height.
    CHECK(texel(0, 0) == glm::ivec4(193, 178, 127, 255));      // sand
    CHECK(texel(255, 0) == glm::ivec4(234, 239, 247, 255));    // snow
    CHECK(texel(0, 255) == glm::ivec4(112, 109, 104, 255));    // rock
    CHECK(texel(255, 255) == glm::ivec4(112, 109, 104, 255));  // rock
}

TEST_CASE("scatter placements are deterministic and obey the slope/height rules", "[terrain]") {
    TerrainWorld world;
    world.configure(smallConfig());
    const TerrainConfig& cfg = world.config();

    const auto first = world.scatterPlacements(3, 4);
    const auto second = world.scatterPlacements(3, 4);
    REQUIRE(first.size() == second.size());

    const float half = 0.5f * cfg.worldSize;
    const float minX = 3 * cfg.tileSize - half;
    const float minZ = 4 * cfg.tileSize - half;
    for (size_t i = 0; i < first.size(); i++) {
        const auto& p = first[i];
        CHECK(p.position == second[i].position);
        CHECK(p.meshIndex == second[i].meshIndex);

        CHECK(p.position.x >= minX);
        CHECK(p.position.x <= minX + cfg.tileSize);
        CHECK(p.position.z >= minZ);
        CHECK(p.position.z <= minZ + cfg.tileSize);

        const float ground = world.heightAt(p.position.x, p.position.z);
        const float slope = world.slopeAt(p.position.x, p.position.z);
        const float hn = ground / cfg.heightScale;
        if (p.meshIndex == 0) {
            // Tree: sits half its height above ground on gentle mid-band land.
            CHECK_THAT(p.position.y, Catch::Matchers::WithinAbs(ground + 0.5f * p.scale.y, 1e-3f));
            CHECK(slope < 0.35f);
            CHECK(hn > 0.25f);
            CHECK(hn < 0.62f);
        } else {
            // Rock: embedded on steeper ground.
            REQUIRE(p.meshIndex == 1);
            CHECK_THAT(p.position.y, Catch::Matchers::WithinAbs(ground + 0.2f * p.scale.x, 1e-3f));
            CHECK(slope >= 0.35f);
            CHECK(slope < 1.1f);
        }
    }

    // A different tile draws a different (deterministic) pattern.
    const auto other = world.scatterPlacements(4, 4);
    const bool differs = other.size() != first.size()
        || (!first.empty() && !other.empty() && other[0].position != first[0].position);
    CHECK(differs);
}

TEST_CASE("computeTargets builds concentric Chebyshev LOD rings", "[terrain]") {
    TerrainWorld world;
    TerrainConfig cfg = smallConfig();
    cfg.worldSize = 20 * 64.0f;  // 20x20 tiles so every ring fits
    cfg.lod0RadiusTiles = 2;
    cfg.lod1RadiusTiles = 4;
    cfg.lod2RadiusTiles = 8;
    world.configure(cfg);

    const glm::ivec2 cam { 10, 10 };
    world.computeTargets(cam);
    for (int t = 0; t < world.tileCount(); t++) {
        const int tx = t % world.tilesPerAxis(), tz = t / world.tilesPerAxis();
        const int ring = std::max(std::abs(tx - cam.x), std::abs(tz - cam.y));
        int expected = TerrainWorld::LOD_COUNT - 1;
        if (ring <= 2) expected = 0;
        else if (ring <= 4) expected = 1;
        else if (ring <= 8) expected = 2;
        CHECK(world.tiles[t].targetLod == expected);
    }

    // Demotion reporting: exactly the tiles whose target fell back to the
    // base coat while a fine LOD is live.
    const int nearTile = 10 * 20 + 10;  // ring 0 — stays fine
    const int farTile = 0;              // ring 10 — must demote
    world.tiles[nearTile].currentLod = 0;
    world.tiles[farTile].currentLod = 1;
    const auto demoted = world.computeTargets(cam);
    CHECK(std::find(demoted.begin(), demoted.end(), farTile) != demoted.end());
    CHECK(std::find(demoted.begin(), demoted.end(), nearTile) == demoted.end());
    for (int t : demoted) CHECK(world.tiles[t].targetLod == TerrainWorld::LOD_COUNT - 1);
}

TEST_CASE("grass cells are deterministic and obey the placement rules", "[terrain][grass]") {
    TerrainWorld world;
    TerrainConfig cfg = smallConfig();
    cfg.grassDensity = 12.0f;      // 12 attempts/m^2 on a 15 m cell = 2700 attempts
    cfg.grassCoverage = 0.7f;
    cfg.grassHeightBand = { 0.02f, 0.64f };
    world.configure(cfg);
    const float cs = world.config().grassCellSize;

    // Deterministic per (cell, seed): byte-identical rebuilds.
    const auto first = world.buildGrassCell(2, -3);
    const auto second = world.buildGrassCell(2, -3);
    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); i++) {
        CHECK(first[i].positionAndHeight == second[i].positionAndHeight);
        CHECK(first[i].params == second[i].params);
    }
    CHECK_FALSE(first.empty());

    const int attempts = static_cast<int>(cfg.grassDensity * cs * cs);
    CHECK(first.size() <= static_cast<size_t>(attempts));

    const float half = 0.5f * world.config().worldSize;
    for (const auto& b : first) {
        const glm::vec3 p(b.positionAndHeight);
        // Inside the cell footprint and the world bounds.
        CHECK(p.x >= 2 * cs);
        CHECK(p.x <= 3 * cs);
        CHECK(p.z >= -3 * cs);
        CHECK(p.z <= -2 * cs);
        CHECK(p.x >= -half);
        CHECK(p.x < half);
        // Sits exactly on the heightfield, inside the growth band.
        CHECK(p.y == world.heightAt(p.x, p.z));
        const float hn = p.y / world.config().heightScale;
        CHECK(hn >= cfg.grassHeightBand.x);
        CHECK(hn <= cfg.grassHeightBand.y);
        // Blade height spans 0.7..1.3 x the configured mean; positive width.
        CHECK(b.positionAndHeight.w >= 0.7f * cfg.grassBladeHeight - 1e-4f);
        CHECK(b.positionAndHeight.w <= 1.3f * cfg.grassBladeHeight + 1e-4f);
        CHECK(b.params.w > 0.0f);
    }

    // A different cell draws a different pattern; coverage scales the count.
    const auto other = world.buildGrassCell(3, -3);
    const bool differs = other.size() != first.size()
        || (!first.empty() && !other.empty()
            && other[0].positionAndHeight != first[0].positionAndHeight);
    CHECK(differs);

    TerrainConfig sparse = cfg;
    sparse.grassCoverage = 0.2f;
    TerrainWorld sparseWorld;
    sparseWorld.configure(sparse);
    CHECK(sparseWorld.buildGrassCell(2, -3).size() < first.size());

    // Zero density disables grass; the result queue round-trips.
    TerrainConfig off = cfg;
    off.grassDensity = 0.0f;
    TerrainWorld offWorld;
    offWorld.configure(off);
    CHECK(offWorld.buildGrassCell(2, -3).empty());

    TerrainWorld::GrassCellResult res;
    res.key = TerrainWorld::grassCellKey({ 2, -3 });
    res.blades = first;
    world.pushGrassResult(std::move(res));
    TerrainWorld::GrassCellResult out;
    REQUIRE(world.popGrassResult(out));
    CHECK(out.key == TerrainWorld::grassCellKey({ 2, -3 }));
    CHECK(out.blades.size() == first.size());
    CHECK_FALSE(world.popGrassResult(out));

    // Cell key packs (x, z) losslessly, negatives included.
    CHECK(TerrainWorld::grassCellKey({ -1, 5 }) != TerrainWorld::grassCellKey({ 5, -1 }));
    CHECK(TerrainWorld::grassCellKey({ -7, -9 }) != TerrainWorld::grassCellKey({ -9, -7 }));
    CHECK(world.grassCellOf(-0.5f * cs, 2.5f * cs) == glm::ivec2(-1, 2));
}

TEST_CASE("result queue and stats bookkeeping", "[terrain]") {
    TerrainWorld world;
    world.configure(smallConfig());

    TerrainWorld::BuildResult r;
    r.tile = 7;
    r.lod = 1;
    world.pushResult(std::move(r));
    TerrainWorld::BuildResult r2;
    r2.tile = 9;
    r2.lod = 0;
    world.pushResult(std::move(r2));
    CHECK(world.stats().queuedResults == 2);

    TerrainWorld::BuildResult out;
    REQUIRE(world.popResult(out));
    CHECK(out.tile == 9);  // LIFO — newest result lands first
    REQUIRE(world.popResult(out));
    CHECK(out.tile == 7);
    CHECK_FALSE(world.popResult(out));

    // lodCounts partition the tile set.
    world.tiles[0].currentLod = 0;
    world.tiles[1].currentLod = 1;
    world.tiles[2].currentLod = 2;
    const auto s = world.stats();
    CHECK(s.lodCounts[0] == 1);
    CHECK(s.lodCounts[1] == 1);
    CHECK(s.lodCounts[2] == 1);
    CHECK(s.lodCounts[3] == world.tileCount() - 3);
    int sum = 0;
    for (int c : s.lodCounts) sum += c;
    CHECK(sum == world.tileCount());
}
