#include "terrain_world.hpp"

#include "terrain_tile_cache.hpp"

#include "FastNoiseLite.h"

#include <algorithm>
#include <cmath>

namespace Vapor {

void TerrainWorld::configure(const TerrainConfig& config) {
    cfg = config;
    tilesAxis = std::max(1, static_cast<int>(cfg.worldSize / cfg.tileSize));
    cfg.worldSize = tilesAxis * cfg.tileSize;

    // The height source — OpenSimplex2 FBm configured exactly like the
    // default heightFn in Atmospheric's TerrainStreamer::Init, so the same
    // (seed, frequency, octaves) reproduces the same mountains.
    noise = std::make_shared<FastNoiseLite>();
    noise->SetSeed(static_cast<int>(cfg.seed));
    noise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise->SetFrequency(cfg.noiseFrequency);
    noise->SetFractalType(FastNoiseLite::FractalType_FBm);
    noise->SetFractalOctaves(cfg.noiseOctaves);
    noise->SetFractalLacunarity(cfg.noiseLacunarity);
    noise->SetFractalGain(cfg.noiseGain);

    // Baked-tile disk cache: hash every input that shapes the generated
    // heights so stale bakes can never replay after a parameter change.
    // worldSize is included (unlike Atmospheric's hash) because tile origins
    // are offset by worldSize / 2 — resizing the world moves every tile.
    tileCache.reset();
    tileCacheHash = 0;
    tileCacheHits.store(0, std::memory_order_relaxed);
    tileCacheMisses.store(0, std::memory_order_relaxed);
    if (!cfg.cacheDir.empty()) {
        auto cache = std::make_shared<TerrainTileCache>(cfg.cacheDir);
        if (cache->enabled()) {
            Uint32 h = 0;
            h = TerrainTileCache::hashCombine(h, &cfg.seed, sizeof(cfg.seed));
            h = TerrainTileCache::hashCombine(h, &cfg.noiseFrequency, sizeof(cfg.noiseFrequency));
            h = TerrainTileCache::hashCombine(h, &cfg.noiseOctaves, sizeof(cfg.noiseOctaves));
            h = TerrainTileCache::hashCombine(h, &cfg.noiseLacunarity, sizeof(cfg.noiseLacunarity));
            h = TerrainTileCache::hashCombine(h, &cfg.noiseGain, sizeof(cfg.noiseGain));
            h = TerrainTileCache::hashCombine(h, &cfg.tileSize, sizeof(cfg.tileSize));
            h = TerrainTileCache::hashCombine(h, &cfg.worldSize, sizeof(cfg.worldSize));
            h = TerrainTileCache::hashCombine(h, &cfg.cacheVersion, sizeof(cfg.cacheVersion));
            tileCacheHash = h;
            tileCache = std::move(cache);
        }
    }

    tiles.assign(static_cast<size_t>(tileCount()), Tile {});
    baseSlots.assign(static_cast<size_t>(tileCount()), Slot {});
    for (auto& pool : finePools) pool.clear();
    for (auto& free : freeSlots) free.clear();
    lastCamTile = { INT32_MIN, INT32_MIN };
    scatterEntities.clear();
    scatterMeshes.clear();
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        results.clear();
    }
    inFlight.store(0, std::memory_order_relaxed);

    grassCells.clear();
    grassPending.clear();
    grassFreeSlots.clear();
    grassSlotCount = 0;
    grassBladesPerSlot = 0;
    lastGrassCell = { INT32_MIN, INT32_MIN };
    {
        std::lock_guard<std::mutex> lock(grassResultMutex);
        grassResults.clear();
    }
    grassInFlight.store(0, std::memory_order_relaxed);
}

float TerrainWorld::height01At(float x, float z) const {
    // Atmospheric's default heightFn is GetNoise * 0.5 + 0.5; the streamer
    // clamps to [0,1] when it bakes tile grids, so clamp here too.
    if (!noise) return 0.0f;
    return glm::clamp(noise->GetNoise(x, z) * 0.5f + 0.5f, 0.0f, 1.0f);
}

float TerrainWorld::heightAt(float x, float z) const {
    return height01At(x, z) * cfg.heightScale;
}

std::vector<float> TerrainWorld::tileHeightGrid(int tileX, int tileZ, int lod) const {
    const int res = kLodRes[glm::clamp(lod, 0, LOD_COUNT - 1)];
    const int w = res + 3;
    std::vector<float> grid;
    if (tileCache && tileCache->load(tileX, tileZ, lod, tileCacheHash, w, grid)) {
        tileCacheHits.fetch_add(1, std::memory_order_relaxed);
        return grid;
    }
    const float half = 0.5f * cfg.worldSize;
    const float minX = tileX * cfg.tileSize - half;
    const float minZ = tileZ * cfg.tileSize - half;
    const float step = cfg.tileSize / res;
    grid.resize(static_cast<size_t>(w) * w);
    for (int j = 0; j < w; j++) {
        const float wz = minZ + (j - 1) * step;
        for (int i = 0; i < w; i++) {
            grid[static_cast<size_t>(j) * w + i] = height01At(minX + (i - 1) * step, wz);
        }
    }
    if (tileCache) {
        tileCache->store(tileX, tileZ, lod, tileCacheHash, w, grid);
        tileCacheMisses.fetch_add(1, std::memory_order_relaxed);
    }
    return grid;
}

float TerrainWorld::slopeAt(float x, float z) const {
    const float d = 4.0f;
    float dhdx = (heightAt(x + d, z) - heightAt(x - d, z)) / (2.0f * d);
    float dhdz = (heightAt(x, z + d) - heightAt(x, z - d)) / (2.0f * d);
    return std::sqrt(dhdx * dhdx + dhdz * dhdz);
}

glm::ivec2 TerrainWorld::worldToTile(float x, float z) const {
    const float half = 0.5f * cfg.worldSize;
    int tx = static_cast<int>(std::floor((x + half) / cfg.tileSize));
    int tz = static_cast<int>(std::floor((z + half) / cfg.tileSize));
    return { glm::clamp(tx, 0, tilesAxis - 1), glm::clamp(tz, 0, tilesAxis - 1) };
}

void TerrainWorld::buildTileGeometry(int tileX, int tileZ, int lod, std::vector<VertexData>& verts,
                                     std::vector<Uint32>& inds, glm::vec3& aabbMin,
                                     glm::vec3& aabbMax) const {
    const int res = kLodRes[glm::clamp(lod, 0, LOD_COUNT - 1)];
    const float half = 0.5f * cfg.worldSize;
    const float minX = tileX * cfg.tileSize - half;
    const float minZ = tileZ * cfg.tileSize - half;
    const float step = cfg.tileSize / res;
    const float skirtDepth = 2.0f * step;  // hides LOD-boundary T-junction cracks

    // One gutter grid drives heights, normals and slopes — the unit the
    // baked-tile cache stores, so a cached tile skips synthesis entirely.
    const std::vector<float> grid = tileHeightGrid(tileX, tileZ, lod);
    const int w = res + 3;
    const auto gridH = [&](int gx, int gz) {
        return grid[static_cast<size_t>(gz + 1) * w + (gx + 1)] * cfg.heightScale;
    };

    verts.clear();
    verts.reserve(static_cast<size_t>(res + 1) * (res + 1) + 4 * (res + 1));
    aabbMin = glm::vec3(minX, 1e9f, minZ);
    aabbMax = glm::vec3(minX + cfg.tileSize, -1e9f, minZ + cfg.tileSize);

    for (int gz = 0; gz <= res; gz++) {
        for (int gx = 0; gx <= res; gx++) {
            const float wx = minX + gx * step;
            const float wz = minZ + gz * step;
            const float h = gridH(gx, gz);
            const float hl = gridH(gx - 1, gz), hr = gridH(gx + 1, gz);
            const float hd = gridH(gx, gz - 1), hu = gridH(gx, gz + 1);
            glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));
            const float slope01 =
                glm::clamp(std::sqrt((hr - hl) * (hr - hl) + (hu - hd) * (hu - hd)) / (4.0f * step), 0.0f, 1.0f);
            VertexData v {};
            v.position = glm::vec3(wx, h, wz);
            v.uv = glm::vec2(h / cfg.heightScale, slope01);
            v.normal = n;
            v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            verts.push_back(v);
            aabbMin.y = std::min(aabbMin.y, h);
            aabbMax.y = std::max(aabbMax.y, h);
        }
    }

    // Skirt ring: edge vertices extruded down; two-sided quads so back-face
    // culling never opens the crack back up.
    auto gridIndex = [res](int gx, int gz) { return static_cast<Uint32>(gz * (res + 1) + gx); };
    std::array<std::vector<Uint32>, 4> edges;  // N, S, W, E edge vertex indices
    for (int i = 0; i <= res; i++) {
        edges[0].push_back(gridIndex(i, 0));
        edges[1].push_back(gridIndex(i, res));
        edges[2].push_back(gridIndex(0, i));
        edges[3].push_back(gridIndex(res, i));
    }
    std::array<std::vector<Uint32>, 4> skirts;
    for (int eIdx = 0; eIdx < 4; eIdx++) {
        for (Uint32 src : edges[eIdx]) {
            VertexData v = verts[src];
            v.position.y -= skirtDepth;
            skirts[eIdx].push_back(static_cast<Uint32>(verts.size()));
            verts.push_back(v);
        }
    }
    aabbMin.y -= skirtDepth;

    inds.clear();
    inds.reserve(static_cast<size_t>(res) * res * 6 + 4 * res * 12);
    for (int gz = 0; gz < res; gz++) {
        for (int gx = 0; gx < res; gx++) {
            Uint32 v00 = gridIndex(gx, gz), v10 = gridIndex(gx + 1, gz);
            Uint32 v01 = gridIndex(gx, gz + 1), v11 = gridIndex(gx + 1, gz + 1);
            inds.insert(inds.end(), { v00, v01, v11, v00, v11, v10 });
        }
    }
    for (int eIdx = 0; eIdx < 4; eIdx++) {
        for (int i = 0; i < res; i++) {
            Uint32 a = edges[eIdx][i], b = edges[eIdx][i + 1];
            Uint32 sa = skirts[eIdx][i], sb = skirts[eIdx][i + 1];
            inds.insert(inds.end(), { a, b, sb, a, sb, sa });
            inds.insert(inds.end(), { a, sb, b, a, sa, sb });
        }
    }
}

std::shared_ptr<Image> TerrainWorld::buildPaletteLUT() const {
    constexpr int N = 256;
    auto lut = std::make_shared<Image>();
    lut->uri = "terrain_palette_lut";  // unique key for the renderer's texture cache
    lut->width = N;
    lut->height = N;
    lut->channelCount = 4;
    lut->byteArray.resize(static_cast<size_t>(N) * N * 4);
    auto put = [&](int x, int y, glm::vec3 c) {
        const size_t i = (static_cast<size_t>(y) * N + x) * 4;
        lut->byteArray[i + 0] = static_cast<Uint8>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        lut->byteArray[i + 1] = static_cast<Uint8>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        lut->byteArray[i + 2] = static_cast<Uint8>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        lut->byteArray[i + 3] = 255;
    };
    const glm::vec3 sand(0.76f, 0.70f, 0.50f), grass(0.22f, 0.42f, 0.16f);
    const glm::vec3 dirt(0.42f, 0.32f, 0.20f), snow(0.92f, 0.94f, 0.97f);
    const glm::vec3 rock(0.44f, 0.43f, 0.41f);
    for (int y = 0; y < N; y++) {
        const float slope = y / float(N - 1);
        for (int x = 0; x < N; x++) {
            const float h = x / float(N - 1);
            glm::vec3 c;
            if (h < 0.12f) c = sand;
            else if (h < 0.20f) c = glm::mix(sand, grass, (h - 0.12f) / 0.08f);
            else if (h < 0.50f) c = grass;
            else if (h < 0.62f) c = glm::mix(grass, dirt, (h - 0.50f) / 0.12f);
            else if (h < 0.70f) c = glm::mix(dirt, snow, (h - 0.62f) / 0.08f);
            else c = snow;
            c = glm::mix(c, rock, glm::smoothstep(0.35f, 0.75f, slope));
            put(x, y, c);
        }
    }
    return lut;
}

std::vector<TerrainWorld::ScatterPlacement> TerrainWorld::scatterPlacements(int tileX, int tileZ) const {
    std::vector<ScatterPlacement> out;
    const float half = 0.5f * cfg.worldSize;
    const float minX = tileX * cfg.tileSize - half;
    const float minZ = tileZ * cfg.tileSize - half;
    Uint32 rng = static_cast<Uint32>(tileX) * 73856093u ^ static_cast<Uint32>(tileZ) * 19349663u ^ cfg.seed;
    auto rand01 = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    };
    for (int i = 0; i < cfg.scatterPerTile; i++) {
        const float wx = minX + rand01() * cfg.tileSize;
        const float wz = minZ + rand01() * cfg.tileSize;
        const float y = heightAt(wx, wz);
        const float slope = slopeAt(wx, wz);
        const float hn = y / cfg.heightScale;
        if (slope < 0.35f && hn > 0.25f && hn < 0.62f) {
            const float s = 3.0f + rand01() * 5.0f;  // tree
            out.push_back({ { wx, y + 0.5f * s, wz }, { 0.5f * s, s, 0.5f * s }, rand01() * 6.2832f, 0 });
        } else if (slope >= 0.35f && slope < 1.1f && rand01() < 0.35f) {
            const float s = 1.0f + rand01() * 3.0f;  // rock
            out.push_back({ { wx, y + 0.2f * s, wz }, { s, 0.6f * s, s }, rand01() * 6.2832f, 1 });
        }
    }
    return out;
}

glm::ivec2 TerrainWorld::grassCellOf(float x, float z) const {
    const float cs = std::max(cfg.grassCellSize, 1.0f);
    return { static_cast<int>(std::floor(x / cs)), static_cast<int>(std::floor(z / cs)) };
}

std::vector<GrassBladeGpu> TerrainWorld::buildGrassCell(int cellX, int cellZ) const {
    std::vector<GrassBladeGpu> out;
    if (cfg.grassDensity <= 0.0f) return out;
    const float cs = std::max(cfg.grassCellSize, 1.0f);
    const float minX = cellX * cs, minZ = cellZ * cs;
    const float half = 0.5f * cfg.worldSize;
    const int attempts = static_cast<int>(cfg.grassDensity * cs * cs);
    out.reserve(static_cast<size_t>(attempts));
    Uint32 rng = static_cast<Uint32>(cellX) * 73856093u ^ static_cast<Uint32>(cellZ) * 19349663u
        ^ (cfg.seed * 2654435761u + 17u);
    auto rand01 = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    };
    const bool slopeCull = cfg.grassMaxSlope < 999.0f;  // default 1000 = everywhere
    for (int i = 0; i < attempts; i++) {
        // Draw every random up-front so the stream (and thus every later
        // blade) is independent of which culls fire.
        const float wx = minX + rand01() * cs;
        const float wz = minZ + rand01() * cs;
        const float cov = rand01();
        const float hRand = rand01();
        const float phase = rand01();
        const float face = rand01();
        const float tint = rand01();
        if (wx < -half || wx >= half || wz < -half || wz >= half) continue;
        if (cov > cfg.grassCoverage) continue;
        const float y = heightAt(wx, wz);
        const float hn = y / cfg.heightScale;
        if (hn < cfg.grassHeightBand.x || hn > cfg.grassHeightBand.y) continue;
        if (slopeCull && slopeAt(wx, wz) > cfg.grassMaxSlope) continue;
        GrassBladeGpu b;
        b.positionAndHeight = glm::vec4(wx, y, wz, cfg.grassBladeHeight * (0.7f + 0.6f * hRand));
        b.params = glm::vec4(phase * 6.2831853f, face * 6.2831853f, tint, 0.045f + 0.025f * tint);
        out.push_back(b);
    }
    return out;
}

std::vector<int> TerrainWorld::computeTargets(glm::ivec2 camTile) {
    const int radii[LOD_COUNT] = { cfg.lod0RadiusTiles, cfg.lod1RadiusTiles, cfg.lod2RadiusTiles, INT32_MAX };
    std::vector<int> demoted;
    for (int t = 0; t < static_cast<int>(tiles.size()); t++) {
        const int tx = t % tilesAxis, tz = t / tilesAxis;
        const int ring = std::max(std::abs(tx - camTile.x), std::abs(tz - camTile.y));
        int target = LOD_COUNT - 1;
        for (int lod = 0; lod < LOD_COUNT; lod++) {
            if (ring <= radii[lod]) {
                target = lod;
                break;
            }
        }
        tiles[t].targetLod = target;
        if (target == LOD_COUNT - 1 && tiles[t].currentLod < LOD_COUNT - 1) demoted.push_back(t);
    }
    return demoted;
}

TerrainWorld::Stats TerrainWorld::stats() const {
    Stats s;
    for (const Tile& t : tiles) s.lodCounts[glm::clamp(t.currentLod, 0, LOD_COUNT - 1)]++;
    s.pendingJobs = inFlight.load(std::memory_order_relaxed);
    s.cacheHits = tileCacheHits.load(std::memory_order_relaxed);
    s.cacheMisses = tileCacheMisses.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        s.queuedResults = static_cast<int>(results.size());
    }
    return s;
}

}// namespace Vapor
