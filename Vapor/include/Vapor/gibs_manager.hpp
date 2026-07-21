#pragma once
#include "graphics.hpp"
#include "graphics_gibs.hpp"  // Surfel, SurfelCell, GIBSData, GIBSQuality (branch graphics.hpp is a monolith, not an umbrella)
#include "render_scene.hpp"
#include <Metal/Metal.hpp>
#include <memory>
#include <vector>

namespace Vapor {

class Renderer_Metal;

/**
 * GIBSManager - Global Illumination Based on Surfels Manager
 *
 * Manages the lifecycle of surfels for indirect lighting:
 * - GPU buffer allocation and management
 * - Spatial hash construction
 * - Surfel generation from scene geometry
 * - Quality preset application
 *
 * Design decisions documented in docs/GIBS_DESIGN.md
 */
class GIBSManager {
public:
    explicit GIBSManager(Renderer_Metal* renderer);
    ~GIBSManager();

    // Lifecycle
    void init();
    void initTextures(Uint32 screenWidth, Uint32 screenHeight);
    void deinit();

    // Per-frame update
    void beginFrame(Uint32 frameIndex);
    void updateGIBSData(const glm::mat4& viewProj, const glm::mat4& invViewProj,
                        const glm::vec3& cameraPos, const glm::vec3& sunDir,
                        const glm::vec3& sunColor, float sunIntensity);

    // Scene management
    void setWorldBounds(const glm::vec3& min, const glm::vec3& max);
    void onSceneLoaded(std::shared_ptr<RenderScene> scene);

    // Quality settings
    void setQuality(GIBSQuality quality);
    GIBSQuality getQuality() const { return currentQuality; }

    // Buffer access for render passes
    MTL::Buffer* getSurfelBuffer() const { return surfelBuffer.get(); }
    MTL::Buffer* getCellHeadBuffer() const { return cellHeadBuffer.get(); }
    MTL::Buffer* getSurfelNextBuffer() const { return surfelNextBuffer.get(); }
    MTL::Buffer* getCounterBuffer() const { return counterBuffer.get(); }
    MTL::Buffer* getGIBSDataBuffer(Uint32 frameIndex) const;
    MTL::Texture* getGIResultTexture() const { return giResultTexture.get(); }
    MTL::Texture* getGIHistoryTexture() const { return giHistoryTexture.get(); }

    // State queries
    Uint32 getMaxSurfels() const { return maxSurfels; }
    Uint32 getActiveSurfelCount() const { return activeSurfelCount; }
    Uint32 getRaysPerSurfel() const { return raysPerSurfel; }
    float getResolutionScale() const { return resolutionScale; }
    const GIBSData& getGIBSData() const { return gibsData; }
    glm::uvec3 getGridSize() const { return gridSize; }
    Uint32 getTotalCells() const { return totalCells; }

    // Manual surfel count update (called by generation pass)
    void setActiveSurfelCount(Uint32 count) { activeSurfelCount = count; }

    // Debug: raw GPU counter values (x = per-frame budget cursor, y = persistent pool cursor)
    glm::uvec2 getRawCounters() const {
        const Uint32* p = static_cast<const Uint32*>(counterBuffer->contents());
        return { p[0], p[1] };
    }

    // Debug: discard all surfels and let generation repopulate from scratch.
    // Stale surfels beyond the new count stay in the buffer but every consumer
    // gates on activeSurfelCount, so they are never read.
    void resetSurfels() {
        Uint32* p = static_cast<Uint32*>(counterBuffer->contents());
        p[0] = 0;
        p[1] = 0;
        activeSurfelCount = 0;
    }

    // Swap history buffers (called after temporal pass)
    void swapHistoryBuffers();

    // Debug
    bool debugVisualization = false;

private:
    Renderer_Metal* renderer;

    // Quality settings
    GIBSQuality currentQuality = GIBSQuality::Medium;
    Uint32 maxSurfels = 500000;
    Uint32 raysPerSurfel = 4;
    float resolutionScale = 0.5f;

    // Runtime state
    Uint32 allocatedMaxSurfels = 0; // Buffer capacity fixed at init; quality changes clamp to this
    Uint32 activeSurfelCount = 0;
    Uint32 currentFrameIndex = 0;
    glm::mat4 prevViewProj = glm::mat4(1.0f);

    // World bounds and spatial hash
    // Bounds cover the GI-relevant area around the play space, NOT the whole
    // world: cell count grows cubically and the hash clear touches every cell.
    glm::vec3 worldMin = glm::vec3(-64.0f);
    glm::vec3 worldMax = glm::vec3(64.0f);
    float cellSize = 1.0f;
    glm::uvec3 gridSize;
    Uint32 totalCells = 0;

    // GIBS uniform data
    GIBSData gibsData;

    // GPU Buffers (linked-list spatial hash: head index per cell + next index per surfel)
    NS::SharedPtr<MTL::Buffer> surfelBuffer;        // Canonical surfel array
    NS::SharedPtr<MTL::Buffer> cellHeadBuffer;      // First surfel index per cell (u32)
    NS::SharedPtr<MTL::Buffer> surfelNextBuffer;    // Next surfel index per surfel (u32)
    NS::SharedPtr<MTL::Buffer> counterBuffer;       // Atomic counters
    std::vector<NS::SharedPtr<MTL::Buffer>> gibsDataBuffers; // Per-frame uniform

    // GI result textures (ping-pong for temporal)
    NS::SharedPtr<MTL::Texture> giResultTexture;
    NS::SharedPtr<MTL::Texture> giHistoryTexture;
    Uint32 giTextureWidth = 0;
    Uint32 giTextureHeight = 0;

    // Initialization helpers
    void createBuffers();
    void createTextures(Uint32 screenWidth, Uint32 screenHeight);
    void calculateGridSize();

    // Constants
    static constexpr Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    static constexpr Uint32 COUNTER_BUFFER_SIZE = 16; // Multiple counters
};

} // namespace Vapor
