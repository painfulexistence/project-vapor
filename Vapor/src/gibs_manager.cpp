#include "Vapor/gibs_manager.hpp"
#include "Vapor/renderer_metal.hpp"
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cmath>

namespace Vapor {

GIBSManager::GIBSManager(Renderer_Metal* renderer)
    : renderer(renderer) {
}

GIBSManager::~GIBSManager() {
    deinit();
}

void GIBSManager::init() {
    SDL_Log("[GIBS] Initializing with quality: %d", static_cast<int>(currentQuality));

    // Apply quality settings
    setQuality(currentQuality);

    // Calculate spatial hash grid
    calculateGridSize();

    // Create GPU buffers
    createBuffers();

    SDL_Log("[GIBS] Initialized: %u max surfels, %u rays/surfel, %.1fx resolution",
            maxSurfels, raysPerSurfel, resolutionScale);
    SDL_Log("[GIBS] Spatial hash: %ux%ux%u cells (%.1fm cell size), total %u cells",
            gridSize.x, gridSize.y, gridSize.z, cellSize, totalCells);
}

void GIBSManager::deinit() {
    surfelBuffer.reset();
    surfelBufferSorted.reset();
    cellBuffer.reset();
    counterBuffer.reset();
    cellCountBuffer.reset();
    gibsDataBuffers.clear();
    giResultTexture.reset();
    giHistoryTexture.reset();

    SDL_Log("[GIBS] Deinitialized");
}

void GIBSManager::createBuffers() {
    MTL::Device* device = renderer->getDevice();

    // Surfel buffer - main storage
    // Design Decision: 128 bytes per surfel for balance of info and memory
    size_t surfelBufferSize = maxSurfels * sizeof(Surfel);
    surfelBuffer = NS::TransferPtr(device->newBuffer(surfelBufferSize, MTL::ResourceStorageModePrivate));
    surfelBuffer->setLabel(NS::String::string("GIBS Surfel Buffer", NS::UTF8StringEncoding));

    // Sorted surfel buffer (for spatial hash ordering)
    surfelBufferSorted = NS::TransferPtr(device->newBuffer(surfelBufferSize, MTL::ResourceStorageModePrivate));
    surfelBufferSorted->setLabel(NS::String::string("GIBS Surfel Buffer Sorted", NS::UTF8StringEncoding));

    // Cell buffer - spatial hash cells
    size_t cellBufferSize = totalCells * sizeof(SurfelCell);
    cellBuffer = NS::TransferPtr(device->newBuffer(cellBufferSize, MTL::ResourceStorageModePrivate));
    cellBuffer->setLabel(NS::String::string("GIBS Cell Buffer", NS::UTF8StringEncoding));

    // Counter buffer - atomic counters for surfel allocation
    counterBuffer = NS::TransferPtr(device->newBuffer(COUNTER_BUFFER_SIZE * sizeof(Uint32),
                                                       MTL::ResourceStorageModeShared));
    counterBuffer->setLabel(NS::String::string("GIBS Counter Buffer", NS::UTF8StringEncoding));

    // Cell count buffer - per-cell surfel counts for prefix sum
    cellCountBuffer = NS::TransferPtr(device->newBuffer(totalCells * sizeof(Uint32),
                                                         MTL::ResourceStorageModePrivate));
    cellCountBuffer->setLabel(NS::String::string("GIBS Cell Count Buffer", NS::UTF8StringEncoding));

    // Per-frame GIBS data buffers (triple buffered)
    gibsDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (Uint32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        gibsDataBuffers[i] = NS::TransferPtr(device->newBuffer(sizeof(GIBSData),
                                                                MTL::ResourceStorageModeShared));
        gibsDataBuffers[i]->setLabel(NS::String::string("GIBS Data Buffer", NS::UTF8StringEncoding));
    }

    // Initialize counter to 0
    Uint32* counterPtr = static_cast<Uint32*>(counterBuffer->contents());
    for (Uint32 i = 0; i < COUNTER_BUFFER_SIZE; i++) {
        counterPtr[i] = 0;
    }

    SDL_Log("[GIBS] Buffers created: Surfels=%.1fMB, Cells=%.1fMB",
            surfelBufferSize / (1024.0f * 1024.0f),
            cellBufferSize / (1024.0f * 1024.0f));
}

void GIBSManager::createTextures(Uint32 screenWidth, Uint32 screenHeight) {
    MTL::Device* device = renderer->getDevice();

    // Calculate GI buffer resolution based on quality
    giTextureWidth = static_cast<Uint32>(screenWidth * resolutionScale);
    giTextureHeight = static_cast<Uint32>(screenHeight * resolutionScale);

    // Ensure minimum size
    giTextureWidth = std::max(giTextureWidth, 64u);
    giTextureHeight = std::max(giTextureHeight, 64u);

    // GI result texture - RGBA16F for HDR indirect lighting
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA16Float,
        giTextureWidth,
        giTextureHeight,
        false
    );
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    desc->setStorageMode(MTL::StorageModePrivate);

    giResultTexture = NS::TransferPtr(device->newTexture(desc));
    giResultTexture->setLabel(NS::String::string("GIBS Result Texture", NS::UTF8StringEncoding));

    // History texture for temporal stability
    giHistoryTexture = NS::TransferPtr(device->newTexture(desc));
    giHistoryTexture->setLabel(NS::String::string("GIBS History Texture", NS::UTF8StringEncoding));

    SDL_Log("[GIBS] GI textures created: %ux%u (%.1fx scale)",
            giTextureWidth, giTextureHeight, resolutionScale);
}

void GIBSManager::calculateGridSize() {
    // Calculate grid dimensions based on world bounds and cell size
    glm::vec3 worldSize = worldMax - worldMin;

    gridSize.x = static_cast<Uint32>(std::ceil(worldSize.x / cellSize));
    gridSize.y = static_cast<Uint32>(std::ceil(worldSize.y / cellSize));
    gridSize.z = static_cast<Uint32>(std::ceil(worldSize.z / cellSize));

    // Clamp to reasonable limits (max 256 per dimension for memory)
    gridSize.x = std::min(gridSize.x, 256u);
    gridSize.y = std::min(gridSize.y, 256u);
    gridSize.z = std::min(gridSize.z, 256u);

    totalCells = gridSize.x * gridSize.y * gridSize.z;

    // Adjust cell size to match grid
    cellSize = std::max({
        worldSize.x / gridSize.x,
        worldSize.y / gridSize.y,
        worldSize.z / gridSize.z
    });
}

void GIBSManager::setQuality(GIBSQuality quality) {
    currentQuality = quality;
    getGIBSQualitySettings(quality, maxSurfels, raysPerSurfel, resolutionScale);

    // Update GIBS data
    gibsData.maxSurfels = maxSurfels;
    gibsData.raysPerSurfel = raysPerSurfel;
}

void GIBSManager::setWorldBounds(const glm::vec3& min, const glm::vec3& max) {
    worldMin = min;
    worldMax = max;

    // Recalculate grid if already initialized
    if (cellBuffer) {
        calculateGridSize();
        // Note: Would need to recreate cell buffer if size changed significantly
    }
}

void GIBSManager::onSceneLoaded(std::shared_ptr<Scene> scene) {
    // Calculate world bounds from scene
    // This could iterate through all meshes to find actual bounds
    // For now, use default large bounds

    SDL_Log("[GIBS] Scene loaded, ready for surfel generation");
}

void GIBSManager::beginFrame(Uint32 frameIndex) {
    currentFrameIndex = frameIndex % MAX_FRAMES_IN_FLIGHT;

    // Reset surfel counter at frame start
    Uint32* counterPtr = static_cast<Uint32*>(counterBuffer->contents());
    // Counter 0: new surfel allocation
    // Counter 1: active surfel count (preserved across frames for stability)
    counterPtr[0] = 0;
}

void GIBSManager::updateGIBSData(const glm::mat4& viewProj, const glm::mat4& invViewProj,
                                  const glm::vec3& cameraPos, const glm::vec3& sunDir,
                                  const glm::vec3& sunColor, float sunIntensity) {
    // Update GIBS uniform data
    gibsData.invViewProj = invViewProj;
    gibsData.prevViewProj = prevViewProj;
    gibsData.cameraPosition = cameraPos;

    gibsData.sunDirection = glm::normalize(sunDir);
    gibsData.sunColor = sunColor;
    gibsData.sunIntensity = sunIntensity;

    gibsData.maxSurfels = maxSurfels;
    gibsData.activeSurfelCount = activeSurfelCount;
    gibsData.surfelRadius = 0.1f; // 10cm default radius
    gibsData.surfelDensity = 4.0f; // 4 surfels per m²

    gibsData.worldMin = worldMin;
    gibsData.cellSize = cellSize;
    gibsData.worldMax = worldMax;
    gibsData.totalCells = totalCells;
    gibsData.gridSize = gridSize;

    gibsData.raysPerSurfel = raysPerSurfel;
    gibsData.maxBounces = 1;
    gibsData.rayBias = 0.001f;
    gibsData.rayMaxDistance = 100.0f;

    gibsData.temporalBlend = 0.1f; // 10% new, 90% history
    gibsData.hysteresis = 0.95f;
    gibsData.frameIndex = currentFrameIndex;

    gibsData.sampleRadius = 2; // Search 2 cells radius
    gibsData.maxSurfelsPerPixel = 8;

    // Copy to GPU buffer
    GIBSData* bufferPtr = static_cast<GIBSData*>(gibsDataBuffers[currentFrameIndex]->contents());
    *bufferPtr = gibsData;

    // Store current viewProj for next frame
    prevViewProj = viewProj;
}

MTL::Buffer* GIBSManager::getGIBSDataBuffer(Uint32 frameIndex) const {
    return gibsDataBuffers[frameIndex % MAX_FRAMES_IN_FLIGHT].get();
}

void GIBSManager::swapHistoryBuffers() {
    std::swap(giResultTexture, giHistoryTexture);
}

} // namespace Vapor
