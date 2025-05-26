#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "renderer_metal.hpp"
#include "graphics.hpp"
#include "asset_manager.hpp"

#include "fmt/core.h"
#include "helper.hpp"

#include "glm/vec2.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"
#include <cstdint>
#include <vector>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

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
    auto mesh = MeshBuilder::buildCube(1.0); // AssetManager::loadOBJ(std::string("assets/models/viking_room.obj"));
    testVertexBuffer = createVertexBuffer(mesh->vertices);
    testIndexBuffer = createIndexBuffer(mesh->indices);
    cameraDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(CameraData), MTL::ResourceStorageModeManaged));
    instanceDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(InstanceData) * numMaxInstances, MTL::ResourceStorageModeManaged));

    // Create textures
    testAlbedoTexture = createTexture(std::string("assets/textures/medieval_blocks_diff.jpg")); // createTexture(std::string("assets/textures/viking_room.png"));
    testNormalTexture = createTexture(std::string("assets/textures/medieval_blocks_norm_dx.jpg"));
    testRoughnessTexture = createTexture(std::string("assets/textures/medieval_blocks_rough.jpg"));

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


auto Renderer_Metal::draw() -> void {
    auto time = (float)SDL_GetTicks() / 1000.0f;

    auto surface = swapchain->nextDrawable();

    // Create default render pass
    auto pass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto colorAttachment = pass->colorAttachments()->object(0);
    colorAttachment->setTexture(msaaTexture.get());
    colorAttachment->setClearColor(clearColor);
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionMultisampleResolve);
    colorAttachment->setResolveTexture(surface->texture());
    auto depthAttachment = pass->depthAttachment();
    depthAttachment->setTexture(depthStencilTexture.get());

    auto cmd = queue->commandBuffer();

    float angle = time * 1.5f;
    // single instance
    InstanceData* instance = reinterpret_cast<InstanceData*>(instanceDataBuffer->contents());
    instance->modelMatrix = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, -1.0f, -1.0f)); // glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(270.0f), glm::vec3(1.0f, 0.0f, 0.0f)), angle, glm::vec3(0.0f, 0.0f, 1.0f));
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

    glm::vec3 camPos = glm::vec3(0.0f, 0.0f, 5.0f);
    CameraData* camera = reinterpret_cast<CameraData*>(cameraDataBuffer->contents());
    camera->projectionMatrix = glm::perspective(glm::radians(45.f), 1.333f, 0.03f, 500.0f);
    camera->viewMatrix = glm::lookAt(camPos, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cameraDataBuffer->didModifyRange(NS::Range::Make(0, cameraDataBuffer->length()));

    auto encoder = cmd->renderCommandEncoder(pass.get());

    encoder->setRenderPipelineState(testDrawPipeline.get());
    encoder->setFragmentTexture(testAlbedoTexture.get(), 0);
    encoder->setFragmentTexture(testNormalTexture.get(), 1);
    encoder->setFragmentTexture(testAOTexture.get(), 2);
    encoder->setFragmentTexture(testRoughnessTexture.get(), 3);
    encoder->setFragmentTexture(testMetallicTexture.get(), 4);
    encoder->setVertexBuffer(testVertexBuffer.get(), 0, 0);
    encoder->setVertexBuffer(cameraDataBuffer.get(), 0, 1);
    encoder->setVertexBuffer(instanceDataBuffer.get(), 0, 2);
    encoder->setFragmentBytes(&camPos, sizeof(glm::vec3), 0);
    encoder->setFragmentBytes(&time, sizeof(float), 1);
    encoder->setCullMode(MTL::CullModeBack);
    encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
    encoder->setDepthStencilState(depthStencilState.get());
    // encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));
    encoder->drawIndexedPrimitives(
      MTL::PrimitiveType::PrimitiveTypeTriangle,
      testIndexBuffer->length() / sizeof(uint16_t),
      MTL::IndexTypeUInt16,
      testIndexBuffer.get(),
      0
    );

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

NS::SharedPtr<MTL::Texture> Renderer_Metal::createTexture(const std::string& filename) {
    auto img = AssetManager::loadImage(filename);
    if (img) {
        MTL::PixelFormat pixelFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
        switch (img->channelCount) {
        case 1:
            pixelFormat = MTL::PixelFormat::PixelFormatR8Unorm;
            break;
        case 3:
        case 4:
            pixelFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
            break;
        default:
            throw std::runtime_error(fmt::format("Unknown texture format at {}\n", filename));
            break;
        }

        auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        textureDesc->setPixelFormat(pixelFormat);
        textureDesc->setTextureType(MTL::TextureType::TextureType2D);
        textureDesc->setWidth(NS::UInteger(img->width));
        textureDesc->setHeight(NS::UInteger(img->height));
        textureDesc->setMipmapLevelCount(10);
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

        return texture;
    } else {
        throw std::runtime_error(fmt::format("Failed to create texture at {}!\n", filename));
    }
}

NS::SharedPtr<MTL::Buffer> Renderer_Metal::createVertexBuffer(std::vector<VertexData> vertices) {
    NS::SharedPtr<MTL::Buffer> buffer = NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModeManaged));

    memcpy(buffer->contents(), vertices.data(), vertices.size() * sizeof(VertexData));
    buffer->didModifyRange(NS::Range::Make(0, buffer->length()));

    return buffer;
}

NS::SharedPtr<MTL::Buffer> Renderer_Metal::createIndexBuffer(std::vector<uint16_t> indices) {
    NS::SharedPtr<MTL::Buffer> buffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(uint16_t), MTL::ResourceStorageModeManaged));

    memcpy(buffer->contents(), indices.data(), indices.size() * sizeof(uint16_t));
    buffer->didModifyRange(NS::Range::Make(0, buffer->length()));

    return buffer;
}