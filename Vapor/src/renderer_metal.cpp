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
    int mipLevels = static_cast<int>(std::floor(std::log2(std::max(swapchain->drawableSize().width, swapchain->drawableSize().height))) + 1);
    shadowTextureDesc->setMipmapLevelCount(mipLevels);
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
    // Note: Image no longer has texture field - store mapping instead
    for (auto& img : scene->images) {
        imageToTextureMap[img] = createTexture(img);
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
    // Note: Scene no longer has vertices/indices - collect from meshes
    std::vector<VertexData> allVertices;
    std::vector<Uint32> allIndices;
    Uint32 currentVertexOffset = 0;
    Uint32 currentIndexOffset = 0;

    const std::function<void(const std::shared_ptr<Node>&)> collectGeometry = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            for (auto& mesh : node->meshGroup->meshes) {
                MeshGPUResources& resources = meshGPUResources[mesh];
                resources.vertexOffset = currentVertexOffset;
                resources.indexOffset = currentIndexOffset;
                resources.vertexCount = static_cast<Uint32>(mesh->vertices.size());
                resources.indexCount = static_cast<Uint32>(mesh->indices.size());

                allVertices.insert(allVertices.end(), mesh->vertices.begin(), mesh->vertices.end());
                for (Uint32 index : mesh->indices) {
                    allIndices.push_back(index + currentVertexOffset);
                }

                currentVertexOffset += resources.vertexCount;
                currentIndexOffset += resources.indexCount;
            }
        }
        for (const auto& child : node->children) {
            collectGeometry(child);
        }
    };
    for (const auto& node : scene->nodes) {
        collectGeometry(node);
    }

    sceneVertexBuffer = createVertexBuffer(allVertices);
    sceneIndexBuffer = createIndexBuffer(allIndices);

    auto cmd = queue->commandBuffer();

    const std::function<void(const std::shared_ptr<Node>&)> stageNode =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                for (auto& mesh : node->meshGroup->meshes) {
                    // mesh->vbos.push_back(createVertexBuffer(mesh->vertices));
                    // mesh->ebo = createIndexBuffer(mesh->indices);

                    auto geomDesc = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
                    const MeshGPUResources& resources = meshGPUResources[mesh];
                    geomDesc->setVertexBuffer(getBuffer(sceneVertexBuffer).get());
                    geomDesc->setVertexStride(sizeof(VertexData));
                    geomDesc->setVertexFormat(MTL::AttributeFormatFloat3);
                    geomDesc->setVertexBufferOffset(resources.vertexOffset * sizeof(VertexData) + offsetof(VertexData, position));
                    geomDesc->setIndexBuffer(getBuffer(sceneIndexBuffer).get());
                    geomDesc->setIndexType(MTL::IndexTypeUInt32);
                    geomDesc->setIndexBufferOffset(resources.indexOffset * sizeof(Uint32));
                    geomDesc->setTriangleCount(resources.indexCount / 3);
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

                    meshGPUResources[mesh].materialID = materialIDs[mesh->material];
                    meshGPUResources[mesh].instanceID = nextInstanceID++;
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
    auto surface = swapchain->nextDrawable();
    if (!surface) {
        return;
    }

    // Prepare data
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

    auto drawableSize = swapchain->drawableSize();
    glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
    glm::uvec3 gridSize = glm::uvec3(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
    uint pointLightCount = scene->pointLights.size();
    uint directionalLightCount = scene->directionalLights.size();

    instances.clear();
    accelInstances.clear();
    instanceBatches.clear();
    const std::function<void(const std::shared_ptr<Node>&)> updateNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            const glm::mat4& transform = node->worldTransform;
            for (const auto& mesh : node->meshGroup->meshes) {
                const MeshGPUResources& resources = meshGPUResources[mesh];
                instances.push_back({
                    .model = transform,
                    .color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
                    .vertexOffset = resources.vertexOffset,
                    .indexOffset = resources.indexOffset,
                    .vertexCount = resources.vertexCount,
                    .indexCount = resources.indexCount,
                    .materialID = resources.materialID,
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
                accelInstanceDesc.accelerationStructureIndex = resources.instanceID;
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
    if (instances.size() > MAX_INSTANCES) { // TODO: reallocate when needed
        fmt::print("Warning: Instance count ({}) exceeds MAX_INSTANCES ({})\n", instances.size(), MAX_INSTANCES);
    }
    // TODO: avoid updating the entire instance data buffer every frame
    memcpy(instanceDataBuffers[currentFrameInFlight]->contents(), instances.data(), instances.size() * sizeof(InstanceData));
    instanceDataBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, instanceDataBuffers[currentFrameInFlight]->length()));
    memcpy(accelInstanceBuffers[currentFrameInFlight]->contents(), accelInstances.data(), accelInstances.size() * sizeof(MTL::AccelerationStructureInstanceDescriptor));
    accelInstanceBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, accelInstanceBuffers[currentFrameInFlight]->length()));

    // Note: Scene no longer has isGeometryDirty - always rebuild BLAS array
    // TODO: Add dirty tracking if needed
    std::vector<NS::Object*> BLASObjects;
    BLASObjects.reserve(BLASs.size());
    for (auto blas : BLASs) {
        BLASObjects.push_back(static_cast<NS::Object*>(blas.get()));
    }
    BLASArray = NS::TransferPtr(NS::Array::array(BLASObjects.data(), BLASObjects.size()));

    auto TLASDesc = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
    TLASDesc->setInstanceCount(accelInstances.size());
    TLASDesc->setInstancedAccelerationStructures(BLASArray.get());
    TLASDesc->setInstanceDescriptorBuffer(accelInstanceBuffers[currentFrameInFlight].get());
    auto TLASSizes = device->accelerationStructureSizes(TLASDesc.get());
    if (!TLASScratchBuffers[currentFrameInFlight] || TLASScratchBuffers[currentFrameInFlight]->length() < TLASSizes.buildScratchBufferSize) {
        TLASScratchBuffers[currentFrameInFlight] = NS::TransferPtr(device->newBuffer(TLASSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));
    }
    if (!TLASBuffers[currentFrameInFlight] || TLASBuffers[currentFrameInFlight]->size() < TLASSizes.accelerationStructureSize) {
        TLASBuffers[currentFrameInFlight] = NS::TransferPtr(device->newAccelerationStructure(TLASSizes.accelerationStructureSize));
    }

    // Create render pass descriptors
    auto prePass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto prePassNormalRT = prePass->colorAttachments()->object(0);
    prePassNormalRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0)); // default no normal
    prePassNormalRT->setLoadAction(MTL::LoadActionClear);
    prePassNormalRT->setStoreAction(MTL::StoreActionStore);
    prePassNormalRT->setTexture(normalRT_MS.get());
    auto prePassDepthRT = prePass->depthAttachment();
    prePassDepthRT->setClearDepth(clearDepth);
    prePassDepthRT->setLoadAction(MTL::LoadActionClear);
    prePassDepthRT->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
    prePassDepthRT->setDepthResolveFilter(MTL::MultisampleDepthResolveFilter::MultisampleDepthResolveFilterMin);
    prePassDepthRT->setTexture(depthStencilRT_MS.get());
    prePassDepthRT->setResolveTexture(depthStencilRT.get());

    auto renderPass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto renderPassColorRT = renderPass->colorAttachments()->object(0);
    renderPassColorRT->setClearColor(MTL::ClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a));
    renderPassColorRT->setLoadAction(MTL::LoadActionClear);
    renderPassColorRT->setStoreAction(MTL::StoreActionMultisampleResolve);
    renderPassColorRT->setTexture(colorRT_MS.get());
    renderPassColorRT->setResolveTexture(colorRT.get());
    auto renderPassDepthRT = renderPass->depthAttachment();
    renderPassDepthRT->setLoadAction(MTL::LoadActionLoad);
    renderPassDepthRT->setTexture(depthStencilRT_MS.get());

    auto postPass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto postPassColorRT = postPass->colorAttachments()->object(0);
    postPassColorRT->setClearColor(MTL::ClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a));
    postPassColorRT->setLoadAction(MTL::LoadActionClear);
    postPassColorRT->setStoreAction(MTL::StoreActionStore);
    postPassColorRT->setTexture(surface->texture());

    auto imguiPass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto imguiPassColorRT = imguiPass->colorAttachments()->object(0);
    imguiPassColorRT->setLoadAction(MTL::LoadActionLoad);
    imguiPassColorRT->setStoreAction(MTL::StoreActionStore);
    imguiPassColorRT->setTexture(surface->texture());

    // Start rendering
    auto cmd = queue->commandBuffer();
    drawCount = 0;

    // 0. build TLAS
    // TODO: only build TLAS if it's dirty
    auto accelEncoder = cmd->accelerationStructureCommandEncoder();
    accelEncoder->buildAccelerationStructure(TLASBuffers[currentFrameInFlight].get(), TLASDesc.get(), TLASScratchBuffers[currentFrameInFlight].get(), 0);
    accelEncoder->endEncoding();

    // 1. pre-pass
    auto prePassEncoder = cmd->renderCommandEncoder(prePass.get());
    prePassEncoder->setRenderPipelineState(prePassPipeline.get());
    prePassEncoder->setCullMode(MTL::CullModeBack);
    prePassEncoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
    prePassEncoder->setDepthStencilState(depthStencilState.get());

    prePassEncoder->setVertexBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 0);
    prePassEncoder->setVertexBuffer(materialDataBuffer.get(), 0, 1);
    prePassEncoder->setVertexBuffer(instanceDataBuffers[currentFrameInFlight].get(), 0, 2);
    prePassEncoder->setVertexBuffer(getBuffer(sceneVertexBuffer).get(), 0, 3);
    for (const auto& [material, meshes] : instanceBatches) {
        TextureHandle albedoTexture = material->albedoMap ? imageToTextureMap[material->albedoMap] : defaultAlbedoTexture;
        prePassEncoder->setFragmentTexture(
            getTexture(albedoTexture).get(),
            0
        );
        // prePassEncoder->setFragmentTexture(
        //     getTexture(material->displacementMap ? material->displacementMap->texture : defaultDisplacementTexture).get(),
        //     1
        // );
        for (const auto& mesh : meshes) {
            if (!camera.isVisible(mesh->getWorldBoundingSphere())) {
                continue;
            }
            // prePassEncoder->setVertexBuffer(getBuffer(mesh->vbos[0]).get(), 0, 2);
            const MeshGPUResources& resources = meshGPUResources[mesh];
            prePassEncoder->setVertexBytes(&resources.instanceID, sizeof(Uint32), 4);
            prePassEncoder->drawIndexedPrimitives(
                MTL::PrimitiveType::PrimitiveTypeTriangle,
                resources.indexCount,// mesh->indices.size(),
                MTL::IndexTypeUInt32,
                getBuffer(sceneIndexBuffer).get(), // getBuffer(mesh->ebo).get(),
                resources.indexOffset * sizeof(Uint32) // 0
            );
            drawCount++;
        }
    }
    prePassEncoder->endEncoding();

    // 2. normal resolve pass
    auto normalResolveEncoder = cmd->computeCommandEncoder();
    normalResolveEncoder->setComputePipelineState(normalResolvePipeline.get());
    normalResolveEncoder->setTexture(normalRT_MS.get(), 0);
    normalResolveEncoder->setTexture(normalRT.get(), 1);
    normalResolveEncoder->setBytes(&MSAA_SAMPLE_COUNT, sizeof(Uint32), 0);
    normalResolveEncoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
    normalResolveEncoder->endEncoding();

    // // 2. cluster building pass
    // auto clusterEncoder = cmd->computeCommandEncoder();
    // clusterEncoder->setComputePipelineState(buildClustersPipeline.get());
    // // clusterEncoder->useResource(clusterBuffer.get(), MTL::ResourceUsageWrite);
    // clusterEncoder->setBuffer(clusterBuffers[currentFrameInFlight].get(), 0, 0);
    // clusterEncoder->setBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 1);
    // clusterEncoder->setBytes(&screenSize, sizeof(glm::vec2), 2);
    // clusterEncoder->setBytes(&gridSize, sizeof(glm::uvec3), 3);
    // clusterEncoder->dispatchThreadgroups(MTL::Size(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ), MTL::Size(1, 1, 1));
    // clusterEncoder->endEncoding();

    // // 3. light culling pass
    // auto cullingEncoder = cmd->computeCommandEncoder();
    // cullingEncoder->setComputePipelineState(cullLightsPipeline.get());
    // // cullingEncoder->useResource(clusterBuffer.get(), MTL::ResourceUsageWrite);
    // cullingEncoder->setBuffer(clusterBuffers[currentFrameInFlight].get(), 0, 0);
    // cullingEncoder->setBuffer(pointLightBuffer.get(), 0, 1);
    // cullingEncoder->setBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 2);
    // cullingEncoder->setBytes(&lightCount, sizeof(uint), 3);
    // cullingEncoder->setBytes(&gridSize, sizeof(glm::uvec3), 4);
    // cullingEncoder->dispatchThreadgroups(MTL::Size(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ), MTL::Size(1, 1, 1));
    // cullingEncoder->endEncoding();

    // 2 & 3. tile culling pass
    auto tileCullingEncoder = cmd->computeCommandEncoder();
    tileCullingEncoder->setComputePipelineState(tileCullingPipeline.get());
    // tileCullingEncoder->useResource(clusterBuffer.get(), MTL::ResourceUsageWrite);
    tileCullingEncoder->setBuffer(clusterBuffers[currentFrameInFlight].get(), 0, 0);
    tileCullingEncoder->setBuffer(pointLightBuffer.get(), 0, 1);
    tileCullingEncoder->setBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 2);
    tileCullingEncoder->setBytes(&pointLightCount, sizeof(uint), 3);
    tileCullingEncoder->setBytes(&gridSize, sizeof(glm::uvec3), 4);
    tileCullingEncoder->setBytes(&screenSize, sizeof(glm::vec2), 5);
    tileCullingEncoder->dispatchThreadgroups(MTL::Size(clusterGridSizeX, clusterGridSizeY, 1), MTL::Size(1, 1, 1));
    tileCullingEncoder->endEncoding();

    // 3. raytrace shadow pass
    auto raytraceShadowEncoder = cmd->computeCommandEncoder();
    raytraceShadowEncoder->setComputePipelineState(raytraceShadowPipeline.get());
    raytraceShadowEncoder->setTexture(depthStencilRT.get(), 0);
    raytraceShadowEncoder->setTexture(normalRT.get(), 1);
    raytraceShadowEncoder->setTexture(shadowRT.get(), 2);
    raytraceShadowEncoder->setBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 0);
    raytraceShadowEncoder->setBuffer(directionalLightBuffer.get(), 0, 1);
    raytraceShadowEncoder->setBuffer(pointLightBuffer.get(), 0, 2);
    raytraceShadowEncoder->setBytes(&screenSize, sizeof(glm::vec2), 3);
    raytraceShadowEncoder->setAccelerationStructure(TLASBuffers[currentFrameInFlight].get(), 4);
    raytraceShadowEncoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
    raytraceShadowEncoder->endEncoding();
    // TODO: not sure if this is needed
    auto mipmapEncoder = NS::TransferPtr(cmd->blitCommandEncoder());
    mipmapEncoder->generateMipmaps(shadowRT.get());
    mipmapEncoder->endEncoding();

    // 3. raytrace AO pass
    auto raytraceAOEncoder = cmd->computeCommandEncoder();
    raytraceAOEncoder->setComputePipelineState(raytraceAOPipeline.get());
    raytraceAOEncoder->setTexture(depthStencilRT.get(), 0);
    raytraceAOEncoder->setTexture(normalRT.get(), 1);
    raytraceAOEncoder->setTexture(aoRT.get(), 2);
    raytraceAOEncoder->setBuffer(frameDataBuffers[currentFrameInFlight].get(), 0, 0);
    raytraceAOEncoder->setBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 1);
    raytraceAOEncoder->setAccelerationStructure(TLASBuffers[currentFrameInFlight].get(), 2);
    raytraceAOEncoder->dispatchThreadgroups(MTL::Size(screenSize.x, screenSize.y, 1), MTL::Size(1, 1, 1));
    raytraceAOEncoder->endEncoding();

    // 4. render pass
    auto renderEncoder = cmd->renderCommandEncoder(renderPass.get());
    renderEncoder->setRenderPipelineState(drawPipeline.get());
    renderEncoder->setCullMode(MTL::CullModeBack);
    renderEncoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
    renderEncoder->setDepthStencilState(depthStencilState.get());
    // encoder->useResource(clusterBuffer.get(), MTL::ResourceUsageRead, MTL::RenderStageFragment);
    // encoder->useResource(pointLightBuffer.get(), MTL::ResourceUsageRead, MTL::RenderStageFragment);
    // encoder->useResource(testStorageBuffer.get(), MTL::ResourceUsageRead, MTL::RenderStageVertex | MTL::RenderStageFragment);

    currentInstanceCount = 0;
    culledInstanceCount = 0;
    renderEncoder->setVertexBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 0);
    renderEncoder->setVertexBuffer(materialDataBuffer.get(), 0, 1);
    renderEncoder->setVertexBuffer(instanceDataBuffers[currentFrameInFlight].get(), 0, 2);
    renderEncoder->setVertexBuffer(getBuffer(sceneVertexBuffer).get(), 0, 3);
    renderEncoder->setFragmentBuffer(directionalLightBuffer.get(), 0, 0);
    renderEncoder->setFragmentBuffer(pointLightBuffer.get(), 0, 1);
    renderEncoder->setFragmentBuffer(clusterBuffers[currentFrameInFlight].get(), 0, 2);
    renderEncoder->setFragmentBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 3);
    renderEncoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 4);
    renderEncoder->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
    renderEncoder->setFragmentBytes(&time, sizeof(float), 6);
    for (const auto& [material, meshes] : instanceBatches) {
        // renderEncoder->setRenderPipelineState(getPipeline(material->pipeline).get());
        TextureHandle albedoTexture = material->albedoMap ? imageToTextureMap[material->albedoMap] : defaultAlbedoTexture;
        TextureHandle normalTexture = material->normalMap ? imageToTextureMap[material->normalMap] : defaultNormalTexture;
        renderEncoder->setFragmentTexture(
            getTexture(albedoTexture).get(),
            0
        );
        renderEncoder->setFragmentTexture(
            getTexture(normalTexture).get(),
            1
        );
        TextureHandle metallicTexture = material->metallicMap ? imageToTextureMap[material->metallicMap] : defaultORMTexture;
        TextureHandle roughnessTexture = material->roughnessMap ? imageToTextureMap[material->roughnessMap] : defaultORMTexture;
        TextureHandle occlusionTexture = material->occlusionMap ? imageToTextureMap[material->occlusionMap] : defaultORMTexture;
        TextureHandle emissiveTexture = material->emissiveMap ? imageToTextureMap[material->emissiveMap] : defaultEmissiveTexture;
        renderEncoder->setFragmentTexture(
            getTexture(metallicTexture).get(),
            2
        );
        renderEncoder->setFragmentTexture(
            getTexture(roughnessTexture).get(),
            3
        );
        renderEncoder->setFragmentTexture(
            getTexture(occlusionTexture).get(),
            4
        );
        renderEncoder->setFragmentTexture(
            getTexture(emissiveTexture).get(),
            5
        );
        // renderEncoder->setFragmentTexture(
        //     getTexture(material->displacementMap ? material->displacementMap->texture : defaultDisplacementTexture).get(),
        //     6
        // );
        renderEncoder->setFragmentTexture(
            shadowRT.get(),
            7
        );
        for (const auto& mesh : meshes) {
            if (!camera.isVisible(mesh->getWorldBoundingSphere())) {
                culledInstanceCount++;
                continue;
            }
            currentInstanceCount++;
            // renderEncoder->setVertexBuffer(getBuffer(mesh->vbos[0]).get(), 0, 2);
            const MeshGPUResources& resources = meshGPUResources[mesh];
            renderEncoder->setVertexBytes(&resources.instanceID, sizeof(Uint32), 4);
            renderEncoder->drawIndexedPrimitives(
                MTL::PrimitiveType::PrimitiveTypeTriangle,
                resources.indexCount,// mesh->indices.size(),
                MTL::IndexTypeUInt32,
                getBuffer(sceneIndexBuffer).get(), // getBuffer(mesh->ebo).get(),
                resources.indexOffset * sizeof(Uint32) // 0
            );
            drawCount++;
        }
    }
    renderEncoder->endEncoding();

    // 5. post-processing pass
    auto postProcessEncoder = cmd->renderCommandEncoder(postPass.get());
    postProcessEncoder->setRenderPipelineState(postProcessPipeline.get());
    postProcessEncoder->setCullMode(MTL::CullModeBack);
    postProcessEncoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
    postProcessEncoder->setFragmentTexture(colorRT.get(), 0);
    postProcessEncoder->setFragmentTexture(aoRT.get(), 1);
    postProcessEncoder->setFragmentTexture(normalRT.get(), 2);
    postProcessEncoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
    postProcessEncoder->endEncoding();

    ImGui_ImplMetal_NewFrame(imguiPass.get());
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // ImGui::DockSpaceOverViewport();

    if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ImGui::Text("Frame rate: %.3f ms/frame (%.1f FPS)", 1000.0f * deltaTime, 1.0f / deltaTime);
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
                    // TODO: show error image if texture is not uploaded
                    if (m->albedoMap) {
                        ImGui::Text("Albedo Map");
                        TextureHandle albedoTexture = imageToTextureMap[m->albedoMap];
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(albedoTexture).get(), ImVec2(64, 64));
                    }
                    if (m->normalMap) {
                        ImGui::Text("Normal Map");
                        TextureHandle normalTexture = imageToTextureMap[m->normalMap];
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(normalTexture).get(), ImVec2(64, 64));
                    }
                    if (m->metallicMap) {
                        ImGui::Text("Metallic Map");
                        TextureHandle metallicTexture = imageToTextureMap[m->metallicMap];
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(metallicTexture).get(), ImVec2(64, 64));
                    }
                    if (m->roughnessMap) {
                        ImGui::Text("Roughness Map");
                        TextureHandle roughnessTexture = imageToTextureMap[m->roughnessMap];
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(roughnessTexture).get(), ImVec2(64, 64));
                    }
                    if (m->occlusionMap) {
                        ImGui::Text("Occlusion Map");
                        TextureHandle occlusionTexture = imageToTextureMap[m->occlusionMap];
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(occlusionTexture).get(), ImVec2(64, 64));
                    }
                    if (m->emissiveMap) {
                        ImGui::Text("Emissive Map");
                        TextureHandle emissiveTexture = imageToTextureMap[m->emissiveMap];
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(emissiveTexture).get(), ImVec2(64, 64));
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
            // Note: Scene no longer has vertices/indices - calculate from meshes
            size_t totalVertices = 0;
            size_t totalIndices = 0;
            const std::function<void(const std::shared_ptr<Node>&)> countGeometry = [&](const std::shared_ptr<Node>& node) {
                if (node->meshGroup) {
                    for (auto& mesh : node->meshGroup->meshes) {
                        totalVertices += mesh->vertices.size();
                        totalIndices += mesh->indices.size();
                    }
                }
                for (const auto& child : node->children) {
                    countGeometry(child);
                }
            };
            for (const auto& node : scene->nodes) {
                countGeometry(node);
            }
            ImGui::Text("Total vertices: %zu", totalVertices);
            ImGui::Text("Total indices: %zu", totalIndices);
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
                            const MeshGPUResources& resources = meshGPUResources[mesh];
                            ImGui::Text("Vertex count: %u", resources.vertexCount);
                            ImGui::Text("Vertex offset: %u", resources.vertexOffset);
                            ImGui::Text("Index count: %u", resources.indexCount);
                            ImGui::Text("Index offset: %u", resources.indexOffset);
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

    ImGui::Render();
    auto imguiEncoder = cmd->renderCommandEncoder(imguiPass.get());
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, imguiEncoder);
    imguiEncoder->endEncoding();

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

Renderer_Metal::TextureHandle Renderer_Metal::createTexture(const std::shared_ptr<Image>& img) {
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

        return Renderer_Metal::TextureHandle { nextTextureID++ };
    } else {
        throw std::runtime_error(fmt::format("Failed to create texture at {}!\n", img->uri));
    }
}

Renderer_Metal::BufferHandle Renderer_Metal::createVertexBuffer(const std::vector<VertexData>& vertices) {
    auto stagingBuffer = NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), vertices.data(), vertices.size() * sizeof(VertexData));

    auto buffer = NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, vertices.size() * sizeof(VertexData));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return Renderer_Metal::BufferHandle { nextBufferID++ };
}

Renderer_Metal::BufferHandle Renderer_Metal::createIndexBuffer(const std::vector<Uint32>& indices) {
    auto stagingBuffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), indices.data(), indices.size() * sizeof(Uint32));

    auto buffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, indices.size() * sizeof(Uint32));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return Renderer_Metal::BufferHandle { nextBufferID++ };
}

NS::SharedPtr<MTL::Buffer> Renderer_Metal::getBuffer(BufferHandle handle) const {
    return buffers.at(handle.id);
}

NS::SharedPtr<MTL::Texture> Renderer_Metal::getTexture(TextureHandle handle) const {
    return textures.at(handle.id);
}

NS::SharedPtr<MTL::RenderPipelineState> Renderer_Metal::getPipeline(PipelineHandle handle) const {
    return pipelines.at(handle.id);
}