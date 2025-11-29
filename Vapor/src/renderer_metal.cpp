#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "renderer_metal.hpp"

#include <fmt/core.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_metal.h"
#include <vector>
#include <functional>

#include "graphics.hpp"
#include "asset_manager.hpp"
#include "helper.hpp"

// ============================================================================
// Render Pass Implementations
// ============================================================================

// Pre-pass: Renders depth and normals
class PrePass : public RenderPass {
public:
    explicit PrePass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "PrePass"; }

    void execute() override {
        auto& r = *renderer;

        // Create render pass descriptor
        auto prePassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto prePassNormalRT = prePassDesc->colorAttachments()->object(0);
        prePassNormalRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        prePassNormalRT->setLoadAction(MTL::LoadActionClear);
        prePassNormalRT->setStoreAction(MTL::StoreActionStore);
        prePassNormalRT->setTexture(r.normalRT_MS.get());

        auto prePassDepthRT = prePassDesc->depthAttachment();
        prePassDepthRT->setClearDepth(clearDepth);
        prePassDepthRT->setLoadAction(MTL::LoadActionClear);
        prePassDepthRT->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
        prePassDepthRT->setDepthResolveFilter(MTL::MultisampleDepthResolveFilter::MultisampleDepthResolveFilterMin);
        prePassDepthRT->setTexture(r.depthStencilRT_MS.get());
        prePassDepthRT->setResolveTexture(r.depthStencilRT.get());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(prePassDesc.get());
        encoder->setRenderPipelineState(r.prePassPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.depthStencilState.get());

        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.materialDataBuffer.get(), 0, 1);
        encoder->setVertexBuffer(r.instanceDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setVertexBuffer(r.getBuffer(r.currentScene->vertexBuffer).get(), 0, 3);

        for (const auto& [material, meshes] : r.instanceBatches) {
            encoder->setFragmentTexture(
                r.getTexture(material->albedoMap ? material->albedoMap->texture : r.defaultAlbedoTexture).get(),
                0
            );

            for (const auto& mesh : meshes) {
                if (!r.currentCamera->isVisible(mesh->getWorldBoundingSphere())) {
                    continue;
                }

                encoder->setVertexBytes(&mesh->instanceID, sizeof(Uint32), 4);
                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    mesh->indexCount,
                    MTL::IndexTypeUInt32,
                    r.getBuffer(r.currentScene->indexBuffer).get(),
                    mesh->indexOffset * sizeof(Uint32)
                );
                r.drawCount++;
            }
        }

        encoder->endEncoding();
    }
};

// TLAS build pass: Builds top-level acceleration structure for ray tracing
class TLASBuildPass : public RenderPass {
public:
    explicit TLASBuildPass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "TLASBuildPass"; }

    void execute() override {
        auto& r = *renderer;

        // Build BLAS array if geometry is dirty
        if (r.currentScene->isGeometryDirty) {
            std::vector<NS::Object*> BLASObjects;
            BLASObjects.reserve(r.BLASs.size());
            for (auto blas : r.BLASs) {
                BLASObjects.push_back(static_cast<NS::Object*>(blas.get()));
            }
            r.BLASArray = NS::TransferPtr(NS::Array::array(BLASObjects.data(), BLASObjects.size()));
            r.currentScene->isGeometryDirty = false;
        }

        // Create TLAS descriptor
        auto TLASDesc = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
        TLASDesc->setInstanceCount(r.accelInstances.size());
        TLASDesc->setInstancedAccelerationStructures(r.BLASArray.get());
        TLASDesc->setInstanceDescriptorBuffer(r.accelInstanceBuffers[r.currentFrameInFlight].get());

        auto TLASSizes = r.device->accelerationStructureSizes(TLASDesc.get());
        if (!r.TLASScratchBuffers[r.currentFrameInFlight] ||
            r.TLASScratchBuffers[r.currentFrameInFlight]->length() < TLASSizes.buildScratchBufferSize) {
            r.TLASScratchBuffers[r.currentFrameInFlight] =
                NS::TransferPtr(r.device->newBuffer(TLASSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));
        }
        if (!r.TLASBuffers[r.currentFrameInFlight] ||
            r.TLASBuffers[r.currentFrameInFlight]->size() < TLASSizes.accelerationStructureSize) {
            r.TLASBuffers[r.currentFrameInFlight] =
                NS::TransferPtr(r.device->newAccelerationStructure(TLASSizes.accelerationStructureSize));
        }

        // Build TLAS
        auto accelEncoder = r.currentCommandBuffer->accelerationStructureCommandEncoder();
        accelEncoder->buildAccelerationStructure(
            r.TLASBuffers[r.currentFrameInFlight].get(),
            TLASDesc.get(),
            r.TLASScratchBuffers[r.currentFrameInFlight].get(),
            0
        );
        accelEncoder->endEncoding();
    }
};

// Normal resolve pass: Resolves MSAA normal texture
class NormalResolvePass : public RenderPass {
public:
    explicit NormalResolvePass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "NormalResolvePass"; }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.normalResolvePipeline.get());
        encoder->setTexture(r.normalRT_MS.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setBytes(&MSAA_SAMPLE_COUNT, sizeof(Uint32), 0);
        encoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }
};

// Tile culling pass: Performs light culling for tiled rendering
class TileCullingPass : public RenderPass {
public:
    explicit TileCullingPass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "TileCullingPass"; }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        glm::uvec3 gridSize = glm::uvec3(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
        uint pointLightCount = r.currentScene->pointLights.size();

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.tileCullingPipeline.get());
        encoder->setBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 1);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setBytes(&pointLightCount, sizeof(uint), 3);
        encoder->setBytes(&gridSize, sizeof(glm::uvec3), 4);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 5);
        encoder->dispatchThreadgroups(MTL::Size(clusterGridSizeX, clusterGridSizeY, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }
};

// Raytrace shadow pass: Computes ray-traced shadows
class RaytraceShadowPass : public RenderPass {
public:
    explicit RaytraceShadowPass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "RaytraceShadowPass"; }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.raytraceShadowPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.shadowRT.get(), 2);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.directionalLightBuffer.get(), 0, 1);
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 2);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 3);
        encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 4);
        encoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();

        // Generate mipmaps for shadow texture
        auto mipmapEncoder = NS::TransferPtr(r.currentCommandBuffer->blitCommandEncoder());
        mipmapEncoder->generateMipmaps(r.shadowRT.get());
        mipmapEncoder->endEncoding();
    }
};

// Raytrace AO pass: Computes ray-traced ambient occlusion
class RaytraceAOPass : public RenderPass {
public:
    explicit RaytraceAOPass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "RaytraceAOPass"; }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        auto encoder = r.currentCommandBuffer->computeCommandEncoder();
        encoder->setComputePipelineState(r.raytraceAOPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.aoRT.get(), 2);
        encoder->setBuffer(r.frameDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 2);
        encoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }
};

// Main render pass: Renders the scene with PBR lighting
class MainRenderPass : public RenderPass {
public:
    explicit MainRenderPass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "MainRenderPass"; }

    void execute() override {
        auto& r = *renderer;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        glm::uvec3 gridSize = glm::uvec3(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
        auto time = (float)SDL_GetTicks() / 1000.0f;

        // Create render pass descriptor
        auto renderPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto renderPassColorRT = renderPassDesc->colorAttachments()->object(0);
        renderPassColorRT->setClearColor(MTL::ClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a));
        renderPassColorRT->setLoadAction(MTL::LoadActionClear);
        renderPassColorRT->setStoreAction(MTL::StoreActionMultisampleResolve);
        renderPassColorRT->setTexture(r.colorRT_MS.get());
        renderPassColorRT->setResolveTexture(r.colorRT.get());

        auto renderPassDepthRT = renderPassDesc->depthAttachment();
        renderPassDepthRT->setLoadAction(MTL::LoadActionLoad);
        renderPassDepthRT->setTexture(r.depthStencilRT_MS.get());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(renderPassDesc.get());
        encoder->setRenderPipelineState(r.drawPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.depthStencilState.get());

        r.currentInstanceCount = 0;
        r.culledInstanceCount = 0;

        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.materialDataBuffer.get(), 0, 1);
        encoder->setVertexBuffer(r.instanceDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setVertexBuffer(r.getBuffer(r.currentScene->vertexBuffer).get(), 0, 3);
        encoder->setFragmentBuffer(r.directionalLightBuffer.get(), 0, 0);
        encoder->setFragmentBuffer(r.pointLightBuffer.get(), 0, 1);
        encoder->setFragmentBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 3);
        encoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 4);
        encoder->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
        encoder->setFragmentBytes(&time, sizeof(float), 6);

        for (const auto& [material, meshes] : r.instanceBatches) {
            encoder->setFragmentTexture(
                r.getTexture(material->albedoMap ? material->albedoMap->texture : r.defaultAlbedoTexture).get(), 0
            );
            encoder->setFragmentTexture(
                r.getTexture(material->normalMap ? material->normalMap->texture : r.defaultNormalTexture).get(), 1
            );
            encoder->setFragmentTexture(
                r.getTexture(material->metallicMap ? material->metallicMap->texture : r.defaultORMTexture).get(), 2
            );
            encoder->setFragmentTexture(
                r.getTexture(material->roughnessMap ? material->roughnessMap->texture : r.defaultORMTexture).get(), 3
            );
            encoder->setFragmentTexture(
                r.getTexture(material->occlusionMap ? material->occlusionMap->texture : r.defaultORMTexture).get(), 4
            );
            encoder->setFragmentTexture(
                r.getTexture(material->emissiveMap ? material->emissiveMap->texture : r.defaultEmissiveTexture).get(), 5
            );
            encoder->setFragmentTexture(r.shadowRT.get(), 7);

            for (const auto& mesh : meshes) {
                if (!r.currentCamera->isVisible(mesh->getWorldBoundingSphere())) {
                    r.culledInstanceCount++;
                    continue;
                }

                r.currentInstanceCount++;
                encoder->setVertexBytes(&mesh->instanceID, sizeof(Uint32), 4);
                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    mesh->indexCount,
                    MTL::IndexTypeUInt32,
                    r.getBuffer(r.currentScene->indexBuffer).get(),
                    mesh->indexOffset * sizeof(Uint32)
                );
                r.drawCount++;
            }
        }

        encoder->endEncoding();
    }
};

// Post-process pass: Applies tone mapping and other post-processing effects
class PostProcessPass : public RenderPass {
public:
    explicit PostProcessPass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "PostProcessPass"; }

    void execute() override {
        auto& r = *renderer;

        // Create render pass descriptor
        auto postPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto postPassColorRT = postPassDesc->colorAttachments()->object(0);
        postPassColorRT->setClearColor(MTL::ClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a));
        postPassColorRT->setLoadAction(MTL::LoadActionClear);
        postPassColorRT->setStoreAction(MTL::StoreActionStore);
        postPassColorRT->setTexture(r.currentDrawable->texture());

        // Execute the pass
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(postPassDesc.get());
        encoder->setRenderPipelineState(r.postProcessPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.colorRT.get(), 0);
        encoder->setFragmentTexture(r.aoRT.get(), 1);
        encoder->setFragmentTexture(r.normalRT.get(), 2);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// ImGui pass: Renders the ImGui UI overlay
class ImGuiPass : public RenderPass {
public:
    explicit ImGuiPass(Renderer_Metal* renderer) : RenderPass(renderer) {}

    const char* getName() const override { return "ImGuiPass"; }

    void execute() override {
        auto& r = *renderer;

        // UI building is done in draw() before this pass
        // This pass just renders the ImGui draw data
        ImGui::Render();

        // Create render pass descriptor
        auto imguiPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto imguiPassColorRT = imguiPassDesc->colorAttachments()->object(0);
        imguiPassColorRT->setLoadAction(MTL::LoadActionLoad);
        imguiPassColorRT->setStoreAction(MTL::StoreActionStore);
        imguiPassColorRT->setTexture(r.currentDrawable->texture());

        auto encoder = r.currentCommandBuffer->renderCommandEncoder(imguiPassDesc.get());
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), r.currentCommandBuffer, encoder);
        encoder->endEncoding();
    }
};


std::unique_ptr<Renderer> createRendererMetal() {
    return std::make_unique<Renderer_Metal>();
}

Renderer_Metal::Renderer_Metal() {

}

Renderer_Metal::~Renderer_Metal() {
    deinit();
}

auto Renderer_Metal::init(SDL_Window* window) -> void {
    renderer = SDL_CreateRenderer(window, nullptr);
    swapchain = (CA::MetalLayer*)SDL_GetRenderMetalLayer(renderer);
    // swapchain->setDisplaySyncEnabled(true);
    swapchain->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
    swapchain->setColorspace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    device = swapchain->device();
    queue = NS::TransferPtr(device->newCommandQueue());

    // ImGui init
    ImGui_ImplSDL3_InitForMetal(window);
    ImGui_ImplMetal_Init(device);

    isInitialized = true;

    createResources();

    // Initialize render graph with all passes
    graph.addPass(std::make_unique<TLASBuildPass>(this));
    graph.addPass(std::make_unique<PrePass>(this));
    graph.addPass(std::make_unique<NormalResolvePass>(this));
    graph.addPass(std::make_unique<TileCullingPass>(this));
    graph.addPass(std::make_unique<RaytraceShadowPass>(this));
    graph.addPass(std::make_unique<RaytraceAOPass>(this));
    graph.addPass(std::make_unique<MainRenderPass>(this));
    graph.addPass(std::make_unique<PostProcessPass>(this));
    graph.addPass(std::make_unique<ImGuiPass>(this));
}

auto Renderer_Metal::deinit() -> void {
    if (!isInitialized) {
        return;
    }

    // ImGui deinit
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplSDL3_Shutdown();

    SDL_DestroyRenderer(renderer);

    isInitialized = false;
}

auto Renderer_Metal::createResources() -> void {
    // Create pipelines
    drawPipeline = createPipeline("assets/shaders/3d_pbr_normal_mapped.metal", true, false, MSAA_SAMPLE_COUNT);
    prePassPipeline = createPipeline("assets/shaders/3d_depth_only.metal", true, false, MSAA_SAMPLE_COUNT);
    postProcessPipeline = createPipeline("assets/shaders/3d_post_process.metal", false, true, 1);
    buildClustersPipeline = createComputePipeline("assets/shaders/3d_cluster_build.metal");
    cullLightsPipeline = createComputePipeline("assets/shaders/3d_light_cull.metal");
    tileCullingPipeline = createComputePipeline("assets/shaders/3d_tile_light_cull.metal");
    normalResolvePipeline = createComputePipeline("assets/shaders/3d_normal_resolve.metal");
    raytraceShadowPipeline = createComputePipeline("assets/shaders/3d_raytrace_shadow.metal");
    raytraceAOPipeline = createComputePipeline("assets/shaders/3d_ssao.metal");

    // Create buffers
    frameDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& frameDataBuffer : frameDataBuffers) {
        frameDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(FrameData), MTL::ResourceStorageModeManaged));
    }
    cameraDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& cameraDataBuffer : cameraDataBuffers) {
        cameraDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(CameraData), MTL::ResourceStorageModeManaged));
    }
    instanceDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& instanceDataBuffer : instanceDataBuffers) {
        instanceDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(InstanceData) * MAX_INSTANCES, MTL::ResourceStorageModeManaged));
    }

    std::vector<Particle> particles{1000};
    testStorageBuffer = NS::TransferPtr(device->newBuffer(particles.size() * sizeof(Particle), MTL::ResourceStorageModeManaged));
    memcpy(testStorageBuffer->contents(), particles.data(), particles.size() * sizeof(Particle));
    testStorageBuffer->didModifyRange(NS::Range::Make(0, testStorageBuffer->length()));

    clusterBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& clusterBuffer : clusterBuffers) {
        clusterBuffer = NS::TransferPtr(device->newBuffer(clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ * sizeof(Cluster), MTL::ResourceStorageModeManaged));
    }

    accelInstanceBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    TLASScratchBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    TLASBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        accelInstanceBuffers[i] = NS::TransferPtr(device->newBuffer(
            MAX_INSTANCES * sizeof(MTL::AccelerationStructureInstanceDescriptor),
            MTL::ResourceStorageModeManaged
        ));
        TLASScratchBuffers[i] = nullptr;
        TLASBuffers[i] = nullptr;
    }

    // Create textures
    defaultAlbedoTexture = createTexture(AssetManager::loadImage("assets/textures/default_albedo.png")); // createTexture(AssetManager::loadImage("assets/textures/viking_room.png"));
    defaultNormalTexture = createTexture(AssetManager::loadImage("assets/textures/default_norm.png"));
    defaultORMTexture = createTexture(AssetManager::loadImage("assets/textures/default_orm.png"));
    defaultEmissiveTexture = createTexture(AssetManager::loadImage("assets/textures/default_emissive.png"));

    MTL::TextureDescriptor* depthStencilTextureDesc = MTL::TextureDescriptor::alloc()->init();
    depthStencilTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    depthStencilTextureDesc->setPixelFormat(MTL::PixelFormatDepth32Float);
    depthStencilTextureDesc->setWidth(swapchain->drawableSize().width);
    depthStencilTextureDesc->setHeight(swapchain->drawableSize().height);
    depthStencilTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    depthStencilTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    depthStencilRT_MS = NS::TransferPtr(device->newTexture(depthStencilTextureDesc));
    depthStencilTextureDesc->setTextureType(MTL::TextureType2D);
    depthStencilTextureDesc->setSampleCount(1);
    depthStencilTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    depthStencilRT = NS::TransferPtr(device->newTexture(depthStencilTextureDesc));
    depthStencilTextureDesc->release();

    MTL::TextureDescriptor* colorTextureDesc = MTL::TextureDescriptor::alloc()->init();
    colorTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    colorTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float); // HDR format
    colorTextureDesc->setWidth(swapchain->drawableSize().width);
    colorTextureDesc->setHeight(swapchain->drawableSize().height);
    colorTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    colorTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    colorRT_MS = NS::TransferPtr(device->newTexture(colorTextureDesc));
    colorTextureDesc->setTextureType(MTL::TextureType2D);
    colorTextureDesc->setSampleCount(1);
    colorTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    colorRT = NS::TransferPtr(device->newTexture(colorTextureDesc));
    colorTextureDesc->release();

    MTL::TextureDescriptor* normalTextureDesc = MTL::TextureDescriptor::alloc()->init();
    normalTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    normalTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float); // HDR format
    normalTextureDesc->setWidth(swapchain->drawableSize().width);
    normalTextureDesc->setHeight(swapchain->drawableSize().height);
    normalTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    normalTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    normalRT_MS = NS::TransferPtr(device->newTexture(normalTextureDesc));
    normalTextureDesc->setTextureType(MTL::TextureType2D);
    normalTextureDesc->setSampleCount(1);
    normalTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    normalRT = NS::TransferPtr(device->newTexture(normalTextureDesc));
    normalTextureDesc->release();

    MTL::TextureDescriptor* shadowTextureDesc = MTL::TextureDescriptor::alloc()->init();
    shadowTextureDesc->setTextureType(MTL::TextureType2D);
    shadowTextureDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    shadowTextureDesc->setWidth(swapchain->drawableSize().width);
    shadowTextureDesc->setHeight(swapchain->drawableSize().height);
    shadowTextureDesc->setMipmapLevelCount(calculateMipmapLevelCount(swapchain->drawableSize().width, swapchain->drawableSize().height));
    shadowTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    shadowRT = NS::TransferPtr(device->newTexture(shadowTextureDesc));
    shadowTextureDesc->release();

    MTL::TextureDescriptor* aoTextureDesc = MTL::TextureDescriptor::alloc()->init();
    aoTextureDesc->setTextureType(MTL::TextureType2D);
    aoTextureDesc->setPixelFormat(MTL::PixelFormatR16Float);
    aoTextureDesc->setWidth(swapchain->drawableSize().width);
    aoTextureDesc->setHeight(swapchain->drawableSize().height);
    aoTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    aoRT = NS::TransferPtr(device->newTexture(aoTextureDesc));
    aoTextureDesc->release();

    // Create depth stencil states (for depth testing)
    MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
    depthStencilDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
    depthStencilDesc->setDepthWriteEnabled(true);
    depthStencilState = NS::TransferPtr(device->newDepthStencilState(depthStencilDesc));
    depthStencilDesc->release();
}

auto Renderer_Metal::stage(std::shared_ptr<Scene> scene) -> void {
    // Lights
    directionalLightBuffer = NS::TransferPtr(device->newBuffer(scene->directionalLights.size() * sizeof(DirectionalLight), MTL::ResourceStorageModeManaged));
    memcpy(directionalLightBuffer->contents(), scene->directionalLights.data(), scene->directionalLights.size() * sizeof(DirectionalLight));
    directionalLightBuffer->didModifyRange(NS::Range::Make(0, directionalLightBuffer->length()));

    pointLightBuffer = NS::TransferPtr(device->newBuffer(scene->pointLights.size() * sizeof(PointLight), MTL::ResourceStorageModeManaged));
    memcpy(pointLightBuffer->contents(), scene->pointLights.data(), scene->pointLights.size() * sizeof(PointLight));
    pointLightBuffer->didModifyRange(NS::Range::Make(0, pointLightBuffer->length()));

    // Textures
    for (auto& img : scene->images) {
        img->texture = createTexture(img);
    }

    // Pipelines & materials
    if (scene->materials.empty()) {
        // TODO: create default material
    }
    for (auto& mat : scene->materials) {
        // pipelines[mat->pipeline] = createPipeline();
        materialIDs[mat] = nextMaterialID++;
    }
    materialDataBuffer = NS::TransferPtr(device->newBuffer(scene->materials.size() * sizeof(MaterialData), MTL::ResourceStorageModeManaged));

    // Buffers
    scene->vertexBuffer = createVertexBuffer(scene->vertices);
    scene->indexBuffer = createIndexBuffer(scene->indices);

    auto cmd = queue->commandBuffer();

    const std::function<void(const std::shared_ptr<Node>&)> stageNode =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                for (auto& mesh : node->meshGroup->meshes) {
                    // mesh->vbos.push_back(createVertexBuffer(mesh->vertices));
                    // mesh->ebo = createIndexBuffer(mesh->indices);

                    auto geomDesc = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
                    geomDesc->setVertexBuffer(getBuffer(scene->vertexBuffer).get());
                    geomDesc->setVertexStride(sizeof(VertexData));
                    geomDesc->setVertexFormat(MTL::AttributeFormatFloat3);
                    geomDesc->setVertexBufferOffset(mesh->vertexOffset * sizeof(VertexData) + offsetof(VertexData, position));
                    geomDesc->setIndexBuffer(getBuffer(scene->indexBuffer).get());
                    geomDesc->setIndexType(MTL::IndexTypeUInt32);
                    geomDesc->setIndexBufferOffset(mesh->indexOffset * sizeof(Uint32));
                    geomDesc->setTriangleCount(mesh->indexCount / 3);
                    geomDesc->setOpaque(true);

                    auto accelDesc = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
                    NS::Object* descriptors[] = { geomDesc.get() };
                    auto geomArray = NS::TransferPtr(NS::Array::array(descriptors, 1));
                    accelDesc->setGeometryDescriptors(geomArray.get());

                    auto accelSizes = device->accelerationStructureSizes(accelDesc.get());
                    auto accelStruct = NS::TransferPtr(device->newAccelerationStructure(accelSizes.accelerationStructureSize));
                    auto scratchBuffer = NS::TransferPtr(device->newBuffer(accelSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));

                    auto encoder = cmd->accelerationStructureCommandEncoder();
                    encoder->buildAccelerationStructure(accelStruct.get(), accelDesc.get(), scratchBuffer.get(), 0);
                    encoder->endEncoding();

                    BLASs.push_back(accelStruct);

                    mesh->materialID = materialIDs[mesh->material];
                    mesh->instanceID = nextInstanceID++;
                }
            }
            for (const auto& child : node->children) {
                stageNode(child);
            }
        };
    for (auto& node : scene->nodes) {
        stageNode(node);
    }

    cmd->commit();
}

auto Renderer_Metal::draw(std::shared_ptr<Scene> scene, Camera& camera) -> void {
    // Get drawable
    auto surface = swapchain->nextDrawable();
    if (!surface) {
        return;
    }

    // ==========================================================================
    // Prepare frame data
    // ==========================================================================
    auto time = (float)SDL_GetTicks() / 1000.0f;

    FrameData* frameData = reinterpret_cast<FrameData*>(frameDataBuffers[currentFrameInFlight]->contents());
    frameData->frameNumber = frameNumber;
    frameData->time = time;
    frameData->deltaTime = 0.016f; // TODO:
    frameDataBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, frameDataBuffers[currentFrameInFlight]->length()));

    float near = camera.near();
    float far = camera.far();
    glm::vec3 camPos = camera.getEye();
    glm::mat4 proj = camera.getProjMatrix();
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 invProj = glm::inverse(proj);
    glm::mat4 invView = glm::inverse(view);
    CameraData* cameraData = reinterpret_cast<CameraData*>(cameraDataBuffers[currentFrameInFlight]->contents());
    cameraData->proj = proj;
    cameraData->view = view;
    cameraData->invProj = invProj;
    cameraData->invView = invView;
    cameraData->near = near;
    cameraData->far = far;
    cameraData->position = camPos;
    cameraDataBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, cameraDataBuffers[currentFrameInFlight]->length()));

    DirectionalLight* dirLights = reinterpret_cast<DirectionalLight*>(directionalLightBuffer->contents());
    for (size_t i = 0; i < scene->directionalLights.size(); ++i) {
        dirLights[i].direction = scene->directionalLights[i].direction;
        dirLights[i].color = scene->directionalLights[i].color;
        dirLights[i].intensity = scene->directionalLights[i].intensity;
    }
    pointLightBuffer->didModifyRange(NS::Range::Make(0, directionalLightBuffer->length()));

    PointLight* pointLights = reinterpret_cast<PointLight*>(pointLightBuffer->contents());
    for (size_t i = 0; i < scene->pointLights.size(); ++i) {
        pointLights[i].position = scene->pointLights[i].position;
        pointLights[i].color = scene->pointLights[i].color;
        pointLights[i].intensity = scene->pointLights[i].intensity;
        pointLights[i].radius = scene->pointLights[i].radius;
    }
    pointLightBuffer->didModifyRange(NS::Range::Make(0, pointLightBuffer->length()));

    MaterialData* materialData = reinterpret_cast<MaterialData*>(materialDataBuffer->contents());
    for (size_t i = 0; i < scene->materials.size(); ++i) {
        const auto& mat = scene->materials[i];
        materialData[i] = MaterialData {
            .baseColorFactor = mat->baseColorFactor,
            .normalScale = mat->normalScale,
            .metallicFactor = mat->metallicFactor,
            .roughnessFactor = mat->roughnessFactor,
            .occlusionStrength = mat->occlusionStrength,
            .emissiveFactor = mat->emissiveFactor,
            .emissiveStrength = mat->emissiveStrength,
            .subsurface = mat->subsurface,
            .specular = mat->specular,
            .specularTint = mat->specularTint,
            .anisotropic = mat->anisotropic,
            .sheen = mat->sheen,
            .sheenTint = mat->sheenTint,
            .clearcoat = mat->clearcoat,
            .clearcoatGloss = mat->clearcoatGloss,
        };
    }
    materialDataBuffer->didModifyRange(NS::Range::Make(0, materialDataBuffer->length()));

    // Update instance data
    instances.clear();
    accelInstances.clear();
    instanceBatches.clear();
    const std::function<void(const std::shared_ptr<Node>&)> updateNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            const glm::mat4& transform = node->worldTransform;
            for (const auto& mesh : node->meshGroup->meshes) {
                instances.push_back({
                    .model = transform,
                    .color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
                    .vertexOffset = mesh->vertexOffset,
                    .indexOffset = mesh->indexOffset,
                    .vertexCount = mesh->vertexCount,
                    .indexCount = mesh->indexCount,
                    .materialID = mesh->materialID,
                    .primitiveMode = mesh->primitiveMode,
                    .AABBMin = mesh->worldAABBMin,
                    .AABBMax = mesh->worldAABBMax,
                });
                MTL::AccelerationStructureInstanceDescriptor accelInstanceDesc;
                for (int i = 0; i < 4; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        accelInstanceDesc.transformationMatrix.columns[i][j] = transform[i][j];
                    }
                }
                accelInstanceDesc.accelerationStructureIndex = mesh->instanceID;
                accelInstanceDesc.mask = 0xFF;
                accelInstances.push_back(accelInstanceDesc);
                if (!mesh->material) {
                    fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                    continue;
                }
                instanceBatches[mesh->material].push_back(mesh);
            }
        }
        for (const auto& child : node->children) {
            updateNode(child);
        }
    };
    for (const auto& node : scene->nodes) {
        updateNode(node);
    }
    if (instances.size() > MAX_INSTANCES) {
        fmt::print("Warning: Instance count ({}) exceeds MAX_INSTANCES ({})\n", instances.size(), MAX_INSTANCES);
    }
    memcpy(instanceDataBuffers[currentFrameInFlight]->contents(), instances.data(), instances.size() * sizeof(InstanceData));
    instanceDataBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, instanceDataBuffers[currentFrameInFlight]->length()));
    memcpy(accelInstanceBuffers[currentFrameInFlight]->contents(), accelInstances.data(), accelInstances.size() * sizeof(MTL::AccelerationStructureInstanceDescriptor));
    accelInstanceBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, accelInstanceBuffers[currentFrameInFlight]->length()));

    // ==========================================================================
    // Set up rendering context for passes
    // ==========================================================================
    auto cmd = queue->commandBuffer();
    currentCommandBuffer = cmd;
    currentScene = scene;
    currentCamera = &camera;
    currentDrawable = surface;
    drawCount = 0;

    // ==========================================================================
    // Build ImGui UI (before ImGuiPass executes)
    // ==========================================================================
    // Create temporary render pass descriptor for ImGui initialization
    auto imguiPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto imguiPassColorRT = imguiPassDesc->colorAttachments()->object(0);
    imguiPassColorRT->setTexture(surface->texture());

    ImGui_ImplMetal_NewFrame(imguiPassDesc.get());
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // ImGui::DockSpaceOverViewport();

    if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Average frame rate: %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGui::ColorEdit3("Clear color", (float*)&clearColor);

        ImGui::Separator();

        if (ImGui::TreeNode("RTs")) {
            ImGui::Separator();
            if (ImGui::TreeNode(fmt::format("Scene Color RT").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)colorRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Scene Depth RT").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)depthStencilRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Raytraced Shadow").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)shadowRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Raytraced AO").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)aoRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode(fmt::format("Scene Normal RT").c_str())) {
                ImGui::Image((ImTextureID)(intptr_t)normalRT.get(), ImVec2(64, 64));
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Materials")) {
            ImGui::Separator();
            for (auto m : scene->materials) {
                if (!m) {
                    continue;
                }
                ImGui::PushID(m.get());
                if (ImGui::TreeNode(fmt:: format("Mat #{}", m->name).c_str())) {
                    if (m->albedoMap) {
                        ImGui::Text("Albedo Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->albedoMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->normalMap) {
                        ImGui::Text("Normal Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->normalMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->metallicMap) {
                        ImGui::Text("Metallic Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->metallicMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->roughnessMap) {
                        ImGui::Text("Roughness Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->roughnessMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->occlusionMap) {
                        ImGui::Text("Occlusion Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->occlusionMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->emissiveMap) {
                        ImGui::Text("Emissive Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->emissiveMap->texture).get(), ImVec2(64, 64));
                    }
                    ImGui::ColorEdit4("Base Color Factor", (float*)&m->baseColorFactor);
                    ImGui::DragFloat("Normal Scale", &m->normalScale, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Roughness Factor", &m->roughnessFactor, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Metallic Factor", &m->metallicFactor, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Occlusion Strength", &m->occlusionStrength, .05f, 0.0f, 5.0f);
                    ImGui::ColorEdit3("Emissive Color Factor", (float*)&m->emissiveFactor);
                    ImGui::DragFloat("Emissive Strength", &m->emissiveStrength, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Subsurface", &m->subsurface, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Specular", &m->specular, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Specular Tint", &m->specularTint, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Anisotropic", &m->anisotropic, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Sheen", &m->sheen, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Sheen Tint", &m->sheenTint, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Clearcoat", &m->clearcoat, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Clearcoat Gloss", &m->clearcoatGloss, .01f, 0.0f, 1.0f);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Lights")) {
            ImGui::Separator();
            for (auto& l : scene->directionalLights) {
                ImGui::Text("Directional Light");
                ImGui::PushID(&l);
                ImGui::DragFloat3("Direction", (float*)&l.direction, 0.1f);
                ImGui::ColorEdit3("Color", (float*)&l.color);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f);
                ImGui::PopID();
            }
            for (auto& l : scene->pointLights) {
                ImGui::Text("Point Light");
                ImGui::PushID(&l);
                ImGui::DragFloat3("Position", (float*)&l.position, 0.1f);
                ImGui::ColorEdit3("Color", (float*)&l.color);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f);
                ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.0001f);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Geometry")) {
            ImGui::Separator();
            ImGui::Text("Total vertices: %zu", scene->vertices.size());
            ImGui::Text("Total indices: %zu", scene->indices.size());
            const std::function<void(const std::shared_ptr<Node>&)> showNode = [&](const std::shared_ptr<Node>& node) {
                ImGui::PushID(node.get());
                ImGui::Text("Node #%s", node->name.c_str());
                glm::vec3 pos = node->getLocalPosition();
                glm::vec3 euler = node->getLocalEulerAngles();
                glm::vec3 scale = node->getLocalScale();
                if (ImGui::DragFloat3("Position", &pos.x, 0.1f))
                    node->setLocalPosition(pos);
                if (ImGui::DragFloat3("Rotation", &euler.x, 1.0f))
                    node->setLocalEulerAngles(euler);
                if (ImGui::DragFloat3("Scale", &scale.x, 0.1f, 0.0001f))
                    node->setLocalScale(scale);
                if (node->meshGroup) {
                    for (const auto& mesh : node->meshGroup->meshes) {
                        ImGui::PushID(mesh.get());
                        if (ImGui::TreeNode(fmt:: format("Mesh").c_str())) {
                            ImGui::Text("Vertex count: %u", mesh->vertexCount);
                            ImGui::Text("Vertex offset: %u", mesh->vertexOffset);
                            ImGui::Text("Index count: %u", mesh->indexCount);
                            ImGui::Text("Index offset: %u", mesh->indexOffset);
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::PopID();
                for (const auto& child : node->children) {
                    showNode(child);
                }
            };
            for (const auto& node : scene->nodes) {
                showNode(node);
            }
            ImGui::TreePop();
        }
    }

    // ==========================================================================
    // Execute all render passes
    // ==========================================================================
    graph.execute();

    // ==========================================================================
    // Present and cleanup
    // ==========================================================================
    cmd->presentDrawable(surface);
    cmd->commit();

    surface->release();

    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
    frameNumber++;
}


NS::SharedPtr<MTL::RenderPipelineState> Renderer_Metal::createPipeline(const std::string& filename, bool isHDR, bool isColorOnly, Uint32 sampleCount) {
    auto shaderSrc = readFile(filename);

    auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* error = nullptr;
    MTL::CompileOptions* options = nullptr;
    MTL::Library* library = device->newLibrary(code, options, &error);
    if (!library) {
        throw std::runtime_error(fmt::format("Could not compile shader! Error: {}\n", error->localizedDescription()->utf8String()));
    }
    // fmt::print("Shader compiled successfully. Shader: {}\n", code->cString(NS::StringEncoding::UTF8StringEncoding));

    auto vertexFuncName = NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding);
    auto vertexMain = library->newFunction(vertexFuncName);

    auto fragmentFuncName = NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding);
    auto fragmentMain = library->newFunction(fragmentFuncName);

    auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    pipelineDesc->setVertexFunction(vertexMain);
    pipelineDesc->setFragmentFunction(fragmentMain);

    // auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

    // auto layout = vertexDesc->layouts()->object(0);
    // layout->setStride(sizeof(VertexData));
    // layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
    // layout->setStepRate(1);

    // auto attributes = vertexDesc->attributes();

    // auto posAttr = attributes->object(0);
    // posAttr->setFormat(MTL::VertexFormatFloat3);
    // posAttr->setOffset(offsetof(VertexData, position));
    // posAttr->setBufferIndex(2);

    // auto uvAttr = attributes->object(1);
    // uvAttr->setFormat(MTL::VertexFormatFloat2);
    // uvAttr->setOffset(offsetof(VertexData, uv));
    // uvAttr->setBufferIndex(2);

    // auto normalAttr = attributes->object(2);
    // normalAttr->setFormat(MTL::VertexFormatFloat3);
    // normalAttr->setOffset(offsetof(VertexData, normal));
    // normalAttr->setBufferIndex(2);

    // auto tangentAttr = attributes->object(3);
    // tangentAttr->setFormat(MTL::VertexFormatFloat4);
    // tangentAttr->setOffset(offsetof(VertexData, tangent));
    // tangentAttr->setBufferIndex(2);

    // pipelineDesc->setVertexDescriptor(vertexDesc);

    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
    if (isHDR) {
        colorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float); // HDR format
    } else {
        colorAttachment->setPixelFormat(swapchain->pixelFormat());
    }
    // colorAttachment->setBlendingEnabled(true);
    // colorAttachment->setAlphaBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setRgbBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    // colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    if (!isColorOnly) {
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    } else {
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
    }
    pipelineDesc->setSampleCount(static_cast<NS::UInteger>(sampleCount));

    auto pipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
    if (!pipeline) {
        std::string errorMsg = error ? error->localizedDescription()->utf8String() : "Unknown error";
        throw std::runtime_error(fmt::format("Could not create pipeline! Error: {}\nShader: {}\n", errorMsg, filename));
    }

    code->release();
    library->release();
    vertexMain->release();
    fragmentMain->release();
    // vertexDesc->release();
    pipelineDesc->release();

    return pipeline;
}

NS::SharedPtr<MTL::ComputePipelineState> Renderer_Metal::createComputePipeline(const std::string& filename) {
    auto shaderSrc = readFile(filename);

    auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* error = nullptr;
    MTL::CompileOptions* options = nullptr;
    MTL::Library* library = device->newLibrary(code, options, &error);
    if (!library) {
        throw std::runtime_error(fmt::format("Could not compile shader! Error: {}\n", error->localizedDescription()->utf8String()));
    }
    // fmt::print("Shader compiled successfully. Shader: {}\n", code->cString(NS::StringEncoding::UTF8StringEncoding));

    auto computeFuncName = NS::String::string("computeMain", NS::StringEncoding::UTF8StringEncoding);
    auto computeMain = library->newFunction(computeFuncName);

    auto pipeline = NS::TransferPtr(device->newComputePipelineState(computeMain, &error));

    code->release();
    library->release();
    computeMain->release();

    return pipeline;
}

TextureHandle Renderer_Metal::createTexture(const std::shared_ptr<Image>& img) {
    if (img) {
        MTL::PixelFormat pixelFormat = MTL::PixelFormat::PixelFormatRGBA8Unorm;
        switch (img->channelCount) {
        case 1:
            pixelFormat = MTL::PixelFormat::PixelFormatR8Unorm;
            break;
        case 3:
        case 4:
            pixelFormat = MTL::PixelFormat::PixelFormatRGBA8Unorm;
            break;
        default:
            throw std::runtime_error(fmt::format("Unknown texture format at {}\n", img->uri));
            break;
        }
        int numLevels = calculateMipmapLevelCount(img->width, img->height);

        auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        textureDesc->setPixelFormat(pixelFormat);
        textureDesc->setTextureType(MTL::TextureType::TextureType2D);
        textureDesc->setWidth(NS::UInteger(img->width));
        textureDesc->setHeight(NS::UInteger(img->height));
        textureDesc->setMipmapLevelCount(numLevels);
        textureDesc->setSampleCount(1);
        textureDesc->setStorageMode(MTL::StorageMode::StorageModeManaged);
        textureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

        auto texture = NS::TransferPtr(device->newTexture(textureDesc.get()));
        texture->replaceRegion(MTL::Region(0, 0, 0, img->width, img->height, 1), 0, img->byteArray.data(), img->width * img->channelCount);

        if (numLevels > 1) {
            auto cmdBlit = NS::TransferPtr(queue->commandBuffer());
            auto enc = NS::TransferPtr(cmdBlit->blitCommandEncoder());
            enc->generateMipmaps(texture.get());
            enc->endEncoding();
            cmdBlit->commit();
        }

        textures[nextTextureID] = texture;

        return TextureHandle { nextTextureID++ };
    } else {
        throw std::runtime_error(fmt::format("Failed to create texture at {}!\n", img->uri));
    }
}

BufferHandle Renderer_Metal::createVertexBuffer(const std::vector<VertexData>& vertices) {
    auto stagingBuffer = NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), vertices.data(), vertices.size() * sizeof(VertexData));

    auto buffer = NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, vertices.size() * sizeof(VertexData));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return BufferHandle { nextBufferID++ };
}

BufferHandle Renderer_Metal::createIndexBuffer(const std::vector<Uint32>& indices) {
    auto stagingBuffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), indices.data(), indices.size() * sizeof(Uint32));

    auto buffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, indices.size() * sizeof(Uint32));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return BufferHandle { nextBufferID++ };
}

NS::SharedPtr<MTL::Buffer> Renderer_Metal::getBuffer(BufferHandle handle) const {
    return buffers.at(handle.rid);
}

NS::SharedPtr<MTL::Texture> Renderer_Metal::getTexture(TextureHandle handle) const {
    return textures.at(handle.rid);
}

NS::SharedPtr<MTL::RenderPipelineState> Renderer_Metal::getPipeline(PipelineHandle handle) const {
    return pipelines.at(handle.rid);
}