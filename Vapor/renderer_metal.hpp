#pragma once
#include "renderer.hpp"
#include "graphics.hpp"

#include "SDL3/SDL.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <string>

class Renderer_Metal final : Renderer {
public:
    Renderer_Metal(SDL_Window* window);

    ~Renderer_Metal();

    virtual void init() override;

    virtual void draw() override;

    void initTestPipelines();

    NS::SharedPtr<MTL::RenderPipelineState> createPipeline(const std::string& filename);

    NS::SharedPtr<MTL::Texture> createTexture(const std::string& filename);

    NS::SharedPtr<MTL::Buffer> createVertexBuffer(std::vector<VertexData> vertices);

    NS::SharedPtr<MTL::Buffer> createIndexBuffer(std::vector<uint16_t> indices);

private:
    SDL_Renderer* renderer;
    CA::MetalLayer* swapchain;
    MTL::Device* device;
    NS::SharedPtr<MTL::CommandQueue> queue;
    MTL::ClearColor clearColor = MTL::ClearColor(0.0 / 255.0, 0.0 / 255.0, 0.0 / 255.0, 1.0);
    NS::UInteger sampleCount = 4;
    NS::UInteger numMaxInstances = 1; // TODO: change this to a bigger number

    NS::SharedPtr<MTL::DepthStencilState> depthStencilState;
    NS::SharedPtr<MTL::RenderPipelineState> testDrawPipeline;

    NS::SharedPtr<MTL::Texture> testAlbedoTexture;
    NS::SharedPtr<MTL::Texture> testNormalTexture;
    NS::SharedPtr<MTL::Texture> testAOTexture;
    NS::SharedPtr<MTL::Texture> testRoughnessTexture;
    NS::SharedPtr<MTL::Texture> testMetallicTexture;

    NS::SharedPtr<MTL::Buffer> testVertexBuffer;
    NS::SharedPtr<MTL::Buffer> testIndexBuffer;

    NS::SharedPtr<MTL::Buffer> instanceDataBuffer;
    NS::SharedPtr<MTL::Buffer> cameraDataBuffer;
    NS::SharedPtr<MTL::Texture> depthStencilTexture;
    NS::SharedPtr<MTL::Texture> msaaTexture;
};