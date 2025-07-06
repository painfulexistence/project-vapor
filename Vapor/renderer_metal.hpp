#pragma once
#include "renderer.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_stdinc.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <string>
#include <unordered_map>

#include "graphics.hpp"

class Renderer_Metal final : public Renderer { // Must be public or factory function won't work
public:
    Renderer_Metal();

    ~Renderer_Metal();

    virtual void init(SDL_Window* window) override;

    virtual void deinit() override;

    virtual void stage(std::shared_ptr<Scene> scene) override;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) override;

    virtual void setRenderPath(RenderPath path) override {
        currentRenderPath = path;
    }
    virtual RenderPath getRenderPath() const override {
        return currentRenderPath;
    }

    NS::SharedPtr<MTL::RenderPipelineState> createPipeline(const std::string& filename, bool isHDR, bool isColorOnly, Uint32 sampleCount);
    NS::SharedPtr<MTL::ComputePipelineState> createComputePipeline(const std::string& filename);

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

    NS::SharedPtr<MTL::DepthStencilState> depthStencilState;
    NS::SharedPtr<MTL::RenderPipelineState> prePassPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> drawPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> postProcessPipeline;

    NS::SharedPtr<MTL::ComputePipelineState> buildClustersPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> cullLightsPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> tileCullingPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> normalResolvePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> raytraceShadowPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> raytraceAOPipeline;

    TextureHandle defaultAlbedoTexture;
    TextureHandle defaultNormalTexture;
    TextureHandle defaultORMTexture;
    TextureHandle defaultEmissiveTexture;
    TextureHandle defaultDisplacementTexture;

    std::vector<NS::SharedPtr<MTL::Buffer>> frameDataBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> cameraDataBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> instanceDataBuffers;
    NS::SharedPtr<MTL::Buffer> testStorageBuffer;
    NS::SharedPtr<MTL::Buffer> directionalLightBuffer;
    NS::SharedPtr<MTL::Buffer> pointLightBuffer;
    NS::SharedPtr<MTL::Buffer> materialDataBuffer;
    std::vector<NS::SharedPtr<MTL::Buffer>> clusterBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> accelInstanceBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> TLASScratchBuffers;
    std::vector<NS::SharedPtr<MTL::AccelerationStructure>> TLASBuffers;

    std::vector<InstanceData> instances;
    std::vector<MTL::AccelerationStructureInstanceDescriptor> accelInstances;
    std::unordered_map<std::shared_ptr<Material>, std::vector<std::shared_ptr<Mesh>>> instanceBatches;

    NS::SharedPtr<MTL::Texture> colorRT_MS;
    NS::SharedPtr<MTL::Texture> colorRT;
    NS::SharedPtr<MTL::Texture> depthStencilRT_MS;
    NS::SharedPtr<MTL::Texture> depthStencilRT;
    NS::SharedPtr<MTL::Texture> normalRT_MS;
    NS::SharedPtr<MTL::Texture> normalRT;
    NS::SharedPtr<MTL::Texture> shadowRT;
    NS::SharedPtr<MTL::Texture> aoRT;

    Uint32 nextBufferID = 0;
    Uint32 nextTextureID = 0;
    Uint32 nextPipelineID = 0;
    Uint32 nextInstanceID = 0;
    Uint32 nextMaterialID = 0;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::Buffer>> buffers;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::Texture>> textures;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::RenderPipelineState>> pipelines;
    std::vector<NS::SharedPtr<MTL::AccelerationStructure>> BLASs;
    NS::SharedPtr<NS::Array> BLASArray;

    std::unordered_map<std::shared_ptr<Material>, Uint32> materialIDs;

    RenderPath currentRenderPath = RenderPath::Forward;

    Uint32 currentInstanceCount = 0;
    Uint32 culledInstanceCount = 0;
    Uint32 drawCount = 0;

    void createResources();
};