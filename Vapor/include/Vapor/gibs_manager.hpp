#pragma once
#include "graphics.hpp"
#include "scene.hpp"
#include <Metal/Metal.hpp>
#include <memory>
#include <vector>

// Forward declaration
class Renderer_Metal;

namespace Vapor {

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
    void deinit();

    // Per-frame update
    void beginFrame(Uint32 frameIndex);
    void updateGIBSData(const glm::mat4& viewProj, const glm::mat4& invViewProj,
                        const glm::vec3& cameraPos, const glm::vec3& sunDir,
                        const glm::vec3& sunColor, float sunIntensity);

    // Scene management
    void setWorldBounds(const glm::vec3& min, const glm::vec3& max);
    void onSceneLoaded(std::shared_ptr<Scene> scene);

    // Quality settings
    void setQuality(GIBSQuality quality);
    GIBSQuality getQuality() const { return currentQuality; }

    // Buffer access for render passes
    MTL::Buffer* getSurfelBuffer() const { return surfelBuffer.get(); }
    MTL::Buffer* getSurfelBufferSorted() const { return surfelBufferSorted.get(); }
    MTL::Buffer* getCellBuffer() const { return cellBuffer.get(); }
    MTL::Buffer* getCounterBuffer() const { return counterBuffer.get(); }
    MTL::Buffer* getCellCountBuffer() const { return cellCountBuffer.get(); }
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
    Uint32 activeSurfelCount = 0;
    Uint32 currentFrameIndex = 0;
    glm::mat4 prevViewProj = glm::mat4(1.0f);

    // World bounds and spatial hash
    glm::vec3 worldMin = glm::vec3(-500.0f);
    glm::vec3 worldMax = glm::vec3(500.0f);
    float cellSize = 2.0f;
    glm::uvec3 gridSize;
    Uint32 totalCells = 0;

    // GIBS uniform data
    GIBSData gibsData;

    // GPU Buffers
    NS::SharedPtr<MTL::Buffer> surfelBuffer;        // Main surfel array
    NS::SharedPtr<MTL::Buffer> surfelBufferSorted;  // Sorted by cell hash
    NS::SharedPtr<MTL::Buffer> cellBuffer;          // Spatial hash cells
    NS::SharedPtr<MTL::Buffer> counterBuffer;       // Atomic counters
    NS::SharedPtr<MTL::Buffer> cellCountBuffer;     // Per-cell surfel counts
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
