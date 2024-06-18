#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "renderer_metal.hpp"
#include "fmt/core.h"
#include "helper.hpp"

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
    encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));

    encoder->endEncoding();

    buffer->presentDrawable(surface);
    buffer->commit();

    surface->release();
}

void Renderer_Metal::initTestPipeline() {
    auto shaderSrc = readFile("shaders/triforce.metal");

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