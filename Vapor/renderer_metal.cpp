#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "renderer_metal.hpp"

#include "fmt/core.h"
#include "helper.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "glm/gtc/type_ptr.hpp"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"

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
    // initTestBuffer();
    // initTestTexture();
}


auto Renderer_Metal::draw() -> void {
    auto surface = swapchain->nextDrawable();

    // Create default render pass
    auto pass = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto attachment = pass->colorAttachments()->object(0);
    attachment->setClearColor(clearColor);
    attachment->setLoadAction(MTL::LoadActionClear);
    attachment->setTexture(surface->texture());

    auto buffer = queue->commandBuffer();
    auto encoder = buffer->renderCommandEncoder(pass.get());

    encoder->setRenderPipelineState(testPipeline.get());
    encoder->setFragmentTexture(testTexture.get(), 0);
    encoder->setVertexBuffer(testPosBuffer.get(), 0, 0);
    encoder->setVertexBuffer(testUVBuffer.get(), 0, 1);
    encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));

    encoder->endEncoding();

    buffer->presentDrawable(surface);
    buffer->commit();

    surface->release();
}

void Renderer_Metal::initTestPipeline() {
    auto shaderSrc = readFile("assets/shaders/triforce_no_buf.metal");

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
}

void Renderer_Metal::initTestTexture() {
    NS::Error* error = nullptr;

    auto textureDesc = MTL::TextureDescriptor::alloc()->init();
    textureDesc->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
    textureDesc->setTextureType(MTL::TextureType::TextureType2D);
    textureDesc->setWidth(NS::UInteger(1024));
    textureDesc->setHeight(NS::UInteger(1024));
    textureDesc->setMipmapLevelCount(1);
    textureDesc->setSampleCount(1);
    textureDesc->setStorageMode(MTL::StorageMode::StorageModeManaged);
    textureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

    testTexture = NS::TransferPtr(device->newTexture(textureDesc));
    int width, height, nChannels;
    uint8_t* data = stbi_load("assets/textures/rick_roll.png", &width, &height, &nChannels, 0);
    if (data) {
        testTexture->replaceRegion(MTL::Region(0, 0, 0, width, height, 1), 0, data, width * nChannels);
    } else {
        fmt::print("Failed to load texture!\n");
    }
    stbi_image_free(data);

    textureDesc->release();
}