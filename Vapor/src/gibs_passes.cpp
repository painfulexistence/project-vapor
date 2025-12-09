#include "Vapor/gibs_passes.hpp"
#include "Vapor/gibs_manager.hpp"
#include <SDL3/SDL_log.h>

namespace Vapor {

// ============================================================================
// SurfelGenerationPass
// Generates surfels from G-buffer (depth, normal, albedo)
// ============================================================================

SurfelGenerationPass::SurfelGenerationPass(Renderer_Metal* renderer, GIBSManager* gibsManager)
    : RenderPass(renderer), gibsManager(gibsManager) {
}

void SurfelGenerationPass::execute() {
    if (!gibsManager) return;

    auto& r = *renderer;

    // Get screen size
    auto drawableSize = r.swapchain->drawableSize();
    glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

    // Prepare generation parameters
    SurfelGenerationParams params;
    params.invViewProj = gibsManager->getGIBSData().invViewProj;
    params.screenSize = screenSize;
    params.surfelRadius = gibsManager->getGIBSData().surfelRadius;
    params.densityThreshold = 0.01f; // 1% probability per pixel
    params.maxNewSurfels = gibsManager->getMaxSurfels() / 10; // Generate up to 10% per frame
    params.frameIndex = r.frameNumber;

    // Encode compute pass
    auto encoder = r.currentCommandBuffer->computeCommandEncoder();
    encoder->setLabel(NS::String::string("Surfel Generation", NS::UTF8StringEncoding));

    encoder->setComputePipelineState(r.surfelGenerationPipeline.get());

    // Textures
    encoder->setTexture(r.depthStencilRT.get(), 0);
    encoder->setTexture(r.normalRT.get(), 1);
    encoder->setTexture(r.colorRT.get(), 2); // Using color as albedo proxy

    // Buffers
    encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
    encoder->setBuffer(gibsManager->getCounterBuffer(), 0, 1);
    encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);
    encoder->setBytes(&params, sizeof(params), 3);

    // Dispatch - one thread per pixel at reduced resolution for performance
    uint32_t dispatchX = (static_cast<uint32_t>(screenSize.x) + 7) / 8;
    uint32_t dispatchY = (static_cast<uint32_t>(screenSize.y) + 7) / 8;
    encoder->dispatchThreadgroups(MTL::Size(dispatchX, dispatchY, 1), MTL::Size(8, 8, 1));

    encoder->endEncoding();
}

// ============================================================================
// SurfelHashBuildPass
// Builds spatial hash for fast surfel neighbor queries
// ============================================================================

SurfelHashBuildPass::SurfelHashBuildPass(Renderer_Metal* renderer, GIBSManager* gibsManager)
    : RenderPass(renderer), gibsManager(gibsManager) {
}

void SurfelHashBuildPass::execute() {
    if (!gibsManager) return;

    auto& r = *renderer;
    const auto& gibsData = gibsManager->getGIBSData();

    // Step 1: Clear cell counts
    {
        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setLabel(NS::String::string("Clear Cell Counts", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.surfelClearCellsPipeline.get());
        encoder->setBuffer(gibsManager->getCellCountBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 1);

        uint32_t totalCells = gibsManager->getTotalCells();
        uint32_t threadGroupSize = 256;
        uint32_t threadGroups = (totalCells + threadGroupSize - 1) / threadGroupSize;
        encoder->dispatchThreadgroups(MTL::Size(threadGroups, 1, 1), MTL::Size(threadGroupSize, 1, 1));
        encoder->endEncoding();
    }

    // Step 2: Count surfels per cell
    {
        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setLabel(NS::String::string("Count Surfels Per Cell", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.surfelCountPerCellPipeline.get());
        encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getCellCountBuffer(), 0, 1);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);

        uint32_t activeSurfels = gibsManager->getActiveSurfelCount();
        if (activeSurfels == 0) activeSurfels = 1; // Avoid zero dispatch
        uint32_t threadGroupSize = 256;
        uint32_t threadGroups = (activeSurfels + threadGroupSize - 1) / threadGroupSize;
        encoder->dispatchThreadgroups(MTL::Size(threadGroups, 1, 1), MTL::Size(threadGroupSize, 1, 1));
        encoder->endEncoding();
    }

    // Step 3: Prefix sum for cell offsets (serial version for simplicity)
    {
        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setLabel(NS::String::string("Prefix Sum Cells", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.surfelPrefixSumPipeline.get());
        encoder->setBuffer(gibsManager->getCellCountBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getCellBuffer(), 0, 1);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);

        // Single thread for serial prefix sum (could optimize with parallel scan)
        encoder->dispatchThreadgroups(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }

    // Step 4: Scatter surfels to sorted positions
    {
        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setLabel(NS::String::string("Scatter Surfels", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.surfelScatterPipeline.get());
        encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getSurfelBufferSorted(), 0, 1);
        encoder->setBuffer(gibsManager->getCellBuffer(), 0, 2);
        encoder->setBuffer(gibsManager->getCellCountBuffer(), 0, 3); // Reused as write offsets
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 4);

        uint32_t activeSurfels = gibsManager->getActiveSurfelCount();
        if (activeSurfels == 0) activeSurfels = 1;
        uint32_t threadGroupSize = 256;
        uint32_t threadGroups = (activeSurfels + threadGroupSize - 1) / threadGroupSize;
        encoder->dispatchThreadgroups(MTL::Size(threadGroups, 1, 1), MTL::Size(threadGroupSize, 1, 1));
        encoder->endEncoding();
    }
}

// ============================================================================
// SurfelRaytracingPass
// Traces rays from surfels to compute indirect lighting
// ============================================================================

SurfelRaytracingPass::SurfelRaytracingPass(Renderer_Metal* renderer, GIBSManager* gibsManager)
    : RenderPass(renderer), gibsManager(gibsManager) {
}

void SurfelRaytracingPass::execute() {
    if (!gibsManager) return;

    auto& r = *renderer;
    const auto& gibsData = gibsManager->getGIBSData();

    // Prepare raytracing parameters
    SurfelRaytracingParams params;
    params.surfelOffset = 0;
    params.surfelCount = gibsManager->getActiveSurfelCount();
    params.raysPerSurfel = gibsManager->getRaysPerSurfel();
    params.frameIndex = r.frameNumber;
    params.rayBias = gibsData.rayBias;
    params.rayMaxDistance = gibsData.rayMaxDistance;

    if (params.surfelCount == 0) return;

    auto encoder = r.currentCommandBuffer->computeCommandEncoder();
    encoder->setLabel(NS::String::string("Surfel Raytracing", NS::UTF8StringEncoding));

    // Use simple version without acceleration structure for now
    // TODO: Switch to full raytracing version when acceleration structure is ready
    encoder->setComputePipelineState(r.surfelRaytracingSimplePipeline.get());

    encoder->setBuffer(gibsManager->getSurfelBufferSorted(), 0, 0);
    encoder->setBuffer(gibsManager->getCellBuffer(), 0, 1);
    encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);
    encoder->setBytes(&params, sizeof(params), 3);

    // Optionally set acceleration structure for full raytracing
    // encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 4);

    uint32_t threadGroupSize = 64;
    uint32_t threadGroups = (params.surfelCount + threadGroupSize - 1) / threadGroupSize;
    encoder->dispatchThreadgroups(MTL::Size(threadGroups, 1, 1), MTL::Size(threadGroupSize, 1, 1));

    encoder->endEncoding();
}

// ============================================================================
// GIBSTemporalPass
// Applies temporal filtering to reduce noise
// ============================================================================

GIBSTemporalPass::GIBSTemporalPass(Renderer_Metal* renderer, GIBSManager* gibsManager)
    : RenderPass(renderer), gibsManager(gibsManager) {
}

void GIBSTemporalPass::execute() {
    if (!gibsManager) return;

    auto& r = *renderer;

    // Apply temporal smoothing to surfels
    auto encoder = r.currentCommandBuffer->computeCommandEncoder();
    encoder->setLabel(NS::String::string("GIBS Temporal", NS::UTF8StringEncoding));
    encoder->setComputePipelineState(r.gibsTemporalPipeline.get());

    encoder->setBuffer(gibsManager->getSurfelBufferSorted(), 0, 0);
    encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 1);

    uint32_t activeSurfels = gibsManager->getActiveSurfelCount();
    if (activeSurfels == 0) return;

    uint32_t threadGroupSize = 256;
    uint32_t threadGroups = (activeSurfels + threadGroupSize - 1) / threadGroupSize;
    encoder->dispatchThreadgroups(MTL::Size(threadGroups, 1, 1), MTL::Size(threadGroupSize, 1, 1));

    encoder->endEncoding();

    // Swap history buffers for next frame
    gibsManager->swapHistoryBuffers();
}

// ============================================================================
// GIBSSamplePass
// Samples indirect lighting from surfels to screen pixels
// ============================================================================

GIBSSamplePass::GIBSSamplePass(Renderer_Metal* renderer, GIBSManager* gibsManager)
    : RenderPass(renderer), gibsManager(gibsManager) {
}

void GIBSSamplePass::execute() {
    if (!gibsManager) return;

    auto& r = *renderer;
    const auto& gibsData = gibsManager->getGIBSData();

    auto drawableSize = r.swapchain->drawableSize();
    glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

    // GI resolution based on quality setting
    float scale = gibsManager->getResolutionScale();
    glm::vec2 giResolution = screenSize * scale;

    // Prepare sample parameters
    GIBSSampleParams params;
    params.invViewProj = gibsData.invViewProj;
    params.screenSize = screenSize;
    params.giResolution = giResolution;
    params.sampleRadius = static_cast<float>(gibsData.sampleRadius) * gibsData.cellSize;
    params.maxSamples = gibsData.maxSurfelsPerPixel;
    params.normalWeight = 1.0f;
    params.distanceWeight = 1.0f;

    // Sample GI at reduced resolution
    {
        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setLabel(NS::String::string("GIBS Sample", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.gibsSamplePipeline.get());

        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(gibsManager->getGIResultTexture(), 2);

        encoder->setBuffer(gibsManager->getSurfelBufferSorted(), 0, 0);
        encoder->setBuffer(gibsManager->getCellBuffer(), 0, 1);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);
        encoder->setBytes(&params, sizeof(params), 3);

        uint32_t dispatchX = (static_cast<uint32_t>(giResolution.x) + 7) / 8;
        uint32_t dispatchY = (static_cast<uint32_t>(giResolution.y) + 7) / 8;
        encoder->dispatchThreadgroups(MTL::Size(dispatchX, dispatchY, 1), MTL::Size(8, 8, 1));

        encoder->endEncoding();
    }

    // Bilateral upsample to full resolution if needed
    if (scale < 1.0f) {
        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setLabel(NS::String::string("GIBS Bilateral Upsample", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.gibsUpsamplePipeline.get());

        encoder->setTexture(gibsManager->getGIResultTexture(), 0);
        encoder->setTexture(r.depthStencilRT.get(), 1);
        encoder->setTexture(r.normalRT.get(), 2);
        encoder->setTexture(gibsManager->getGIHistoryTexture(), 3); // Use history as full-res output

        encoder->setBytes(&params, sizeof(params), 0);

        uint32_t dispatchX = (static_cast<uint32_t>(screenSize.x) + 7) / 8;
        uint32_t dispatchY = (static_cast<uint32_t>(screenSize.y) + 7) / 8;
        encoder->dispatchThreadgroups(MTL::Size(dispatchX, dispatchY, 1), MTL::Size(8, 8, 1));

        encoder->endEncoding();
    }
}

} // namespace Vapor
