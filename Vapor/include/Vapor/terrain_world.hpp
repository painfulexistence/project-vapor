#pragma once
#include "graphics.hpp"// VertexData, Mesh, Image, GrassBladeGpu (data structs; no GPU calls here)
#include <SDL3/SDL_stdinc.h>
#include <array>
#include <atomic>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Vapor {

// ============================================================================
// TerrainWorld — CPU core of the streamed heightfield terrain (the engine
// half of Atmospheric's TerrainStreamer, rebuilt on Vapor's ECS: entities and
// renderer calls live in TerrainSystem; everything here is data + pure
// functions, so the whole core is unit-testable without a GPU).
//
// The world is a TILES x TILES grid of heightfield tiles. Every tile keeps an
// always-resident coarsest-LOD mesh (the "base coat" — full horizon from the
// first frame), and concentric rings around the camera refine tiles through
// fixed per-LOD mesh slot pools: a slot's geometry is REWRITTEN in place
// (IRenderer::updateMeshGeometry) when it is re-assigned, so streaming never
// allocates or leaks GPU memory. Tile builds run as task-scheduler jobs;
// results come back through the mutex-guarded queue below.
//
// Splat texturing is approximated by baking (height01, slope01) into each
// vertex's UV and sampling the generated palette LUT — one material for
// every tile, zero per-tile textures.
// ============================================================================

struct TerrainConfig {
    float worldSize = 10240.0f;   // meters per axis; rounded to a tile multiple
    float tileSize = 512.0f;      // meters per tile edge
    float heightScale = 500.0f;   // peak height in meters
    float noiseFrequency = 0.0007f;// ~1.4 km feature wavelength
    int noiseOctaves = 9;
    Uint32 seed = 20260705u;
    // Concentric ring radius (Chebyshev, in tiles) selecting LOD 0/1/2;
    // everything further keeps the always-resident base coat (LOD 3).
    int lod0RadiusTiles = 2;
    int lod1RadiusTiles = 4;
    int lod2RadiusTiles = 8;
    // Deterministic tree/rock scatter ring.
    int scatterRadiusTiles = 3;
    int scatterPerTile = 90;      // placement attempts per tile

    // Streamed grass ring (GoT-style sway; defaults from the original demo).
    // grassDensity is placement ATTEMPTS per m^2; coverage/height-band/slope
    // rules cull them. 0 disables grass entirely.
    float grassDensity = 40.0f;
    float grassRadius = 90.0f;      // ring radius around the camera (m)
    float grassCellSize = 15.0f;    // streaming cell edge (m)
    float grassBladeHeight = 1.6f;  // mean blade height (m)
    glm::vec2 grassHeightBand { 0.02f, 0.64f };  // height01 range that grows grass
    float grassCoverage = 0.7f;     // fraction of attempts that sprout
    float grassMaxSlope = 1000.0f;  // rise/run cutoff (>= 999 = grow everywhere)
};

class TerrainWorld {
public:
    static constexpr int LOD_COUNT = 4;
    // Mesh resolution (quads per edge) per LOD; LOD 3 is the base coat.
    static constexpr int kLodRes[LOD_COUNT] = { 64, 32, 16, 8 };
    // Slot-pool sizes per fine LOD: ring area plus slack for transitions.
    static constexpr int kLodSlots[LOD_COUNT - 1] = { 32, 96, 288 };

    struct Slot {
        std::shared_ptr<Mesh> mesh;
        entt::entity entity = entt::null;
    };
    struct Tile {
        int currentLod = 3;
        int targetLod = 3;
        int fineSlot = -1;  // index into finePools[currentLod] when currentLod < 3
        bool buildPending = false;
    };
    struct BuildResult {
        int tile = 0;
        int lod = 0;
        std::vector<VertexData> verts;
        std::vector<Uint32> inds;
        glm::vec3 aabbMin { 0.0f }, aabbMax { 0.0f };
    };
    // One scatter instance: meshIndex picks the prototype (0 = tree, 1 = rock).
    struct ScatterPlacement {
        glm::vec3 position { 0.0f };
        glm::vec3 scale { 1.0f };
        float yawRadians = 0.0f;
        int meshIndex = 0;
    };
    struct Stats {
        int lodCounts[LOD_COUNT] = {};
        int pendingJobs = 0;
        int queuedResults = 0;
    };

    // Rounds worldSize down to a whole number of tiles (at least 1) and
    // resets every tile/slot to the boot state.
    void configure(const TerrainConfig& config);
    const TerrainConfig& config() const { return cfg; }
    int tilesPerAxis() const { return tilesAxis; }
    int tileCount() const { return tilesAxis * tilesAxis; }

    // ---- Pure queries (thread-safe; shared by jobs and gameplay) ---------
    float heightAt(float x, float z) const;
    float slopeAt(float x, float z) const;
    glm::ivec2 worldToTile(float x, float z) const;

    // Heightfield tile mesh: (res+1)^2 grid vertices with normals and
    // (height01, slope01) UVs, plus a two-sided skirt ring hiding
    // LOD-boundary cracks. Vertex/index counts depend only on the LOD, which
    // is what makes in-place slot rewrites possible.
    void buildTileGeometry(int tileX, int tileZ, int lod, std::vector<VertexData>& verts,
                           std::vector<Uint32>& inds, glm::vec3& aabbMin, glm::vec3& aabbMax) const;

    // 256x256 RGBA8 palette LUT (u = height01, v = slope01): sand/grass/dirt/
    // snow height bands blending to rock on steep slopes.
    std::shared_ptr<Image> buildPaletteLUT() const;

    // Deterministic per-tile scatter (slope/height rules from the original
    // demo): trees on gentle mid-band ground, rocks on steeper slopes.
    std::vector<ScatterPlacement> scatterPlacements(int tileX, int tileZ) const;

    // ---- Grass ring (streamed instanced blades) --------------------------
    // Cells are a world-origin-anchored grid of grassCellSize metres,
    // independent of the terrain tiles. buildGrassCell is pure and
    // deterministic per (cell, seed): attempts = density * cellSize^2, each
    // culled by world bounds, coverage, the height01 band, and (when
    // grassMaxSlope < 999) the slope cutoff. Blades sit exactly on heightAt.
    glm::ivec2 grassCellOf(float x, float z) const;
    static long long grassCellKey(glm::ivec2 c) {
        return (static_cast<long long>(c.y) << 32) | static_cast<Uint32>(c.x);
    }
    std::vector<GrassBladeGpu> buildGrassCell(int cellX, int cellZ) const;

    struct GrassCellResult {
        long long key = 0;
        std::vector<GrassBladeGpu> blades;
    };
    void pushGrassResult(GrassCellResult&& result) {
        std::lock_guard<std::mutex> lock(grassResultMutex);
        grassResults.push_back(std::move(result));
    }
    bool popGrassResult(GrassCellResult& out) {
        std::lock_guard<std::mutex> lock(grassResultMutex);
        if (grassResults.empty()) return false;
        out = std::move(grassResults.back());
        grassResults.pop_back();
        return true;
    }

    // ---- Streaming bookkeeping (driven by TerrainSystem) -----------------
    // Recomputes every tile's target LOD around camTile; returns the tiles
    // whose target dropped to the base coat while a fine slot is live —
    // demotions are free (the base mesh is always resident) and the system
    // applies them immediately so the freed slots serve incoming tiles.
    std::vector<int> computeTargets(glm::ivec2 camTile);

    void pushResult(BuildResult&& result) {
        std::lock_guard<std::mutex> lock(resultMutex);
        results.push_back(std::move(result));
    }
    bool popResult(BuildResult& out) {
        std::lock_guard<std::mutex> lock(resultMutex);
        if (results.empty()) return false;
        out = std::move(results.back());
        results.pop_back();
        return true;
    }

    Stats stats() const;

    // System-owned state, public like a component's data: tiles, the
    // always-resident base slots, the per-LOD fine pools with their free
    // lists, and the scatter entities per tile.
    std::vector<Tile> tiles;
    std::vector<Slot> baseSlots;
    std::array<std::vector<Slot>, LOD_COUNT - 1> finePools;
    std::array<std::vector<int>, LOD_COUNT - 1> freeSlots;
    glm::ivec2 lastCamTile { INT32_MIN, INT32_MIN };
    std::unordered_map<int, std::vector<entt::entity>> scatterEntities;
    std::vector<std::shared_ptr<Mesh>> scatterMeshes;  // prototypes (0 tree, 1 rock)
    std::atomic<int> inFlight { 0 };

    // Grass streaming bookkeeping (system-owned, like the tile slots above).
    struct GrassCellState {
        int slot = -1;
        Uint32 bladeCount = 0;
    };
    std::unordered_map<long long, GrassCellState> grassCells;  // resident cells
    std::unordered_set<long long> grassPending;                // builds in flight
    std::vector<int> grassFreeSlots;
    Uint32 grassSlotCount = 0;
    Uint32 grassBladesPerSlot = 0;
    glm::ivec2 lastGrassCell { INT32_MIN, INT32_MIN };
    std::atomic<int> grassInFlight { 0 };

private:
    TerrainConfig cfg;
    int tilesAxis = 20;

    mutable std::mutex resultMutex;
    std::vector<BuildResult> results;

    mutable std::mutex grassResultMutex;
    std::vector<GrassCellResult> grassResults;
};

}// namespace Vapor
