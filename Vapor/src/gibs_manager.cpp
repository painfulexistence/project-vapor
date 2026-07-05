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

    // Note: createTextures() must be called separately after screen size is known
    // Call initTextures(width, height) from renderer after swapchain is ready

    SDL_Log("[GIBS] Initialized: %u max surfels, %u rays/surfel, %.1fx resolution",
            maxSurfels, raysPerSurfel, resolutionScale);
    SDL_Log("[GIBS] Spatial hash: %ux%ux%u cells (%.1fm cell size), total %u cells",
            gridSize.x, gridSize.y, gridSize.z, cellSize, totalCells);
}

void GIBSManager::initTextures(Uint32 screenWidth, Uint32 screenHeight) {
    Uint32 targetWidth = static_cast<Uint32>(screenWidth * resolutionScale);
    Uint32 targetHeight = static_cast<Uint32>(screenHeight * resolutionScale);

    if (giResultTexture) {
        // Already initialized, check if resize needed
        if (giTextureWidth == targetWidth && giTextureHeight == targetHeight) {
            return; // No change needed
        }
        // Release old textures
        giResultTexture.reset();
        giHistoryTexture.reset();
        SDL_Log("[GIBS] Resizing GI textures from %ux%u to %ux%u",
                giTextureWidth, giTextureHeight, targetWidth, targetHeight);
    }

    createTextures(screenWidth, screenHeight);
}

void GIBSManager::deinit() {
    surfelBuffer.reset();
    cellHeadBuffer.reset();
    surfelNextBuffer.reset();
    counterBuffer.reset();
    gibsDataBuffers.clear();
    giResultTexture.reset();
    giHistoryTexture.reset();

    SDL_Log("[GIBS] Deinitialized");
}

void GIBSManager::createBuffers() {
    MTL::Device* device = renderer->getDevice();

    // Surfel buffer - main storage
    // Design Decision: 128 bytes per surfel for balance of info and memory
    allocatedMaxSurfels = maxSurfels;
    size_t surfelBufferSize = maxSurfels * sizeof(Surfel);
    surfelBuffer = NS::TransferPtr(device->newBuffer(surfelBufferSize, MTL::ResourceStorageModePrivate));
    surfelBuffer->setLabel(NS::String::string("GIBS Surfel Buffer", NS::UTF8StringEncoding));

    // Linked-list spatial hash: head index per cell + next index per surfel.
    // Replaces the counting-sort layout (sorted copy + prefix sum) whose serial
    // scan over every cell dominated frame time.
    size_t cellHeadBufferSize = totalCells * sizeof(Uint32);
    cellHeadBuffer = NS::TransferPtr(device->newBuffer(cellHeadBufferSize, MTL::ResourceStorageModePrivate));
    cellHeadBuffer->setLabel(NS::String::string("GIBS Cell Head Buffer", NS::UTF8StringEncoding));

    surfelNextBuffer = NS::TransferPtr(device->newBuffer(maxSurfels * sizeof(Uint32), MTL::ResourceStorageModePrivate));
    surfelNextBuffer->setLabel(NS::String::string("GIBS Surfel Next Buffer", NS::UTF8StringEncoding));

    // Counter buffer - atomic counters for surfel allocation
    counterBuffer = NS::TransferPtr(device->newBuffer(COUNTER_BUFFER_SIZE * sizeof(Uint32),
                                                       MTL::ResourceStorageModeShared));
    counterBuffer->setLabel(NS::String::string("GIBS Counter Buffer", NS::UTF8StringEncoding));

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

    SDL_Log("[GIBS] Buffers created: Surfels=%.1fMB, CellHeads=%.1fMB",
            surfelBufferSize / (1024.0f * 1024.0f),
            cellHeadBufferSize / (1024.0f * 1024.0f));
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

    // Buffers are sized once at init; clamp to allocated capacity so a runtime
    // quality change never lets GPU kernels write past the allocated pool
    if (allocatedMaxSurfels > 0) {
        maxSurfels = std::min(maxSurfels, allocatedMaxSurfels);
    }

    // Update GIBS data
    gibsData.maxSurfels = maxSurfels;
    gibsData.raysPerSurfel = raysPerSurfel;
}

void GIBSManager::setWorldBounds(const glm::vec3& min, const glm::vec3& max) {
    worldMin = min;
    worldMax = max;

    // Recalculate grid if already initialized
    if (cellHeadBuffer) {
        calculateGridSize();
        // Note: Would need to recreate the cell head buffer if size changed significantly
    }
}

void GIBSManager::onSceneLoaded(std::shared_ptr<RenderScene> scene) {
    // Calculate world bounds from scene
    // This could iterate through all meshes to find actual bounds
    // For now, use default large bounds

    SDL_Log("[GIBS] Scene loaded, ready for surfel generation");
}

void GIBSManager::beginFrame(Uint32 frameIndex) {
    currentFrameIndex = frameIndex % MAX_FRAMES_IN_FLIGHT;

    // Read surfel count from previous frame's GPU work (1 frame latency, no sync stall).
    // counter[1] is the persistent pool allocation cursor incremented by the
    // generation kernel; it may slightly overshoot maxSurfels (racy budget check), so clamp.
    Uint32* counterPtr = static_cast<Uint32*>(counterBuffer->contents());
    activeSurfelCount = std::min(counterPtr[1], maxSurfels);

    // Reset the per-frame generation budget counter (counter[1] is never reset)
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
    // 25cm radius: coverage need per area scales with 1/r², so this is ~6x
    // fewer surfels than the previous 10cm for the same coverage
    gibsData.surfelRadius = 0.25f;
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

    gibsData.sampleRadius = 1; // Neighbor cell search radius IN CELLS (3^3 = 27 cells)
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
