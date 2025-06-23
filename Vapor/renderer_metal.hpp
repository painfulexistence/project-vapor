#pragma once
#include "renderer.hpp"

#include <SDL3/SDL.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <string>
#include <unordered_map>

#include "graphics.hpp"

class Renderer_Metal final : public Renderer { // Must be public or factory function won't work
public:
    Renderer_Metal(SDL_Window* window);

    ~Renderer_Metal();

    virtual void init() override;

    virtual void stage(std::shared_ptr<Scene> scene) override;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) override;

    void initTestPipelines();

    NS::SharedPtr<MTL::RenderPipelineState> createPipeline(const std::string& filename);

    TextureHandle createTexture(const std::shared_ptr<Image>& img);

    BufferHandle createVertexBuffer(const std::vector<VertexData>& vertices);
    BufferHandle createIndexBuffer(const std::vector<Uint32>& indices);
    BufferHandle createStorageBuffer(const std::vector<VertexData>& vertices);

    NS::SharedPtr<MTL::Buffer> getBuffer(BufferHandle handle) const;
    NS::SharedPtr<MTL::Texture> getTexture(TextureHandle handle) const;
    NS::SharedPtr<MTL::RenderPipelineState> getPipeline(PipelineHandle handle) const;

private:
    SDL_Renderer* renderer;
    CA::MetalLayer* swapchain;
    MTL::Device* device;
    NS::SharedPtr<MTL::CommandQueue> queue;
    NS::UInteger sampleCount = 4;
    NS::UInteger numMaxInstances = 1; // TODO: change this to a bigger number

    NS::SharedPtr<MTL::DepthStencilState> depthStencilState;
    NS::SharedPtr<MTL::RenderPipelineState> testDrawPipeline;

    TextureHandle defaultAlbedoTexture;
    TextureHandle defaultNormalTexture;
    TextureHandle defaultORMTexture;
    TextureHandle defaultEmissiveTexture;
    TextureHandle defaultDisplacementTexture;

    NS::SharedPtr<MTL::Buffer> instanceDataBuffer;
    NS::SharedPtr<MTL::Buffer> cameraDataBuffer;
    NS::SharedPtr<MTL::Buffer> testStorageBuffer;
    NS::SharedPtr<MTL::Buffer> directionalLightBuffer;
    NS::SharedPtr<MTL::Buffer> pointLightBuffer;

    NS::SharedPtr<MTL::Texture> depthStencilTexture;
    NS::SharedPtr<MTL::Texture> msaaTexture;

    Uint32 nextBufferID = 1;
    Uint32 nextTextureID = 1;
    Uint32 nextPipelineID = 1;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::Buffer>> buffers;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::Texture>> textures;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::RenderPipelineState>> pipelines;
};