#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "renderer_metal.hpp"

#include <fmt/core.h>
#include <SDL3/SDL_stdinc.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <vector>
#include <functional>

#include "graphics.hpp"
#include "asset_manager.hpp"
#include "helper.hpp"


std::unique_ptr<Renderer> createRendererMetal(SDL_Window* window) {
    return std::make_unique<Renderer_Metal>(window);
}

Renderer_Metal::Renderer_Metal(SDL_Window* window) {
    renderer = SDL_CreateRenderer(window, nullptr);
    swapchain = (CA::MetalLayer*)SDL_GetRenderMetalLayer(renderer);
    // swapchain->setDisplaySyncEnabled(true);
    swapchain->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
    swapchain->setColorspace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    device = swapchain->device();
    queue = NS::TransferPtr(device->newCommandQueue());
}

Renderer_Metal::~Renderer_Metal() {
    SDL_DestroyRenderer(renderer);
}
struct Particle {
    glm::vec3 position = glm::vec3(1.0f);
    glm::vec3 velocity = glm::vec3(1.0f);
    glm::vec3 density = glm::vec3(1.0f);
};

auto Renderer_Metal::init() -> void {
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

    // Pipelines
    if (scene->materials.empty()) {
        // TODO: create default material
    }
    for (auto& mat : scene->materials) {
        // pipelines[mat->pipeline] = createPipeline();
        materialIDs[mat] = nextMaterialID++;
    }

    // Buffers
    auto cmd = queue->commandBuffer();

    const std::function<void(const std::shared_ptr<Node>&)> stageNode =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                for (auto& mesh : node->meshGroup->meshes) {
                    mesh->vbos.push_back(createVertexBuffer(mesh->vertices)); // TODO: use single vbo for all meshes
                    mesh->ebo = createIndexBuffer(mesh->indices);

                    auto geomDesc = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
                    geomDesc->setVertexBuffer(getBuffer(mesh->vbos[0]).get());
                    geomDesc->setVertexStride(sizeof(VertexData));
                    geomDesc->setVertexFormat(MTL::AttributeFormatFloat3);
                    geomDesc->setVertexBufferOffset(offsetof(VertexData, position));
                    geomDesc->setIndexBuffer(getBuffer(mesh->ebo).get());
                    geomDesc->setIndexType(MTL::IndexTypeUInt32);
                    geomDesc->setIndexBufferOffset(0);
                    geomDesc->setTriangleCount(mesh->indices.size() / 3);
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
    glm::vec3 camPos = camera.GetEye();
    glm::mat4 proj = camera.GetProjMatrix();
    glm::mat4 view = camera.GetViewMatrix();
    glm::mat4 invProj = glm::inverse(proj);
    glm::mat4 invView = glm::inverse(view);
    CameraData* cameraData = reinterpret_cast<CameraData*>(cameraDataBuffers[currentFrameInFlight]->contents());
    cameraData->proj = proj;
    cameraData->view = view;
    cameraData->invProj = invProj;
    cameraData->invView = invView;
    cameraData->near = near;
    cameraData->far = far;
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

    auto drawableSize = swapchain->drawableSize();
    glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
    glm::uvec3 gridSize = glm::uvec3(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
    uint pointLightCount = scene->pointLights.size();
    uint directionalLightCount = scene->directionalLights.size();

    instances.clear();
    accelInstances.clear();
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
                    .boundingBoxMin = mesh->boundingBoxMin,
                    .boundingBoxMax = mesh->boundingBoxMax,
                    .boundingSphere = mesh->boundingSphere
                });
                MTL::AccelerationStructureInstanceDescriptor accelInstanceDesc;
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 4; ++j) {
                        accelInstanceDesc.transformationMatrix.columns[i][j] = transform[i][j];
                    }
                }
                accelInstanceDesc.accelerationStructureIndex = mesh->instanceID;
                accelInstanceDesc.mask = 0xFF;
                accelInstances.push_back(accelInstanceDesc);
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

    std::vector<NS::Object*> BLASObjects;
    BLASObjects.reserve(BLASs.size());
    for (auto blas : BLASs) {
        BLASObjects.push_back(static_cast<NS::Object*>(blas.get()));
    }
    auto TLASDesc = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
    auto BLASArray = NS::TransferPtr(NS::Array::array(BLASObjects.data(), BLASObjects.size()));
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

    // Start rendering
    auto cmd = queue->commandBuffer();

    // 0. build TLAS
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
    prePassEncoder->setVertexBuffer(instanceDataBuffers[currentFrameInFlight].get(), 0, 1);
    const std::function<void(const std::shared_ptr<Node>&)> drawNodeDepth =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                for (const auto& mesh : node->meshGroup->meshes) {
                    if (!mesh->material) {
                        fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                        continue;
                    }
                    // prePassEncoder->setFragmentTexture(
                    //     getTexture(mesh->material->displacementMap ? mesh->material->displacementMap->texture : defaultDisplacementTexture).get(),
                    //     1
                    // );
                    prePassEncoder->setVertexBuffer(getBuffer(mesh->vbos[0]).get(), 0, 2);
                    prePassEncoder->setVertexBytes(&mesh->instanceID, sizeof(Uint32), 3);
                    prePassEncoder->setFragmentTexture(
                        getTexture(mesh->material->albedoMap ? mesh->material->albedoMap->texture : defaultAlbedoTexture).get(),
                        0
                    );
                    if (mesh->indices.size() > 0) {
                        prePassEncoder->drawIndexedPrimitives(
                            MTL::PrimitiveType::PrimitiveTypeTriangle,
                            mesh->indices.size(),
                            MTL::IndexTypeUInt32,
                            getBuffer(mesh->ebo).get(),
                            0
                        );
                    } else {
                        prePassEncoder->drawPrimitives(
                            MTL::PrimitiveType::PrimitiveTypeTriangle,
                            0,
                            mesh->vertices.size(),
                            1
                        );
                    }
                }
            }
            for (const auto& child : node->children) {
                drawNodeDepth(child);
            }
        };
    for (const auto& node : scene->nodes) {
        drawNodeDepth(node);
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

    renderEncoder->setVertexBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 0);
    renderEncoder->setVertexBuffer(instanceDataBuffers[currentFrameInFlight].get(), 0, 1);
    const std::function<void(const std::shared_ptr<Node>&)> drawNode =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                for (const auto& mesh : node->meshGroup->meshes) {
                    if (!mesh->material) {
                        fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                        continue;
                    }
                    // encoder->setRenderPipelineState(getPipeline(mesh->material->pipeline));
                    renderEncoder->setFragmentTexture(
                        getTexture(mesh->material->albedoMap ? mesh->material->albedoMap->texture : defaultAlbedoTexture).get(),
                        0
                    );
                    renderEncoder->setFragmentTexture(
                        getTexture(mesh->material->normalMap ? mesh->material->normalMap->texture : defaultNormalTexture).get(),
                        1
                    );
                    renderEncoder->setFragmentTexture(
                        getTexture(mesh->material->metallicRoughnessMap ? mesh->material->metallicRoughnessMap->texture : defaultORMTexture).get(),
                        2
                    );
                    renderEncoder->setFragmentTexture(
                        getTexture(mesh->material->occlusionMap ? mesh->material->occlusionMap->texture : defaultORMTexture).get(),
                        3
                    );
                    renderEncoder->setFragmentTexture(
                        getTexture(mesh->material->emissiveMap ? mesh->material->emissiveMap->texture : defaultEmissiveTexture).get(),
                        4
                    );
                    // encoder->setFragmentTexture(
                    //     getTexture(mesh->material->displacementMap ? mesh->material->displacementMap->texture : defaultDisplacementTexture).get(),
                    //     5
                    // );
                    renderEncoder->setFragmentTexture(
                        shadowRT.get(),
                        7
                    );
                    renderEncoder->setVertexBuffer(getBuffer(mesh->vbos[0]).get(), 0, 2);
                    renderEncoder->setVertexBytes(&mesh->instanceID, sizeof(Uint32), 3);
                    renderEncoder->setFragmentBuffer(directionalLightBuffer.get(), 0, 0);
                    renderEncoder->setFragmentBuffer(pointLightBuffer.get(), 0, 1);
                    renderEncoder->setFragmentBuffer(clusterBuffers[currentFrameInFlight].get(), 0, 2);
                    renderEncoder->setFragmentBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 3);
                    renderEncoder->setFragmentBytes(&camPos, sizeof(glm::vec3), 4);
                    renderEncoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 5);
                    renderEncoder->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 6);
                    renderEncoder->setFragmentBytes(&time, sizeof(float), 7);
                    if (mesh->indices.size() > 0) {
                        renderEncoder->drawIndexedPrimitives(
                            MTL::PrimitiveType::PrimitiveTypeTriangle,
                            mesh->indices.size(),
                            MTL::IndexTypeUInt32,
                            getBuffer(mesh->ebo).get(),
                            0
                        );
                    } else {
                        renderEncoder->drawPrimitives(
                            MTL::PrimitiveType::PrimitiveTypeTriangle,
                            0,
                            mesh->vertices.size(),
                            1
                        );
                    }
                }
            }
            for (const auto& child : node->children) {
                drawNode(child);
            }
        };
    for (const auto& node : scene->nodes) {
        drawNode(node);
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

    code->release();
    library->release();
    vertexMain->release();
    fragmentMain->release();
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
    NS::SharedPtr<MTL::Buffer> buffer = NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModeManaged));

    memcpy(buffer->contents(), vertices.data(), vertices.size() * sizeof(VertexData));
    buffer->didModifyRange(NS::Range::Make(0, buffer->length()));

    buffers[nextBufferID] = buffer;

    return BufferHandle { nextBufferID++ };
}

BufferHandle Renderer_Metal::createIndexBuffer(const std::vector<Uint32>& indices) {
    NS::SharedPtr<MTL::Buffer> buffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModeManaged));

    memcpy(buffer->contents(), indices.data(), indices.size() * sizeof(Uint32));
    buffer->didModifyRange(NS::Range::Make(0, buffer->length()));

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