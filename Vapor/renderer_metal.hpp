#pragma once
#include "renderer.hpp"

#include "SDL.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <string>

class Renderer_Metal final : Renderer {
public:
    Renderer_Metal(SDL_Window* window);

    ~Renderer_Metal();

    virtual void init();

    virtual void draw();

    void initTestBuffer();

    NS::SharedPtr<MTL::Texture> createTexture(const std::string& filename);

    void initTestPipeline();

private:
    SDL_Renderer* renderer;
    CA::MetalLayer* swapchain;
    MTL::Device* device;
    NS::SharedPtr<MTL::CommandQueue> queue;
    MTL::ClearColor clearColor = MTL::ClearColor(0.0 / 255.0, 0.0 / 255.0, 0.0 / 255.0, 1.0);
    NS::UInteger sampleCount = 4;

    NS::SharedPtr<MTL::DepthStencilState> depthStencilState;
    NS::SharedPtr<MTL::RenderPipelineState> testDrawPipeline;
    NS::SharedPtr<MTL::Texture> testAlbedoTexture;
    NS::SharedPtr<MTL::Texture> testNormalTexture;
    NS::SharedPtr<MTL::Buffer> testPosBuffer;
    NS::SharedPtr<MTL::Buffer> testUVBuffer;
    NS::SharedPtr<MTL::Buffer> testCubeVertexBuffer;
    NS::SharedPtr<MTL::Buffer> testCubeIndexBuffer;
    NS::SharedPtr<MTL::Buffer> testCubeInstanceBuffer;
    NS::SharedPtr<MTL::Buffer> cameraDataBuffer;
    NS::SharedPtr<MTL::Texture> msaaTexture;
};