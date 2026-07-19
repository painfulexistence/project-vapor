// ============================================================================
// TerrainStreaming — 10.24 km x 10.24 km streamed open-world terrain.
//
// The Vapor port of Atmospheric's Examples/TerrainStreaming, rebuilt as a
// demo-local streamer over the engine's ECS + renderer (game systems live in
// the app layer). The whole world is prewarmed at the coarsest LOD during
// load — full horizon on frame one — then concentric detail rings refine
// around the camera on task-scheduler workers while you fly, with no
// geometry leaks: every tile mesh comes from a fixed per-LOD slot pool
// registered once and rewritten in place (Renderer::updateMeshGeometry) as
// the rings move.
//
// Differences from the original (engine-subsystem scope kept out of a demo):
// no grass ring, no RmlUi HUD (keyboard only), no physics colliders, and
// splat texturing is approximated by baking (height, slope) into each
// vertex's UV and sampling a generated palette LUT — one material for every
// tile, zero per-tile textures.
//
// Controls: WASD move, R/F up/down, IJKL look, LShift sprint (x50),
//           G toggle ground-clamp, T teleport +2km (streaming stress test),
//           Esc quit. (--vulkan / --metal pick the backend.)
// ============================================================================

#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/irenderer.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/render_scene.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/systems.hpp"
#include "Vapor/task_scheduler.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <cstring>
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"

namespace {

// ============================================================================
// Height source — gradient-noise fBm, pure and thread-safe, so worker jobs
// and main-thread queries (ground clamp, scatter) share one function.
// ============================================================================

float hashNoise(int x, int y, Uint32 seed) {
    Uint32 h = static_cast<Uint32>(x) * 374761393u + static_cast<Uint32>(y) * 668265263u + seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

float gradDot(int xi, int zi, glm::vec2 offset, Uint32 seed) {
    glm::vec2 g(hashNoise(xi, zi, seed) - 0.5f, hashNoise(xi, zi, seed ^ 0x9E3779B9u) - 0.5f);
    float len = glm::length(g);
    if (len < 1e-6f) return offset.x;
    return glm::dot(g / len, offset);
}

float gradNoise2(glm::vec2 p, Uint32 seed) {
    glm::vec2 pf = glm::floor(p);
    glm::vec2 f = p - pf;
    glm::vec2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);  // quintic fade
    int xi = static_cast<int>(pf.x), zi = static_cast<int>(pf.y);
    float a = gradDot(xi, zi, f, seed);
    float b = gradDot(xi + 1, zi, f - glm::vec2(1, 0), seed);
    float c = gradDot(xi, zi + 1, f - glm::vec2(0, 1), seed);
    float d = gradDot(xi + 1, zi + 1, f - glm::vec2(1, 1), seed);
    return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
}

struct TerrainParams {
    static constexpr float WORLD_SIZE = 10240.0f;
    static constexpr float TILE_SIZE = 512.0f;
    static constexpr int TILES = 20;  // per axis (TILES * TILE_SIZE == WORLD_SIZE)
    static constexpr float HEIGHT_SCALE = 500.0f;
    // ~1.4 km feature wavelength: distinct ridges/valleys across 10 km.
    static constexpr float NOISE_FREQ = 0.0007f;
    static constexpr int NOISE_OCTAVES = 9;
    static constexpr Uint32 SEED = 20260705u;
};

float heightAt(float x, float z) {
    glm::vec2 p = glm::vec2(x, z) * TerrainParams::NOISE_FREQ;
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < TerrainParams::NOISE_OCTAVES; i++) {
        sum += amp * gradNoise2(p, TerrainParams::SEED + static_cast<Uint32>(i) * 101u);
        p *= 2.0f;
        amp *= 0.5f;
    }
    return glm::clamp(0.5f + sum * 1.2f, 0.0f, 1.0f) * TerrainParams::HEIGHT_SCALE;
}

float slopeAt(float x, float z) {
    const float d = 4.0f;
    float dhdx = (heightAt(x + d, z) - heightAt(x - d, z)) / (2.0f * d);
    float dhdz = (heightAt(x, z + d) - heightAt(x, z - d)) / (2.0f * d);
    return std::sqrt(dhdx * dhdx + dhdz * dhdz);
}

// ============================================================================
// Streaming terrain — fixed per-LOD slot pools, async tile builds.
// ============================================================================

class TerrainStreamer {
public:
    static constexpr int LOD_COUNT = 4;
    // Mesh resolution (quads per edge) per LOD; LOD3 is the always-resident
    // base coat every tile keeps.
    static constexpr int kLodRes[LOD_COUNT] = { 64, 32, 16, 8 };
    // Concentric ring radius (Chebyshev, in tiles) selecting each LOD.
    static constexpr int kLodRadius[LOD_COUNT] = { 2, 4, 8, 1 << 20 };
    // Slot-pool sizes per fine LOD, ring area plus slack (transitions overlap).
    static constexpr int kLodSlots[LOD_COUNT - 1] = { 32, 96, 288 };

    struct Stats {
        int lodCounts[LOD_COUNT] = {};
        int pendingJobs = 0;
        int queuedResults = 0;
        int scatterEntities = 0;
    };

    void init(entt::registry& registry, RenderScene& scene, Renderer* rhiRenderer,
              Vapor::TaskScheduler& scheduler) {
        m_registry = &registry;
        m_renderer = rhiRenderer;
        m_scheduler = &scheduler;

        buildPaletteMaterial(scene);
        buildScatterPrototypes(scene);

        const int total = TerrainParams::TILES * TerrainParams::TILES;
        m_tiles.resize(total);

        // Base coat: one LOD3 mesh per tile, generated in parallel and staged
        // before the first frame — the whole horizon is present at boot.
        m_baseSlots.resize(total);
        std::atomic<int> remaining { total };
        for (int t = 0; t < total; t++) {
            Slot& slot = m_baseSlots[t];
            slot.mesh = std::make_shared<Vapor::Mesh>();
            scheduler.submitTask([this, t, &remaining] {
                std::vector<Vapor::VertexData> verts;
                std::vector<Uint32> inds;
                glm::vec3 mn, mx;
                buildTileGeometry(t % TerrainParams::TILES, t / TerrainParams::TILES, 3, verts, inds, mn, mx);
                Slot& s = m_baseSlots[t];
                s.mesh->initialize(verts, inds);
                s.mesh->localAABBMin = mn;
                s.mesh->localAABBMax = mx;
                remaining.fetch_sub(1, std::memory_order_release);
            });
        }
        scheduler.waitForAll();
        (void)remaining;
        for (int t = 0; t < total; t++) {
            Slot& slot = m_baseSlots[t];
            slot.mesh->material = m_terrainMaterial;
            scene.addMesh(slot.mesh);
            slot.entity = makeTileEntity(registry, slot.mesh, "Terrain.Base", /*visible=*/true);
        }

        // Fine pools: registered once with a flat placeholder (one shared
        // placeholder per LOD — contents are rewritten in place on stream).
        for (int lod = 0; lod < LOD_COUNT - 1; lod++) {
            std::vector<Vapor::VertexData> verts;
            std::vector<Uint32> inds;
            glm::vec3 mn, mx;
            buildTileGeometry(0, 0, lod, verts, inds, mn, mx);
            m_finePools[lod].resize(kLodSlots[lod]);
            m_freeSlots[lod].clear();
            for (int s = 0; s < kLodSlots[lod]; s++) {
                Slot& slot = m_finePools[lod][s];
                slot.mesh = std::make_shared<Vapor::Mesh>();
                slot.mesh->initialize(verts, inds);
                slot.mesh->material = m_terrainMaterial;
                scene.addMesh(slot.mesh);
                slot.entity = makeTileEntity(registry, slot.mesh, "Terrain.Fine", /*visible=*/false);
                m_freeSlots[lod].push_back(s);
            }
        }
    }

    void update(const glm::vec3& camPos) {
        const glm::ivec2 camTile = worldToTile(camPos.x, camPos.z);
        if (camTile != m_lastCamTile) {
            m_lastCamTile = camTile;
            retargetTiles(camTile);
            updateScatter(camTile);
        }
        enqueueBuilds();
        applyResults();
    }

    float groundHeight(float x, float z) const { return heightAt(x, z); }

    Stats stats() const {
        Stats s;
        for (const Tile& t : m_tiles) s.lodCounts[t.currentLod]++;
        s.pendingJobs = m_inFlight.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            s.queuedResults = static_cast<int>(m_results.size());
        }
        for (const auto& [tile, ents] : m_scatter) s.scatterEntities += static_cast<int>(ents.size());
        return s;
    }

private:
    struct Slot {
        std::shared_ptr<Vapor::Mesh> mesh;
        entt::entity entity = entt::null;
    };
    struct Tile {
        int currentLod = 3;
        int targetLod = 3;
        int fineSlot = -1;  // index into m_finePools[currentLod] when currentLod < 3
        bool buildPending = false;
    };
    struct BuildResult {
        int tile = 0;
        int lod = 0;
        std::vector<Vapor::VertexData> verts;
        std::vector<Uint32> inds;
        glm::vec3 aabbMin { 0.0f }, aabbMax { 0.0f };
    };

    static glm::ivec2 worldToTile(float x, float z) {
        const float half = 0.5f * TerrainParams::WORLD_SIZE;
        int tx = static_cast<int>(std::floor((x + half) / TerrainParams::TILE_SIZE));
        int tz = static_cast<int>(std::floor((z + half) / TerrainParams::TILE_SIZE));
        return { glm::clamp(tx, 0, TerrainParams::TILES - 1), glm::clamp(tz, 0, TerrainParams::TILES - 1) };
    }

    entt::entity makeTileEntity(entt::registry& registry, const std::shared_ptr<Vapor::Mesh>& mesh,
                                const char* name, bool visible) {
        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent { name });
        registry.emplace<Vapor::TransformComponent>(e);  // identity — world coords baked into vertices
        auto& mr = registry.emplace<Vapor::MeshRendererComponent>(e);
        mr.meshes.push_back(mesh);
        mr.visible = visible;
        return e;
    }

    // (height01, slope01) baked into every vertex's UV; the palette LUT
    // material turns that into grass/rock/dirt/snow banding.
    static void buildTileGeometry(int tileX, int tileZ, int lod, std::vector<Vapor::VertexData>& verts,
                                  std::vector<Uint32>& inds, glm::vec3& aabbMin, glm::vec3& aabbMax) {
        const int res = kLodRes[lod];
        const float half = 0.5f * TerrainParams::WORLD_SIZE;
        const float minX = tileX * TerrainParams::TILE_SIZE - half;
        const float minZ = tileZ * TerrainParams::TILE_SIZE - half;
        const float step = TerrainParams::TILE_SIZE / res;
        const float skirtDepth = 2.0f * step;  // hides LOD-boundary T-junction cracks

        const int gridVerts = (res + 1) * (res + 1);
        verts.clear();
        verts.reserve(gridVerts + 4 * (res + 1));
        aabbMin = glm::vec3(minX, 1e9f, minZ);
        aabbMax = glm::vec3(minX + TerrainParams::TILE_SIZE, -1e9f, minZ + TerrainParams::TILE_SIZE);

        for (int gz = 0; gz <= res; gz++) {
            for (int gx = 0; gx <= res; gx++) {
                const float wx = minX + gx * step;
                const float wz = minZ + gz * step;
                const float h = heightAt(wx, wz);
                const float hl = heightAt(wx - step, wz), hr = heightAt(wx + step, wz);
                const float hd = heightAt(wx, wz - step), hu = heightAt(wx, wz + step);
                glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));
                const float slope01 = glm::clamp(std::sqrt((hr - hl) * (hr - hl) + (hu - hd) * (hu - hd))
                                                     / (4.0f * step),
                                                 0.0f, 1.0f);
                Vapor::VertexData v {};
                v.position = glm::vec3(wx, h, wz);
                v.uv = glm::vec2(h / TerrainParams::HEIGHT_SCALE, slope01);
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
                Vapor::VertexData v = verts[src];
                v.position.y -= skirtDepth;
                skirts[eIdx].push_back(static_cast<Uint32>(verts.size()));
                verts.push_back(v);
            }
        }
        aabbMin.y -= skirtDepth;

        inds.clear();
        inds.reserve(res * res * 6 + 4 * res * 12);
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

    // Palette LUT: u = height01, v = slope01. Valleys sand->grass, mid dirt,
    // peaks snow; steep slopes blend to rock everywhere.
    void buildPaletteMaterial(RenderScene& scene) {
        constexpr int N = 256;
        auto lut = std::make_shared<Vapor::Image>();
        lut->uri = "terrain_palette_lut";  // unique key for the texture cache
        lut->width = N;
        lut->height = N;
        lut->channelCount = 4;
        lut->byteArray.resize(N * N * 4);
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
        scene.images.push_back(lut);

        m_terrainMaterial = std::make_shared<Vapor::Material>();
        m_terrainMaterial->albedoMap = lut;
        m_terrainMaterial->roughnessFactor = 0.95f;
        m_terrainMaterial->metallicFactor = 0.0f;
        scene.materials.push_back(m_terrainMaterial);
    }

    void buildScatterPrototypes(RenderScene& scene) {
        auto treeMat = std::make_shared<Vapor::Material>();
        treeMat->baseColorFactor = glm::vec4(0.16f, 0.42f, 0.18f, 1.0f);
        treeMat->roughnessFactor = 0.9f;
        scene.materials.push_back(treeMat);
        m_treeMesh = MeshBuilder::buildCube(1.0f);
        m_treeMesh->material = treeMat;
        scene.addMesh(m_treeMesh);

        auto rockMat = std::make_shared<Vapor::Material>();
        rockMat->baseColorFactor = glm::vec4(0.45f, 0.44f, 0.42f, 1.0f);
        rockMat->roughnessFactor = 0.95f;
        scene.materials.push_back(rockMat);
        m_rockMesh = MeshBuilder::buildCube(1.0f);
        m_rockMesh->material = rockMat;
        scene.addMesh(m_rockMesh);
    }

    void retargetTiles(glm::ivec2 camTile) {
        for (int t = 0; t < static_cast<int>(m_tiles.size()); t++) {
            const int tx = t % TerrainParams::TILES, tz = t / TerrainParams::TILES;
            const int ring = std::max(std::abs(tx - camTile.x), std::abs(tz - camTile.y));
            int target = LOD_COUNT - 1;
            for (int lod = 0; lod < LOD_COUNT; lod++) {
                if (ring <= kLodRadius[lod]) {
                    target = lod;
                    break;
                }
            }
            Tile& tile = m_tiles[t];
            tile.targetLod = target;
            // Demotion to the always-resident base coat is free — apply now so
            // the freed slot can serve an incoming fine tile this frame.
            if (target == 3 && tile.currentLod < 3) releaseFineSlot(t);
        }
    }

    void releaseFineSlot(int t) {
        Tile& tile = m_tiles[t];
        if (tile.fineSlot >= 0) {
            Slot& slot = m_finePools[tile.currentLod][tile.fineSlot];
            m_registry->get<Vapor::MeshRendererComponent>(slot.entity).visible = false;
            m_freeSlots[tile.currentLod].push_back(tile.fineSlot);
            tile.fineSlot = -1;
        }
        m_registry->get<Vapor::MeshRendererComponent>(m_baseSlots[t].entity).visible = true;
        tile.currentLod = 3;
    }

    void enqueueBuilds() {
        for (int t = 0; t < static_cast<int>(m_tiles.size()); t++) {
            Tile& tile = m_tiles[t];
            if (tile.buildPending || tile.targetLod == tile.currentLod || tile.targetLod == 3) continue;
            if (m_freeSlots[tile.targetLod].empty()) continue;  // pool busy; retry next frame
            if (m_inFlight.load(std::memory_order_relaxed) >= 8) break;
            tile.buildPending = true;
            m_inFlight.fetch_add(1, std::memory_order_relaxed);
            const int lod = tile.targetLod;
            m_scheduler->submitTask([this, t, lod] {
                BuildResult r;
                r.tile = t;
                r.lod = lod;
                buildTileGeometry(t % TerrainParams::TILES, t / TerrainParams::TILES, lod, r.verts, r.inds,
                                  r.aabbMin, r.aabbMax);
                {
                    std::lock_guard<std::mutex> lock(m_resultMutex);
                    m_results.push_back(std::move(r));
                }
                m_inFlight.fetch_sub(1, std::memory_order_relaxed);
            });
        }
    }

    void applyResults() {
        for (int applied = 0; applied < 2; applied++) {
            BuildResult r;
            {
                std::lock_guard<std::mutex> lock(m_resultMutex);
                if (m_results.empty()) return;
                r = std::move(m_results.back());
                m_results.pop_back();
            }
            Tile& tile = m_tiles[r.tile];
            tile.buildPending = false;
            // Stale (rings moved on) or no slot left: drop; the enqueue scan
            // resubmits while the mismatch persists.
            if (tile.targetLod != r.lod || m_freeSlots[r.lod].empty()) continue;

            const int slotIdx = m_freeSlots[r.lod].back();
            m_freeSlots[r.lod].pop_back();
            Slot& slot = m_finePools[r.lod][slotIdx];
            if (!m_renderer->updateMeshGeometry(slot.mesh->renderMeshId, r.verts, r.inds)) {
                m_freeSlots[r.lod].push_back(slotIdx);
                continue;
            }
            slot.mesh->localAABBMin = r.aabbMin;
            slot.mesh->localAABBMax = r.aabbMax;
            m_registry->get<Vapor::MeshRendererComponent>(slot.entity).visible = true;

            // Swap out whatever the tile was showing before.
            if (tile.currentLod < 3 && tile.fineSlot >= 0) {
                Slot& old = m_finePools[tile.currentLod][tile.fineSlot];
                m_registry->get<Vapor::MeshRendererComponent>(old.entity).visible = false;
                m_freeSlots[tile.currentLod].push_back(tile.fineSlot);
            }
            m_registry->get<Vapor::MeshRendererComponent>(m_baseSlots[r.tile].entity).visible = false;
            tile.currentLod = r.lod;
            tile.fineSlot = slotIdx;
        }
    }

    // Deterministic tree/rock scatter, one instanced-mesh entity per
    // placement, spawned for tiles entering the scatter ring and destroyed on
    // exit (slope/height rules from the original demo).
    void updateScatter(glm::ivec2 camTile) {
        constexpr int RADIUS = 3;
        std::unordered_map<int, std::vector<entt::entity>> keep;
        for (int tz = camTile.y - RADIUS; tz <= camTile.y + RADIUS; tz++) {
            for (int tx = camTile.x - RADIUS; tx <= camTile.x + RADIUS; tx++) {
                if (tx < 0 || tz < 0 || tx >= TerrainParams::TILES || tz >= TerrainParams::TILES) continue;
                const int t = tz * TerrainParams::TILES + tx;
                auto it = m_scatter.find(t);
                if (it != m_scatter.end()) {
                    keep.emplace(t, std::move(it->second));
                    m_scatter.erase(it);
                    continue;
                }
                keep.emplace(t, spawnScatter(tx, tz));
            }
        }
        for (auto& [tile, ents] : m_scatter) {
            for (auto e : ents) m_registry->destroy(e);
        }
        m_scatter = std::move(keep);
    }

    std::vector<entt::entity> spawnScatter(int tx, int tz) {
        std::vector<entt::entity> out;
        const float half = 0.5f * TerrainParams::WORLD_SIZE;
        const float minX = tx * TerrainParams::TILE_SIZE - half;
        const float minZ = tz * TerrainParams::TILE_SIZE - half;
        Uint32 rng = static_cast<Uint32>(tx) * 73856093u ^ static_cast<Uint32>(tz) * 19349663u
            ^ TerrainParams::SEED;
        auto rand01 = [&rng] {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
        };
        for (int i = 0; i < 90; i++) {
            const float wx = minX + rand01() * TerrainParams::TILE_SIZE;
            const float wz = minZ + rand01() * TerrainParams::TILE_SIZE;
            const float y = heightAt(wx, wz);
            const float slope = slopeAt(wx, wz);
            const float hn = y / TerrainParams::HEIGHT_SCALE;
            std::shared_ptr<Vapor::Mesh> mesh;
            glm::vec3 pos, scale;
            if (slope < 0.35f && hn > 0.25f && hn < 0.62f) {
                const float s = 3.0f + rand01() * 5.0f;  // tree
                mesh = m_treeMesh;
                pos = { wx, y + 0.5f * s, wz };
                scale = { 0.5f * s, s, 0.5f * s };
            } else if (slope >= 0.35f && slope < 1.1f && rand01() < 0.35f) {
                const float s = 1.0f + rand01() * 3.0f;  // rock
                mesh = m_rockMesh;
                pos = { wx, y + 0.2f * s, wz };
                scale = { s, 0.6f * s, s };
            } else {
                continue;
            }
            auto e = m_registry->create();
            auto& tr = m_registry->emplace<Vapor::TransformComponent>(e);
            tr.position = pos;
            tr.rotation = glm::angleAxis(rand01() * 6.2832f, glm::vec3(0, 1, 0));
            tr.scale = scale;
            auto& mr = m_registry->emplace<Vapor::MeshRendererComponent>(e);
            mr.meshes.push_back(mesh);
            out.push_back(e);
        }
        return out;
    }

    entt::registry* m_registry = nullptr;
    Renderer* m_renderer = nullptr;
    Vapor::TaskScheduler* m_scheduler = nullptr;

    std::shared_ptr<Vapor::Material> m_terrainMaterial;
    std::shared_ptr<Vapor::Mesh> m_treeMesh, m_rockMesh;

    std::vector<Tile> m_tiles;
    std::vector<Slot> m_baseSlots;
    std::array<std::vector<Slot>, LOD_COUNT - 1> m_finePools;
    std::array<std::vector<int>, LOD_COUNT - 1> m_freeSlots;
    glm::ivec2 m_lastCamTile { -1000, -1000 };

    mutable std::mutex m_resultMutex;
    std::vector<BuildResult> m_results;
    std::atomic<int> m_inFlight { 0 };

    std::unordered_map<int, std::vector<entt::entity>> m_scatter;
};

// ECS fly-camera driver — same demo-local system as Examples/MicroVoxel;
// perspectiveZO because the RHI's clip depth is [0,1] on both backends.
// Sprint is x50 here (streaming stress), matching the original demo.
struct FlyCameraSystem {
    static void update(entt::registry& reg, const Vapor::InputState& input, float deltaTime) {
        auto view = reg.view<Vapor::VirtualCameraComponent, Vapor::FlyCameraComponent>();
        for (auto entity : view) {
            auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);
            auto& fly = view.get<Vapor::FlyCameraComponent>(entity);
            if (!cam.isActive) continue;

            glm::vec2 look = input.getVector(Vapor::InputAction::LookLeft, Vapor::InputAction::LookRight,
                                             Vapor::InputAction::LookDown, Vapor::InputAction::LookUp);
            glm::vec2 move = input.getVector(Vapor::InputAction::StrafeLeft, Vapor::InputAction::StrafeRight,
                                             Vapor::InputAction::MoveBackward, Vapor::InputAction::MoveForward);
            float vertical = input.getAxis(Vapor::InputAction::MoveDown, Vapor::InputAction::MoveUp);
            float moveSpeed = fly.moveSpeed * (input.isHeld(Vapor::InputAction::Sprint) ? 50.0f : 1.0f);

            fly.pitch -= look.y * fly.rotateSpeed * deltaTime;
            fly.yaw -= look.x * fly.rotateSpeed * deltaTime;
            fly.pitch = glm::clamp(fly.pitch, -89.0f, 89.0f);

            cam.rotation = glm::quat(glm::vec3(glm::radians(-fly.pitch), glm::radians(fly.yaw - 90.0f), 0.0f));
            glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);
            glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
            glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);
            cam.position += move.x * right * moveSpeed * deltaTime;
            cam.position += move.y * front * moveSpeed * deltaTime;
            cam.position += vertical * up * moveSpeed * deltaTime;

            glm::mat4 rotation = glm::mat4_cast(cam.rotation);
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
            cam.viewMatrix = glm::inverse(translation * rotation);
            cam.projectionMatrix = glm::perspectiveZO(cam.fov, cam.aspect, cam.near, cam.far);
        }
    }
};

entt::entity getActiveCamera(entt::registry& reg) {
    auto view = reg.view<Vapor::VirtualCameraComponent>();
    for (auto entity : view) {
        if (view.get<Vapor::VirtualCameraComponent>(entity).isActive) return entity;
    }
    return entt::null;
}

}  // namespace

auto main(int argc, char* args[]) -> int {
    bool wantVulkan = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(args[i], "--vulkan") == 0) wantVulkan = true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print(stderr, "SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    GraphicsBackend gfxBackend;
    const char* winTitle;
#if defined(__APPLE__)
    if (wantVulkan) {
        winTitle = "TerrainStreaming (Vulkan)";
        winFlags |= SDL_WINDOW_VULKAN;
        gfxBackend = GraphicsBackend::Vulkan;
    } else {
        winTitle = "TerrainStreaming (Metal)";
        winFlags |= SDL_WINDOW_METAL;
        gfxBackend = GraphicsBackend::Metal;
    }
#else
    winTitle = "TerrainStreaming (Vulkan)";
    winFlags |= SDL_WINDOW_VULKAN;
    gfxBackend = GraphicsBackend::Vulkan;
#endif

    auto window = SDL_CreateWindow(winTitle, 1280, 720, winFlags);
    if (!window) {
        fmt::print(stderr, "Failed to create SDL_Window: {}\n", SDL_GetError());
        return 1;
    }
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    auto renderer = createRenderer(gfxBackend, window);
    if (!renderer) {
        fmt::print(stderr, "Failed to create renderer (backend unavailable?)\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // The streamer rewrites tile meshes in place through the RHI renderer.
    auto* rhiRenderer = dynamic_cast<Renderer*>(renderer.get());
    if (!rhiRenderer) {
        fmt::print(stderr, "TerrainStreaming requires the RHI renderer\n");
        return 1;
    }

    auto scene = std::make_shared<RenderScene>("terrain");
    entt::registry registry;

    // ---- Environment -------------------------------------------------------
    {
        auto sun = registry.create();
        registry.emplace<Vapor::NameComponent>(sun, Vapor::NameComponent { "Sun" });
        auto& dl = registry.emplace<Vapor::DirectionalLightComponent>(sun);
        dl.direction = glm::normalize(glm::vec3(-0.4f, -1.0f, 0.35f));
        dl.color = glm::vec3(1.0f, 0.97f, 0.9f);
        dl.intensity = 8.0f;
        registry.emplace<Vapor::SunComponent>(sun);

        auto env = registry.create();
        registry.emplace<Vapor::NameComponent>(env, Vapor::NameComponent { "Environment" });
        registry.emplace<Vapor::SkyComponent>(env);
    }

    // ---- Terrain (prewarms the whole horizon before the first frame) -------
    const auto bootStart = SDL_GetTicks();
    TerrainStreamer terrain;
    terrain.init(registry, *scene, rhiRenderer, engineCore->getTaskScheduler());
    renderer->stage(scene);
    scene->stagedMeshes.clear();
    scene->stagedMeshTransforms.clear();
    fmt::print("TerrainStreaming: full {:.2f} km x {:.2f} km horizon ready in {} ms\n",
               TerrainParams::WORLD_SIZE / 1000.0f, TerrainParams::WORLD_SIZE / 1000.0f,
               SDL_GetTicks() - bootStart);

    // Camera: spawn high over the valley for an establishing vista; far plane
    // past the world diagonal so the whole streamed horizon is in view.
    {
        auto camEntity = registry.create();
        registry.emplace<Vapor::NameComponent>(camEntity, Vapor::NameComponent { "Fly Camera" });
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(camEntity);
        cam.isActive = true;
        cam.aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
        cam.fov = glm::radians(55.0f);
        cam.near = 0.5f;
        cam.far = 30000.0f;
        cam.position = glm::vec3(-382.1f, terrain.groundHeight(-382.1f, -2279.9f) + 40.0f, -2279.9f);
        auto& fly = registry.emplace<Vapor::FlyCameraComponent>(camEntity);
        fly.moveSpeed = 12.0f;  // sprinting-character pace; LShift = x50
        fly.rotateSpeed = 90.0f;
        fly.yaw = 50.0f;
        fly.pitch = 8.0f;
    }

    bool groundClamp = true;
    fmt::print("WASD move, R/F up/down, IJKL look, LShift sprint (x50), G ground-clamp, "
               "T teleport +2km, Esc quit.\n");

    auto& inputManager = engineCore->getInputManager();
    bool quit = false;
    float time = SDL_GetTicks() / 1000.0f;
    float statsTimer = 0.0f;

    while (!quit) {
        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = currTime - time;
        time = currTime;

        inputManager.update(deltaTime);

        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            inputManager.processEvent(e);
            switch (e.type) {
                case SDL_EVENT_QUIT:
                    quit = true;
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    if (e.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
                    if (e.key.repeat) break;
                    if (e.key.scancode == SDL_SCANCODE_G) {
                        groundClamp = !groundClamp;
                        fmt::print("Ground clamp: {}\n", groundClamp ? "on" : "off");
                    }
                    if (e.key.scancode == SDL_SCANCODE_T) {
                        // Teleport 2 km along the view direction, kept inside
                        // the world — a streaming stress test.
                        entt::entity ce = getActiveCamera(registry);
                        if (ce != entt::null) {
                            auto& cam = registry.get<Vapor::VirtualCameraComponent>(ce);
                            glm::vec3 fwd = cam.rotation * glm::vec3(0, 0, -1);
                            fwd.y = 0.0f;
                            fwd = glm::length(fwd) > 1e-3f ? glm::normalize(fwd) : glm::vec3(1, 0, 0);
                            const float bound = 0.5f * TerrainParams::WORLD_SIZE - TerrainParams::TILE_SIZE;
                            glm::vec3 pos = cam.position + fwd * 2000.0f;
                            pos.x = glm::clamp(pos.x, -bound, bound);
                            pos.z = glm::clamp(pos.z, -bound, bound);
                            pos.y = terrain.groundHeight(pos.x, pos.z) + 200.0f;
                            cam.position = pos;
                            fmt::print("Teleported to ({}, {})\n", static_cast<int>(pos.x),
                                       static_cast<int>(pos.z));
                        }
                    }
                    break;
                }
                case SDL_EVENT_WINDOW_RESIZED: {
                    windowWidth = e.window.data1;
                    windowHeight = e.window.data2;
                    auto camView = registry.view<Vapor::VirtualCameraComponent>();
                    camView.each([&](auto& cam) {
                        cam.aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
                    });
                    break;
                }
                default:
                    break;
            }
        }

        const auto& input = inputManager.getInputState();
        FlyCameraSystem::update(registry, input, deltaTime);

        entt::entity activeCam = getActiveCamera(registry);
        if (activeCam != entt::null) {
            auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCam);
            // Keep the fly camera above the streamed terrain (after movement,
            // so the clamp applies to this frame).
            if (groundClamp) {
                cam.position.y =
                    std::max(cam.position.y, terrain.groundHeight(cam.position.x, cam.position.z) + 2.0f);
            }
            terrain.update(cam.position);
        }

        engineCore->update(deltaTime);
        Vapor::TransformSystem::update(registry);
        Vapor::LightGatherSystem::update(registry, scene.get());
        Vapor::SkySystem::update(registry, renderer.get());

        statsTimer += deltaTime;
        if (statsTimer >= 5.0f) {
            statsTimer = 0.0f;
            const auto s = terrain.stats();
            entt::entity ce = getActiveCamera(registry);
            const glm::vec3 pos =
                ce != entt::null ? registry.get<Vapor::VirtualCameraComponent>(ce).position : glm::vec3(0.0f);
            fmt::print("cam ({}, {}) lod0 {} lod1 {} lod2 {} base {} pending {} queued {} scatter {}\n",
                       static_cast<int>(pos.x), static_cast<int>(pos.z), s.lodCounts[0], s.lodCounts[1],
                       s.lodCounts[2], s.lodCounts[3], s.pendingJobs, s.queuedResults, s.scatterEntities);
        }

        if (activeCam == entt::null) continue;
        const auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCam);
        Camera tempCamera;
        tempCamera.setEye(cam.position);
        tempCamera.setViewMatrix(cam.viewMatrix);
        tempCamera.setProjectionMatrix(cam.projectionMatrix);

        CameraRenderData camData;
        camData.proj = cam.projectionMatrix;
        camData.view = cam.viewMatrix;
        camData.invProj = glm::inverse(cam.projectionMatrix);
        camData.invView = glm::inverse(cam.viewMatrix);
        camData.nearPlane = cam.near;
        camData.farPlane = cam.far;
        camData.position = cam.position;

        renderer->beginFrame(camData);
        ImGui::NewFrame();
        renderer->invokeImGuiCallback();
        renderer->draw(registry, scene, tempCamera);
        ImGui::Render();
        renderer->endFrame();
    }

    // Streaming jobs capture `terrain`; drain them before it goes away.
    engineCore->getTaskScheduler().waitForAll();
    engineCore->shutdown();
    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
