#pragma once
#include "renderer.hpp"
#include "rmlui_render.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_stdinc.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <string>
#include <unordered_map>

#include "graphics.hpp"

class Renderer_Metal;
class PrePass;
class TLASBuildPass;
class NormalResolvePass;
class TileCullingPass;
class RaytraceShadowPass;
class RaytraceAOPass;
class SkyAtmospherePass;
class SkyCapturePass;
class IrradianceConvolutionPass;
class PrefilterEnvMapPass;
class BRDFLUTPass;
class MainRenderPass;
class WaterPass;
class PostProcessPass;
class ImGuiPass;
class RmlUiPass;

class RenderPass {
public:
    explicit RenderPass(Renderer_Metal* renderer) : renderer(renderer) {}
    virtual ~RenderPass() = default;
    virtual void execute() = 0;
    virtual const char* getName() const = 0;

    bool enabled = true;

protected:
    Renderer_Metal* renderer;
};

class RenderGraph {
public:
    void addPass(std::unique_ptr<RenderPass> pass) {
        passes.push_back(std::move(pass));
    }

    void execute() {
        for (auto& pass : passes) {
            if (pass->enabled) {
                pass->execute();
            }
        }
    }

    void clear() {
        passes.clear();
    }

private:
    std::vector<std::unique_ptr<RenderPass>> passes;
};


class Renderer_Metal final : public Renderer, public Vapor::RmlUiRenderer { // Must be public or factory function won't work
    friend class PrePass;
    friend class TLASBuildPass;
    friend class NormalResolvePass;
    friend class TileCullingPass;
    friend class RaytraceShadowPass;
    friend class RaytraceAOPass;
    friend class SkyAtmospherePass;
    friend class SkyCapturePass;
    friend class IrradianceConvolutionPass;
    friend class PrefilterEnvMapPass;
    friend class BRDFLUTPass;
    friend class MainRenderPass;
    friend class WaterPass;
    friend class PostProcessPass;
    friend class ImGuiPass;
    friend class RmlUiPass;

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

    // --- RmlUiRenderer interface implementation ---
    void rmluiInit() override;
    void rmluiShutdown() override;
    Uint32 rmluiCreateGeometry(const std::vector<Vapor::RmlUiVertex>& vertices, const std::vector<Uint32>& indices) override;
    void rmluiReleaseGeometry(Uint32 geometryId) override;
    Uint32 rmluiCreateTexture(Uint32 width, Uint32 height, const Uint8* data) override;
    void rmluiReleaseTexture(Uint32 textureId) override;
    void rmluiSetViewport(int width, int height) override;
    void rmluiBeginFrame() override;
    void rmluiRenderGeometry(Uint32 geometryId, const glm::vec2& translation, Uint32 textureId, bool hasTexture) override;
    void rmluiEnableScissor(int x, int y, int width, int height) override;
    void rmluiDisableScissor() override;
    void rmluiEndFrame() override;

protected:
    RenderGraph graph;

    // Per-frame rendering context
    MTL::CommandBuffer* currentCommandBuffer = nullptr;
    std::shared_ptr<Scene> currentScene;
    Camera* currentCamera = nullptr;
    CA::MetalDrawable* currentDrawable = nullptr;

    // Metal device and core resources
    SDL_Renderer* renderer;
    CA::MetalLayer* swapchain;
    MTL::Device* device;
    NS::SharedPtr<MTL::CommandQueue> queue;

    // Pipeline states
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
    NS::SharedPtr<MTL::RenderPipelineState> atmospherePipeline;
    NS::SharedPtr<MTL::RenderPipelineState> skyCapturePipeline;
    NS::SharedPtr<MTL::RenderPipelineState> irradianceConvolutionPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> prefilterEnvMapPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> brdfLUTPipeline;

    // RmlUi rendering pipeline and resources
    NS::SharedPtr<MTL::RenderPipelineState> rmluiTexturedPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> rmluiColorPipeline;
    NS::SharedPtr<MTL::DepthStencilState> rmluiDepthStencilState;
    int rmluiViewportWidth = 0;
    int rmluiViewportHeight = 0;
    bool rmluiScissorEnabled = false;
    int rmluiScissorX = 0, rmluiScissorY = 0;
    int rmluiScissorWidth = 0, rmluiScissorHeight = 0;

    // RmlUi geometry storage
    struct RmlUiGeometry {
        NS::SharedPtr<MTL::Buffer> vertexBuffer;
        NS::SharedPtr<MTL::Buffer> indexBuffer;
        Uint32 indexCount;
    };
    std::unordered_map<Uint32, RmlUiGeometry> rmluiGeometries;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::Texture>> rmluiTextures;
    Uint32 nextRmluiGeometryId = 1;
    Uint32 nextRmluiTextureId = 1;

    // RmlUi render commands for batching
    struct RmlUiRenderCmd {
        Uint32 geometryId;
        glm::vec2 translation;
        Uint32 textureId;
        bool hasTexture;
        bool enableScissor;
        int scissorX, scissorY, scissorWidth, scissorHeight;
    };
    std::vector<RmlUiRenderCmd> rmluiRenderCommands;

    // Water rendering pipeline and resources
    NS::SharedPtr<MTL::RenderPipelineState> waterPipeline;
    NS::SharedPtr<MTL::DepthStencilState> waterDepthStencilState;
    std::vector<NS::SharedPtr<MTL::Buffer>> waterDataBuffers;
    NS::SharedPtr<MTL::Buffer> waterVertexBuffer;
    NS::SharedPtr<MTL::Buffer> waterIndexBuffer;
    Uint32 waterIndexCount = 0;
    bool waterEnabled = true;
    WaterData waterSettings;
    WaterTransform waterTransform;

    // Default textures
    TextureHandle defaultAlbedoTexture;
    TextureHandle defaultNormalTexture;
    TextureHandle defaultORMTexture;
    TextureHandle defaultEmissiveTexture;
    TextureHandle defaultDisplacementTexture;

    // Water textures
    TextureHandle waterNormalMap1;
    TextureHandle waterNormalMap2;
    TextureHandle waterFoamMap;
    TextureHandle waterNoiseMap;
    NS::SharedPtr<MTL::Texture> environmentCubeMap;

    // Per-frame buffers
    std::vector<NS::SharedPtr<MTL::Buffer>> frameDataBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> cameraDataBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> instanceDataBuffers;
    NS::SharedPtr<MTL::Buffer> testStorageBuffer;
    NS::SharedPtr<MTL::Buffer> directionalLightBuffer;
    NS::SharedPtr<MTL::Buffer> pointLightBuffer;
    NS::SharedPtr<MTL::Buffer> materialDataBuffer;
    NS::SharedPtr<MTL::Buffer> atmosphereDataBuffer;
    NS::SharedPtr<MTL::Buffer> iblCaptureDataBuffer;
    std::vector<NS::SharedPtr<MTL::Buffer>> clusterBuffers;

    // IBL textures
    NS::SharedPtr<MTL::Texture> environmentCubemap;      // Captured sky cubemap
    NS::SharedPtr<MTL::Texture> irradianceMap;           // Diffuse irradiance cubemap
    NS::SharedPtr<MTL::Texture> prefilterMap;            // Pre-filtered specular cubemap (with mipmaps)
    NS::SharedPtr<MTL::Texture> brdfLUT;                 // BRDF integration LUT
    bool iblNeedsUpdate = true;                          // Flag to trigger IBL update
    std::vector<NS::SharedPtr<MTL::Buffer>> accelInstanceBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> TLASScratchBuffers;
    std::vector<NS::SharedPtr<MTL::AccelerationStructure>> TLASBuffers;

    // Instance data
    std::vector<InstanceData> instances;
    std::vector<MTL::AccelerationStructureInstanceDescriptor> accelInstances;
    std::unordered_map<std::shared_ptr<Material>, std::vector<std::shared_ptr<Mesh>>> instanceBatches;

    // Render targets
    NS::SharedPtr<MTL::Texture> colorRT_MS;
    NS::SharedPtr<MTL::Texture> colorRT;
    NS::SharedPtr<MTL::Texture> depthStencilRT_MS;
    NS::SharedPtr<MTL::Texture> depthStencilRT;
    NS::SharedPtr<MTL::Texture> normalRT_MS;
    NS::SharedPtr<MTL::Texture> normalRT;
    NS::SharedPtr<MTL::Texture> shadowRT;
    NS::SharedPtr<MTL::Texture> aoRT;

    // Acceleration structures for ray tracing
    std::vector<NS::SharedPtr<MTL::AccelerationStructure>> BLASs;
    NS::SharedPtr<NS::Array> BLASArray;

    // Rendering statistics
    Uint32 currentInstanceCount = 0;
    Uint32 culledInstanceCount = 0;
    Uint32 drawCount = 0;

private:
    // Resource ID counters
    Uint32 nextBufferID = 0;
    Uint32 nextTextureID = 0;
    Uint32 nextPipelineID = 0;
    Uint32 nextInstanceID = 0;
    Uint32 nextMaterialID = 0;

    // Resource handle dicts
    std::unordered_map<Uint32, NS::SharedPtr<MTL::Buffer>> buffers;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::Texture>> textures;
    std::unordered_map<Uint32, NS::SharedPtr<MTL::RenderPipelineState>> pipelines;
    std::unordered_map<std::shared_ptr<Material>, Uint32> materialIDs;

    RenderPath currentRenderPath = RenderPath::Forward;

    void createResources();
};