#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "renderer_metal.hpp"

#include "fmt/core.h"
#include "helper.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "glm/vec2.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"
#include <cstdint>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

struct CameraData {
    glm::mat4 projectionMatrix;
    glm::mat4 viewMatrix;
};

struct InstanceData {
    glm::mat4 modelMatrix;
    glm::vec4 color;
};

Renderer_Metal::Renderer_Metal(SDL_Window* window) {
    renderer = SDL_CreateRenderer(window, -1, 0);
}

Renderer_Metal::~Renderer_Metal() {
    SDL_DestroyRenderer(renderer);
}

auto Renderer_Metal::init() -> void {
    swapchain = (CA::MetalLayer*)SDL_RenderGetMetalLayer(renderer);
    device = swapchain->device();
    queue = NS::TransferPtr(device->newCommandQueue());

    initTestPipeline();
    initTestBuffer();
    initTestTexture();

    MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
    depthStencilDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLess);
    depthStencilDesc->setDepthWriteEnabled(true);
    depthStencilState = NS::TransferPtr(device->newDepthStencilState(depthStencilDesc));
    depthStencilDesc->release();
}


auto Renderer_Metal::draw() -> void {
    auto time = (float)SDL_GetTicks() / 1000.0f;

    auto surface = swapchain->nextDrawable();

    // Create default render pass
    auto pass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto attachment = pass->colorAttachments()->object(0);
    attachment->setClearColor(clearColor);
    attachment->setLoadAction(MTL::LoadActionClear);
    attachment->setTexture(surface->texture());

    auto cmd = queue->commandBuffer();

    float angle = time * 1.5f;
    InstanceData* instance = reinterpret_cast<InstanceData*>(testCubeInstanceBuffer->contents());
    instance->modelMatrix = glm::rotate(glm::identity<glm::mat4>(), angle, glm::vec3(0.0f, 1.0f, -1.0f));
    instance->color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    testCubeInstanceBuffer->didModifyRange(NS::Range::Make(0, testCubeInstanceBuffer->length()));

    glm::vec3 camPos = glm::vec3(0.0f, 0.0f, 3.0f);
    CameraData* camera = reinterpret_cast<CameraData*>(cameraDataBuffer->contents());
    camera->projectionMatrix = glm::perspective(45.f * (float)M_PI / 180.f, 1.333f, 0.03f, 500.0f);
    camera->viewMatrix = glm::lookAt(camPos, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cameraDataBuffer->didModifyRange(NS::Range::Make(0, cameraDataBuffer->length()));

    auto encoder = cmd->renderCommandEncoder(pass.get());

    encoder->setRenderPipelineState(testPipeline.get());
    encoder->setFragmentTexture(testTexture.get(), 0);
    encoder->setVertexBuffer(testCubeVertexBuffer.get(), 0, 0);
    encoder->setVertexBuffer(cameraDataBuffer.get(), 0, 1);
    encoder->setVertexBuffer(testCubeInstanceBuffer.get(), 0, 2);
    encoder->setFragmentBytes(&camPos, sizeof(glm::vec3), 0);
    encoder->setFragmentBytes(&time, sizeof(float), 1);
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setDepthStencilState(depthStencilState.get());
    // encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));
    encoder->drawIndexedPrimitives(
      MTL::PrimitiveType::PrimitiveTypeTriangle,
      testCubeIndexBuffer->length() / sizeof(uint16_t),
      MTL::IndexTypeUInt16,
      testCubeIndexBuffer.get(),
      0
    );

    encoder->endEncoding();

    cmd->presentDrawable(surface);
    cmd->commit();

    surface->release();
}

void Renderer_Metal::initTestPipeline() {
    auto shaderSrc = readFile("assets/shaders/cube_blinn_phong.metal");

    auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* error = nullptr;
    MTL::CompileOptions* options = nullptr;
    MTL::Library* library = device->newLibrary(code, options, &error);
    if (!library) {
        fmt::print("Could not compile shader! Error: {}\n", error->localizedDescription()->utf8String());
        return;
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

    testPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));

    code->release();
    library->release();
    vertexMain->release();
    fragmentMain->release();
    pipelineDesc->release();
}

void Renderer_Metal::initTestBuffer() {
    glm::vec3 verts[6] = { { -0.5f, 0.5f, 0.0f }, { -0.5f, -0.5f, 0.0f }, { 0.5f, 0.5f, 0.0 },
                           { 0.5f, 0.5f, 0.0f },  { -0.5f, -0.5f, 0.0f }, { 0.5f, -0.5f, 0.0f } };
    glm::vec2 uvs[6] = {
        { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f }
    };

    testPosBuffer = NS::TransferPtr(device->newBuffer(6 * sizeof(glm::vec3), MTL::ResourceStorageModeManaged));
    testUVBuffer = NS::TransferPtr(device->newBuffer(6 * sizeof(glm::vec2), MTL::ResourceStorageModeManaged));

    memcpy(testPosBuffer->contents(), verts, 6 * sizeof(glm::vec3));
    memcpy(testUVBuffer->contents(), uvs, 6 * sizeof(glm::vec2));

    testPosBuffer->didModifyRange(NS::Range::Make(0, testPosBuffer->length()));
    testUVBuffer->didModifyRange(NS::Range::Make(0, testUVBuffer->length()));

    float cubeVerts[] = { // left
                          .5f,
                          .5f,
                          .5f,
                          0.0f,
                          0.0f,
                          1.0f,
                          1.0f,
                          1.0f,
                          .5f,
                          -.5f,
                          .5f,
                          0.0f,
                          0.0f,
                          1.0f,
                          1.0f,
                          0.0f,
                          -.5f,
                          .5f,
                          .5f,
                          0.0f,
                          0.0f,
                          1.0f,
                          0.0f,
                          1.0f,
                          -.5f,
                          -.5f,
                          .5f,
                          0.0f,
                          0.0f,
                          1.0f,
                          0.0f,
                          0.0f,
                          // right
                          .5f,
                          .5f,
                          -.5f,
                          0.0f,
                          0.0f,
                          -1.0f,
                          1.0f,
                          1.0f,
                          .5f,
                          -.5f,
                          -.5f,
                          0.0f,
                          0.0f,
                          -1.0f,
                          1.0f,
                          0.0f,
                          -.5f,
                          .5f,
                          -.5f,
                          0.0f,
                          0.0f,
                          -1.0f,
                          0.0f,
                          1.0f,
                          -.5f,
                          -.5f,
                          -.5f,
                          0.0f,
                          0.0f,
                          -1.0f,
                          0.0f,
                          0.0f,
                          // back
                          -.5f,
                          .5f,
                          .5f,
                          -1.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          1.0f,
                          -.5f,
                          .5f,
                          -.5f,
                          -1.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          -.5f,
                          -.5f,
                          .5f,
                          -1.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          0.0f,
                          -.5f,
                          -.5f,
                          -.5f,
                          -1.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          // front
                          .5f,
                          .5f,
                          .5f,
                          1.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          1.0f,
                          .5f,
                          .5f,
                          -.5f,
                          1.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          .5f,
                          -.5f,
                          .5f,
                          1.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          0.0f,
                          .5f,
                          -.5f,
                          -.5f,
                          1.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          // top
                          .5f,
                          .5f,
                          .5f,
                          0.0f,
                          1.0f,
                          0.0f,
                          1.0f,
                          1.0f,
                          .5f,
                          .5f,
                          -.5f,
                          0.0f,
                          1.0f,
                          0.0f,
                          1.0f,
                          0.0f,
                          -.5f,
                          .5f,
                          .5f,
                          0.0f,
                          1.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          -.5f,
                          .5f,
                          -.5f,
                          0.0f,
                          1.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          // bottom
                          .5f,
                          -.5f,
                          .5f,
                          0.0f,
                          -1.0f,
                          0.0f,
                          1.0f,
                          1.0f,
                          .5f,
                          -.5f,
                          -.5f,
                          0.0f,
                          -1.0f,
                          0.0f,
                          1.0f,
                          0.0f,
                          -.5f,
                          -.5f,
                          .5f,
                          0.0f,
                          -1.0f,
                          0.0f,
                          0.0f,
                          1.0f,
                          -.5f,
                          -.5f,
                          -.5f,
                          0.0f,
                          -1.0f,
                          0.0f,
                          0.0f,
                          0.0f
    };
    uint16_t cubeTris[] = { 0,  2,  1,  1,  2,  3,  4,  5,  6,  6,  5,  7,  8,  9,  10, 10, 9,  11,
                            12, 14, 13, 13, 14, 15, 16, 17, 18, 18, 17, 19, 20, 22, 21, 21, 22, 23 };

    testCubeVertexBuffer = NS::TransferPtr(device->newBuffer(192 * sizeof(float), MTL::ResourceStorageModeManaged));
    testCubeIndexBuffer = NS::TransferPtr(device->newBuffer(36 * sizeof(uint16_t), MTL::ResourceStorageModeManaged));

    memcpy(testCubeVertexBuffer->contents(), cubeVerts, 192 * sizeof(float));
    memcpy(testCubeIndexBuffer->contents(), cubeTris, 36 * sizeof(uint16_t));

    testCubeVertexBuffer->didModifyRange(NS::Range::Make(0, testCubeVertexBuffer->length()));
    testCubeIndexBuffer->didModifyRange(NS::Range::Make(0, testCubeIndexBuffer->length()));

    cameraDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(CameraData), MTL::ResourceStorageModeManaged));

    testCubeInstanceBuffer = NS::TransferPtr(device->newBuffer(sizeof(InstanceData), MTL::ResourceStorageModeManaged));
}

void Renderer_Metal::initTestTexture() {
    int width, height, numChannels;
    uint8_t* data = stbi_load("assets/textures/american_walnut.png", &width, &height, &numChannels, 0);
    if (data) {
        NS::Error* error = nullptr;

        auto textureDesc = MTL::TextureDescriptor::alloc()->init();
        textureDesc->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
        textureDesc->setTextureType(MTL::TextureType::TextureType2D);
        textureDesc->setWidth(NS::UInteger(width));
        textureDesc->setHeight(NS::UInteger(height));
        textureDesc->setMipmapLevelCount(10);
        textureDesc->setSampleCount(1);
        textureDesc->setStorageMode(MTL::StorageMode::StorageModeManaged);
        textureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

        testTexture = NS::TransferPtr(device->newTexture(textureDesc));
        testTexture->replaceRegion(MTL::Region(0, 0, 0, width, height, 1), 0, data, width * numChannels);

        textureDesc->release();

        auto cmdBlit = queue->commandBuffer();

        auto enc = cmdBlit->blitCommandEncoder();
        enc->generateMipmaps(testTexture.get());
        enc->endEncoding();

        cmdBlit->commit();
    } else {
        fmt::print("Failed to load image!\n");
    }
    stbi_image_free(data);
}