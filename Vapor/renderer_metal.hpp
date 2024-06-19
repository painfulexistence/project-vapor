#pragma once
#include "renderer.hpp"

#include "SDL.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

class Renderer_Metal final : Renderer {
public:
    Renderer_Metal(SDL_Window* window);

    ~Renderer_Metal();

    virtual void init();

    virtual void draw();

    void initTestBuffer();

    void initTestTexture();

    void initTestPipeline();

private:
    SDL_Renderer* renderer;
    CA::MetalLayer* swapchain;
    MTL::Device* device;
    NS::SharedPtr<MTL::CommandQueue> queue;
    MTL::ClearColor clearColor = MTL::ClearColor(0.0 / 255.0, 0.0 / 255.0, 0.0 / 255.0, 1.0);

    NS::SharedPtr<MTL::RenderPipelineState> testPipeline;
    NS::SharedPtr<MTL::Texture> testTexture;
    NS::SharedPtr<MTL::Buffer> testPosBuffer;
    NS::SharedPtr<MTL::Buffer> testUVBuffer;
};