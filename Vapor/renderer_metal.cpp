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

struct alignas(16) CameraData {
    glm::mat4 proj;
    glm::mat4 view;
    glm::mat4 invProj;
    float near;
    float far;
};

struct InstanceData {
    alignas(16) glm::mat4 model;
    glm::vec4 color;
};

Renderer_Metal::Renderer_Metal(SDL_Window* window) {
    renderer = SDL_CreateRenderer(window, nullptr);
    swapchain = (CA::MetalLayer*)SDL_GetRenderMetalLayer(renderer);
    // swapchain->setDisplaySyncEnabled(true);
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
    drawPipeline = createPipeline("assets/shaders/3d_pbr_normal_mapped.metal");
    prePassPipeline = createPipeline("assets/shaders/3d_depth_only.metal");
    postProcessPipeline = createPipeline("assets/shaders/3d_post_process.metal");
    buildClustersPipeline = createComputePipeline("assets/shaders/3d_cluster_build.metal");
    cullLightsPipeline = createComputePipeline("assets/shaders/3d_light_cull.metal");
    tileCullingPipeline = createComputePipeline("assets/shaders/3d_tile_light_cull.metal");

    // Create buffers
    cameraDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& cameraDataBuffer : cameraDataBuffers) {
        cameraDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(CameraData), MTL::ResourceStorageModeManaged));
    }
    instanceDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& instanceDataBuffer : instanceDataBuffers) {
        instanceDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(InstanceData) * numMaxInstances, MTL::ResourceStorageModeManaged));
    }

    std::vector<Particle> particles{1000};
    testStorageBuffer = NS::TransferPtr(device->newBuffer(particles.size() * sizeof(Particle), MTL::ResourceStorageModeManaged));
    memcpy(testStorageBuffer->contents(), particles.data(), particles.size() * sizeof(Particle));
    testStorageBuffer->didModifyRange(NS::Range::Make(0, testStorageBuffer->length()));

    clusterBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& clusterBuffer : clusterBuffers) {
        clusterBuffer = NS::TransferPtr(device->newBuffer(clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ * sizeof(Cluster), MTL::ResourceStorageModeManaged));
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
    colorTextureDesc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
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

    // Create depth stencil states (for depth testing)
    MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
    depthStencilDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
    depthStencilDesc->setDepthWriteEnabled(true);
    depthStencilState = NS::TransferPtr(device->newDepthStencilState(depthStencilDesc));
    depthStencilDesc->release();
}

auto Renderer_Metal::stage(std::shared_ptr<Scene> scene) -> void {
    // Buffers
    const std::function<void(const std::shared_ptr<Node>&)> stageNode =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                for (auto& mesh : node->meshGroup->meshes) {
                    mesh->vbos.push_back(createVertexBuffer(mesh->vertices)); // TODO: use single vbo for all meshes
                    mesh->ebo = createIndexBuffer(mesh->indices);
                }
            }
            for (const auto& child : node->children) {
                stageNode(child);
            }
        };
    for (auto& node : scene->nodes) {
        stageNode(node);
    }

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
    }
}

auto Renderer_Metal::draw(std::shared_ptr<Scene> scene, Camera& camera) -> void {
    auto surface = swapchain->nextDrawable();
    if (!surface) {
        return;
    }

    // Prepare data
    auto time = (float)SDL_GetTicks() / 1000.0f;

    float near = camera.near();
    float far = camera.far();
    glm::vec3 camPos = camera.GetEye();
    glm::mat4 proj = camera.GetProjMatrix();
    glm::mat4 view = camera.GetViewMatrix();
    glm::mat4 invProj = glm::inverse(proj);
    CameraData* cameraData = reinterpret_cast<CameraData*>(cameraDataBuffers[currentFrameInFlight]->contents());
    cameraData->proj = proj;
    cameraData->view = view;
    cameraData->invProj = invProj;
    cameraData->near = near;
    cameraData->far = far;
    cameraDataBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, cameraDataBuffers[currentFrameInFlight]->length()));

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
    uint lightCount = scene->pointLights.size();

    // Create render pass descriptors
    auto prePass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
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

    // 1. pre-pass
    auto prePassEncoder = cmd->renderCommandEncoder(prePass.get());
    prePassEncoder->setRenderPipelineState(prePassPipeline.get());
    prePassEncoder->setCullMode(MTL::CullModeBack);
    prePassEncoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
    prePassEncoder->setDepthStencilState(depthStencilState.get());
    const std::function<void(const std::shared_ptr<Node>&)> drawNodeDepth =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                // single instance
                InstanceData* instance = reinterpret_cast<InstanceData*>(instanceDataBuffers[currentFrameInFlight]->contents());
                instance->model = node->worldTransform;
                instance->color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                // multiple instances
                // std::vector<InstanceData> instances = {{
                //     { glm::rotate(glm::identity<glm::mat4>(), angle, glm::vec3(1.0f, 0.0f, 1.0f)), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
                //     { glm::rotate(glm::identity<glm::mat4>(), angle + 0.5f, glm::vec3(1.0f, 0.0f, 1.0f)), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) },
                // }};
                // for (size_t i = 0; i < instances.size(); ++i) {
                //     InstanceData* instance = reinterpret_cast<InstanceData*>(instanceDataBuffers[currentFrameInFlight]->contents()) + i;
                //     instance->model = instances[i].model;
                //     instance->color = instances[i].color;
                // }
                instanceDataBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, instanceDataBuffers[currentFrameInFlight]->length())); // TODO: avoid updating the entire instance data buffer every frame

                for (const auto& mesh : node->meshGroup->meshes) {
                    if (!mesh->material) {
                        fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                        continue;
                    }
                    // prePassEncoder->setFragmentTexture(
                    //     getTexture(mesh->material->displacementMap ? mesh->material->displacementMap->texture : defaultDisplacementTexture).get(),
                    //     1
                    // );
                    prePassEncoder->setVertexBuffer(getBuffer(mesh->vbos[0]).get(), 0, 0);
                    prePassEncoder->setVertexBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 1);
                    prePassEncoder->setVertexBuffer(instanceDataBuffers[currentFrameInFlight].get(), 0, 2);
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
                            mesh->positions.size(),
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
    tileCullingEncoder->setBytes(&lightCount, sizeof(uint), 3);
    tileCullingEncoder->setBytes(&gridSize, sizeof(glm::uvec3), 4);
    tileCullingEncoder->setBytes(&screenSize, sizeof(glm::vec2), 5);
    tileCullingEncoder->dispatchThreadgroups(MTL::Size(clusterGridSizeX, clusterGridSizeY, 1), MTL::Size(1, 1, 1));
    tileCullingEncoder->endEncoding();

    // 4. render pass
    auto renderEncoder = cmd->renderCommandEncoder(renderPass.get());
    renderEncoder->setRenderPipelineState(drawPipeline.get());
    renderEncoder->setCullMode(MTL::CullModeBack);
    renderEncoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
    renderEncoder->setDepthStencilState(depthStencilState.get());
    // encoder->useResource(clusterBuffer.get(), MTL::ResourceUsageRead, MTL::RenderStageFragment);
    // encoder->useResource(pointLightBuffer.get(), MTL::ResourceUsageRead, MTL::RenderStageFragment);
    // encoder->useResource(testStorageBuffer.get(), MTL::ResourceUsageRead, MTL::RenderStageVertex | MTL::RenderStageFragment);

    const std::function<void(const std::shared_ptr<Node>&)> drawNode =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                // single instance
                InstanceData* instance = reinterpret_cast<InstanceData*>(instanceDataBuffers[currentFrameInFlight]->contents());
                instance->model = node->worldTransform;
                instance->color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                // multiple instances
                // std::vector<InstanceData> instances = {{
                //     { glm::rotate(glm::identity<glm::mat4>(), angle, glm::vec3(1.0f, 0.0f, 1.0f)), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
                //     { glm::rotate(glm::identity<glm::mat4>(), angle + 0.5f, glm::vec3(1.0f, 0.0f, 1.0f)), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) },
                // }};
                // for (size_t i = 0; i < instances.size(); ++i) {
                //     InstanceData* instance = reinterpret_cast<InstanceData*>(instanceDataBuffers[currentFrameInFlight]->contents()) + i;
                //     instance->model = instances[i].model;
                //     instance->color = instances[i].color;
                // }
                instanceDataBuffers[currentFrameInFlight]->didModifyRange(NS::Range::Make(0, instanceDataBuffers[currentFrameInFlight]->length())); // TODO: avoid updating the entire instance data buffer every frame

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
                    renderEncoder->setVertexBuffer(getBuffer(mesh->vbos[0]).get(), 0, 0);
                    renderEncoder->setVertexBuffer(cameraDataBuffers[currentFrameInFlight].get(), 0, 1);
                    renderEncoder->setVertexBuffer(instanceDataBuffers[currentFrameInFlight].get(), 0, 2);
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
                            mesh->positions.size(),
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
    postProcessEncoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
    postProcessEncoder->endEncoding();

    cmd->presentDrawable(surface);
    cmd->commit();

    surface->release();

    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
}


NS::SharedPtr<MTL::RenderPipelineState> Renderer_Metal::createPipeline(const std::string& filename) {
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
    colorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm);
    // colorAttachment->setBlendingEnabled(true);
    // colorAttachment->setAlphaBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setRgbBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    // colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    pipelineDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT)); // TODO: make this configurable

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
        MTL::PixelFormat pixelFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
        int numLevels = static_cast<int>(std::floor(std::log2(std::max(img->width, img->height))) + 1);
        switch (img->channelCount) {
        case 1:
            pixelFormat = MTL::PixelFormat::PixelFormatR8Unorm;
            break;
        case 3:
        case 4:
            pixelFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
            break;
        default:
            throw std::runtime_error(fmt::format("Unknown texture format at {}\n", img->uri));
            break;
        }

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

        auto cmdBlit = NS::TransferPtr(queue->commandBuffer());

        auto enc = NS::TransferPtr(cmdBlit->blitCommandEncoder());
        enc->generateMipmaps(texture.get());
        enc->endEncoding();

        cmdBlit->commit();

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