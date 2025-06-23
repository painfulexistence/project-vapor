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

struct CameraData {
    alignas(16) glm::mat4 projectionMatrix;
    alignas(16) glm::mat4 viewMatrix;
};

struct InstanceData {
    alignas(16) glm::mat4 modelMatrix;
    glm::vec4 color;
};

Renderer_Metal::Renderer_Metal(SDL_Window* window) {
    renderer = SDL_CreateRenderer(window, nullptr);
    swapchain = (CA::MetalLayer*)SDL_GetRenderMetalLayer(renderer);
    device = swapchain->device();
    queue = NS::TransferPtr(device->newCommandQueue());
}

Renderer_Metal::~Renderer_Metal() {
    SDL_DestroyRenderer(renderer);
}

auto Renderer_Metal::init() -> void {
    initTestPipelines();

    // Create buffers
    cameraDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(CameraData), MTL::ResourceStorageModeManaged));
    instanceDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(InstanceData) * numMaxInstances, MTL::ResourceStorageModeManaged));

    // Create textures
    defaultAlbedoTexture = createTexture(AssetManager::loadImage("assets/textures/default_albedo.png")); // createTexture(AssetManager::loadImage("assets/textures/viking_room.png"));
    defaultNormalTexture = createTexture(AssetManager::loadImage("assets/textures/default_norm.png"));
    defaultORMTexture = createTexture(AssetManager::loadImage("assets/textures/default_orm.png"));
    defaultEmissiveTexture = createTexture(AssetManager::loadImage("assets/textures/default_emissive.png"));

    MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
    depthStencilDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLess);
    depthStencilDesc->setDepthWriteEnabled(true);
    depthStencilState = NS::TransferPtr(device->newDepthStencilState(depthStencilDesc));
    depthStencilDesc->release();

    MTL::TextureDescriptor* depthStencilTextureDesc = MTL::TextureDescriptor::alloc()->init();
    depthStencilTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    depthStencilTextureDesc->setPixelFormat(MTL::PixelFormatDepth32Float);
    depthStencilTextureDesc->setWidth(swapchain->drawableSize().width);
    depthStencilTextureDesc->setHeight(swapchain->drawableSize().height);
    depthStencilTextureDesc->setSampleCount(sampleCount);
    depthStencilTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    depthStencilTexture = NS::TransferPtr(device->newTexture(depthStencilTextureDesc));
    depthStencilTextureDesc->release();

    MTL::TextureDescriptor* msaaTextureDesc = MTL::TextureDescriptor::alloc()->init();
    msaaTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    msaaTextureDesc->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    msaaTextureDesc->setWidth(swapchain->drawableSize().width);
    msaaTextureDesc->setHeight(swapchain->drawableSize().height);
    msaaTextureDesc->setSampleCount(sampleCount);
    msaaTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    msaaTexture = NS::TransferPtr(device->newTexture(msaaTextureDesc));
    msaaTextureDesc->release();
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
    auto time = (float)SDL_GetTicks() / 1000.0f;

    auto surface = swapchain->nextDrawable();

    // Create default render pass
    auto pass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto colorAttachment = pass->colorAttachments()->object(0);
    colorAttachment->setTexture(msaaTexture.get());
    colorAttachment->setClearColor(MTL::ClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a));
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionMultisampleResolve);
    colorAttachment->setResolveTexture(surface->texture());
    auto depthAttachment = pass->depthAttachment();
    depthAttachment->setTexture(depthStencilTexture.get());

    auto cmd = queue->commandBuffer();

    glm::vec3 camPos = camera.GetEye();
    CameraData* cameraData = reinterpret_cast<CameraData*>(cameraDataBuffer->contents());
    cameraData->projectionMatrix = camera.GetProjMatrix();
    cameraData->viewMatrix = camera.GetViewMatrix();
    cameraDataBuffer->didModifyRange(NS::Range::Make(0, cameraDataBuffer->length()));

    auto encoder = cmd->renderCommandEncoder(pass.get());

    encoder->setRenderPipelineState(testDrawPipeline.get());
    // encoder->useResource(testStorageBuffer.get(), MTL::ResourceUsageRead, MTL::RenderStageVertex | MTL::RenderStageFragment);

    const std::function<void(const std::shared_ptr<Node>&)> drawNode =
        [&](const std::shared_ptr<Node>& node) {
            if (node->meshGroup) {
                // single instance
                InstanceData* instance = reinterpret_cast<InstanceData*>(instanceDataBuffer->contents());
                instance->modelMatrix = node->worldTransform;
                instance->color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                // multiple instances
                // std::vector<InstanceData> instances = {{
                //     { glm::rotate(glm::identity<glm::mat4>(), angle, glm::vec3(1.0f, 0.0f, 1.0f)), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
                //     { glm::rotate(glm::identity<glm::mat4>(), angle + 0.5f, glm::vec3(1.0f, 0.0f, 1.0f)), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) },
                // }};
                // for (size_t i = 0; i < instances.size(); ++i) {
                //     InstanceData* instance = reinterpret_cast<InstanceData*>(instanceDataBuffer->contents()) + i;
                //     instance->modelMatrix = instances[i].modelMatrix;
                //     instance->color = instances[i].color;
                // }
                instanceDataBuffer->didModifyRange(NS::Range::Make(0, instanceDataBuffer->length())); // TODO: avoid updating the entire instance data buffer every frame

                for (const auto& mesh : node->meshGroup->meshes) {
                    if (!mesh->material) {
                        fmt::print("No material found for mesh in mesh group {}\n", node->meshGroup->name);
                        continue;
                    }
                    // encoder->setRenderPipelineState(getPipeline(mesh->material->pipeline));
                    encoder->setFragmentTexture(
                        getTexture(mesh->material->albedoMap ? mesh->material->albedoMap->texture : defaultAlbedoTexture).get(),
                        0
                    );
                    encoder->setFragmentTexture(
                        getTexture(mesh->material->normalMap ? mesh->material->normalMap->texture : defaultNormalTexture).get(),
                        1
                    );
                    encoder->setFragmentTexture(
                        getTexture(mesh->material->metallicRoughnessMap ? mesh->material->metallicRoughnessMap->texture : defaultORMTexture).get(),
                        2
                    );
                    encoder->setFragmentTexture(
                        getTexture(mesh->material->occlusionMap ? mesh->material->occlusionMap->texture : defaultORMTexture).get(),
                        3
                    );
                    encoder->setFragmentTexture(
                        getTexture(mesh->material->emissiveMap ? mesh->material->emissiveMap->texture : defaultEmissiveTexture).get(),
                        4
                    );
                    // encoder->setFragmentTexture(
                    //     getTexture(mesh->material->displacementMap ? mesh->material->displacementMap->texture : defaultDisplacementTexture).get(),
                    //     5
                    // );
                    encoder->setVertexBuffer(getBuffer(mesh->vbos[0]).get(), 0, 0);
                    encoder->setVertexBuffer(cameraDataBuffer.get(), 0, 1);
                    encoder->setVertexBuffer(instanceDataBuffer.get(), 0, 2);
                    encoder->setFragmentBytes(&camPos, sizeof(glm::vec3), 0);
                    encoder->setFragmentBytes(&time, sizeof(float), 1);
                    encoder->setFragmentBuffer(directionalLightBuffer.get(), 0, 2);
                    encoder->setFragmentBuffer(pointLightBuffer.get(), 0, 3);
                    encoder->setCullMode(MTL::CullModeBack);
                    encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
                    encoder->setDepthStencilState(depthStencilState.get());
                    if (mesh->indices.size() > 0) {
                        encoder->drawIndexedPrimitives(
                            MTL::PrimitiveType::PrimitiveTypeTriangle,
                            mesh->indices.size(), // getBuffer(mesh->ebo)->length() / sizeof(Uint32)
                            MTL::IndexTypeUInt32,
                            getBuffer(mesh->ebo).get(),
                            0
                        );
                    } else {
                        encoder->drawPrimitives(
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

    encoder->endEncoding();

    cmd->presentDrawable(surface);
    cmd->commit();

    surface->release();
}

void Renderer_Metal::initTestPipelines() {
    testDrawPipeline = createPipeline("assets/shaders/3d_pbr_normal_mapped.metal");
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
    fmt::print("Shader compiled successfully. Shader: {}\n", code->cString(NS::StringEncoding::UTF8StringEncoding));

    auto vertexFuncName = NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding);
    auto vertexMain = library->newFunction(vertexFuncName);

    auto fragmentFuncName = NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding);
    auto fragmentMain = library->newFunction(fragmentFuncName);

    auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    pipelineDesc->setVertexFunction(vertexMain);
    pipelineDesc->setFragmentFunction(fragmentMain);
    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
    pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    pipelineDesc->setSampleCount(sampleCount);

    auto pipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));

    code->release();
    library->release();
    vertexMain->release();
    fragmentMain->release();
    pipelineDesc->release();

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