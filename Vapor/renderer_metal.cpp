#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "renderer_metal.hpp"

Renderer_Metal::Renderer_Metal(SDL_Window* window) {
    renderer = SDL_CreateRenderer(window, -1, 0);
}

Renderer_Metal::~Renderer_Metal() {
    SDL_DestroyRenderer(renderer);
}

auto Renderer_Metal::init() -> void {
    swapchain = (CA::MetalLayer*)SDL_RenderGetMetalLayer(renderer);
    device = swapchain->device();
    queue = device->newCommandQueue();
}

auto Renderer_Metal::draw() -> void {
    auto surface = swapchain->nextDrawable();
    auto pass = MTL::RenderPassDescriptor::renderPassDescriptor();

    MTL::ClearColor clearColor(0.0 / 255.0, 255.0 / 255.0, 255.0 / 255.0, 1.0);
    auto attachment = pass->colorAttachments()->object(0);
    attachment->setClearColor(clearColor);
    attachment->setLoadAction(MTL::LoadActionClear);
    attachment->setTexture(surface->texture());

    auto buffer = queue->commandBuffer();
    auto encoder = buffer->renderCommandEncoder(pass);

    encoder->endEncoding();

    buffer->presentDrawable(surface);
    buffer->commit();

    surface->release();
}