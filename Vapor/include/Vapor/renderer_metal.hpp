#pragma once
#include "renderer.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>

#include "debug_draw.hpp"
#include "graphics.hpp"

// Forward declarations
namespace Rml {
    class Context;
}

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
class ParticlePass;
class LightScatteringPass;
class VolumetricFogPass;
class VolumetricCloudPass;
class SunFlarePass;
class PostProcessPass;
class BloomBrightnessPass;
class BloomDownsamplePass;
class BloomUpsamplePass;
class BloomCompositePass;
class DOFCoCPass;
class DOFBlurPass;
class DOFCompositePass;
class RmlUiPass;
class ImGuiPass;
class DebugDrawPass;
class CanvasPass;
class WorldCanvasPass;

class RenderPass {
public:
    explicit RenderPass(Renderer_Metal* renderer) : renderer(renderer) {
    }
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


class Renderer_Metal final : public Renderer {// Must be public or factory function won't work
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
    friend class ParticlePass;
    friend class LightScatteringPass;
    friend class VolumetricFogPass;
    friend class VolumetricCloudPass;
    friend class SunFlarePass;
    friend class PostProcessPass;
    friend class BloomBrightnessPass;
    friend class BloomDownsamplePass;
    friend class BloomUpsamplePass;
    friend class BloomCompositePass;
    friend class DOFCoCPass;
    friend class DOFBlurPass;
    friend class DOFCompositePass;
    friend class RmlUiPass;
    friend class ImGuiPass;
    friend class DebugDrawPass;
    friend class CanvasPass;
    friend class WorldCanvasPass;

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

    // Get Metal device (for RmlUI initialization)
    MTL::Device* getDevice() const {
        return device;
    }

    // Initialize UI rendering (sets RenderInterface and finalizes RmlUI initialization)
    bool initUI() override;

    // Debug draw - set the external debug draw queue
    std::shared_ptr<Vapor::DebugDraw> getDebugDraw() override {
        return debugDraw;
    }

    // ===== 2D/3D Batch Rendering API =====
    void flush2D() override;
    void flush3D() override;

    // Quad drawing (2D = screen space, 3D = world space with depth)
    void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) override;
    void drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) override;
    void drawQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    ) override;
    void drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) override;
    void drawQuad2D(
        const glm::mat4& transform,
        TextureHandle texture,
        const glm::vec2* texCoords,
        const glm::vec4& tintColor = glm::vec4(1.0f),
        int entityID = -1
    ) override;
    void drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) override;
    void drawQuad3D(
        const glm::vec3& position,
        const glm::vec2& size,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    ) override;
    void drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) override;
    void drawQuad3D(
        const glm::mat4& transform,
        TextureHandle texture,
        const glm::vec2* texCoords,
        const glm::vec4& tintColor = glm::vec4(1.0f),
        int entityID = -1
    ) override;

    // Rotated quad
    void drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color)
        override;
    void drawRotatedQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        float rotation,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    ) override;

    // Line drawing
    void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness = 1.0f) override;
    void drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness = 1.0f) override;

    // Shape drawing
    void drawRect2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness = 1.0f)
        override;
    void drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) override;
    void drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) override;
    void drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) override;
    void drawTriangleFilled2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color)
        override;

    // Batch statistics
    Batch2DStats getBatch2DStats() const override {
        return batch2DStats;
    }
    void resetBatch2DStats() override {
        batch2DStats = {};
    }

    NS::SharedPtr<MTL::RenderPipelineState>
        createPipeline(const std::string& filename, bool isHDR, bool isColorOnly, Uint32 sampleCount);
    NS::SharedPtr<MTL::ComputePipelineState> createComputePipeline(const std::string& filename);

    TextureHandle createTexture(const std::shared_ptr<Image>& img) override;

    // ===== Font Rendering API =====
    FontHandle loadFont(const std::string& path, float baseSize) override;
    void unloadFont(FontHandle handle) override;
    void drawText2D(
        FontHandle font,
        const std::string& text,
        const glm::vec2& position,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    ) override;
    void drawText3D(
        FontHandle font,
        const std::string& text,
        const glm::vec3& worldPosition,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    ) override;
    glm::vec2 measureText(FontHandle font, const std::string& text, float scale = 1.0f) override;
    float getFontLineHeight(FontHandle font, float scale = 1.0f) override;

    BufferHandle createVertexBuffer(const std::vector<VertexData>& vertices);
    BufferHandle createIndexBuffer(const std::vector<Uint32>& indices);
    BufferHandle createStorageBuffer(const std::vector<VertexData>& vertices);

    NS::SharedPtr<MTL::Buffer> getBuffer(BufferHandle handle) const;
    NS::SharedPtr<MTL::Texture> getTexture(TextureHandle handle) const;
    NS::SharedPtr<MTL::RenderPipelineState> getPipeline(PipelineHandle handle) const;

protected:
    RenderGraph graph;

    // Per-frame rendering context
    MTL::CommandBuffer* currentCommandBuffer = nullptr;
    std::shared_ptr<Scene> currentScene;
    Camera* currentCamera = nullptr;
    CA::MetalDrawable* currentDrawable = nullptr;

    // Metal device and core resources
    SDL_Window* window = nullptr;
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
    NS::SharedPtr<MTL::RenderPipelineState> lightScatteringPipeline;

    // Bloom pipelines
    NS::SharedPtr<MTL::RenderPipelineState> bloomBrightnessPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> bloomDownsamplePipeline;
    NS::SharedPtr<MTL::RenderPipelineState> bloomUpsamplePipeline;
    NS::SharedPtr<MTL::RenderPipelineState> bloomCompositePipeline;

    // DOF (Tilt-Shift) pipelines
    NS::SharedPtr<MTL::RenderPipelineState> dofCoCPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> dofBlurPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> dofCompositePipeline;

    // Debug draw pipeline and resources
    NS::SharedPtr<MTL::RenderPipelineState> debugDrawPipeline;
    NS::SharedPtr<MTL::DepthStencilState> debugDrawDepthStencilState;
    std::vector<NS::SharedPtr<MTL::Buffer>> debugDrawVertexBuffers;// Per-frame buffers
    std::shared_ptr<Vapor::DebugDraw> debugDraw = nullptr;

    // 2D Batch rendering pipeline and resources
    NS::SharedPtr<MTL::RenderPipelineState> batch2DPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> batch2DPipelineAdditive;
    NS::SharedPtr<MTL::RenderPipelineState> batch2DPipelineMultiply;
    NS::SharedPtr<MTL::DepthStencilState> batch2DDepthStencilState;// No depth test
    NS::SharedPtr<MTL::DepthStencilState> batch2DDepthStencilStateEnabled;// With depth test
    std::vector<NS::SharedPtr<MTL::Buffer>> batch2DVertexBuffers;// Per-frame triple-buffered
    std::vector<NS::SharedPtr<MTL::Buffer>> batch2DIndexBuffers;// Per-frame triple-buffered
    std::vector<NS::SharedPtr<MTL::Buffer>> batch2DUniformBuffers;// Per-frame triple-buffered

    // 3D Batch buffers (to avoid overwriting 2D buffers in the same frame)
    std::vector<NS::SharedPtr<MTL::Buffer>> batch3DVertexBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> batch3DIndexBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> batch3DUniformBuffers;
    NS::SharedPtr<MTL::Texture> batch2DWhiteTexture;// 1x1 white texture
    TextureHandle batch2DWhiteTextureHandle;

    // Batch constants
    static constexpr Uint32 BatchMaxQuads = 20000;
    static constexpr Uint32 BatchMaxVertices = BatchMaxQuads * 4;
    static constexpr Uint32 BatchMaxIndices = BatchMaxQuads * 6;
    static constexpr Uint32 BatchMaxTextureSlots = 16;

    // 2D Batch CPU-side state (screen space, no depth)
    std::vector<Batch2DVertex> batch2DVertices;
    std::vector<Uint32> batch2DIndices;
    std::array<TextureHandle, 16> batch2DTextureSlots;
    Uint32 batch2DTextureSlotIndex = 1;// 0 = white texture
    glm::mat4 batch2DProjection = glm::mat4(1.0f);
    BlendMode batch2DBlendMode = BlendMode::Alpha;
    Batch2DStats batch2DStats;
    bool batch2DActive = false;

    // 3D Batch CPU-side state (world space, with depth)
    std::vector<Batch2DVertex> batch3DVertices;
    std::vector<Uint32> batch3DIndices;
    std::array<TextureHandle, 16> batch3DTextureSlots;
    Uint32 batch3DTextureSlotIndex = 1;
    glm::mat4 batch3DProjection = glm::mat4(1.0f);
    BlendMode batch3DBlendMode = BlendMode::Alpha;
    Batch2DStats batch3DStats;
    bool batch3DActive = false;

    // Pre-computed quad positions and UVs
    glm::vec4 batchQuadPositions[4];
    glm::vec2 batchQuadTexCoords[4];

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

    // Particle system
    static constexpr Uint32 MAX_PARTICLES = 1000;// Reduced for debugging
    bool particleSystemEnabled = true;
    Uint32 particleCount = MAX_PARTICLES;

    NS::SharedPtr<MTL::ComputePipelineState> particleForcePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> particleIntegratePipeline;
    NS::SharedPtr<MTL::RenderPipelineState> particleRenderPipeline;
    NS::SharedPtr<MTL::DepthStencilState> particleDepthStencilState;

    // Single particle buffer (persistent state, not triple-buffered)
    NS::SharedPtr<MTL::Buffer> particleBuffer;
    // Per-frame uniform buffers (triple-buffered)
    std::vector<NS::SharedPtr<MTL::Buffer>> particleSimParamsBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> particleAttractorBuffers;

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

    // Light scattering (God Rays) resources
    std::vector<NS::SharedPtr<MTL::Buffer>> lightScatteringDataBuffers;
    NS::SharedPtr<MTL::Texture> lightScatteringRT;// Half-resolution scattering texture
    bool lightScatteringEnabled = true;
    LightScatteringData lightScatteringSettings;

    // Volumetric Fog resources
    NS::SharedPtr<MTL::ComputePipelineState> fogFroxelInjectionPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> fogScatteringIntegrationPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> fogApplyPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> fogSimplePipeline;
    std::vector<NS::SharedPtr<MTL::Buffer>> volumetricFogDataBuffers;
    NS::SharedPtr<MTL::Texture> fogFroxelGrid;// 3D froxel data texture
    NS::SharedPtr<MTL::Texture> fogIntegratedVolume;// 3D integrated scattering
    bool volumetricFogEnabled = true;
    VolumetricFogData volumetricFogSettings;

    // Volumetric Cloud resources
    NS::SharedPtr<MTL::RenderPipelineState> cloudRenderPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> cloudLowResPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> cloudTemporalResolvePipeline;
    NS::SharedPtr<MTL::RenderPipelineState> cloudCompositePipeline;
    std::vector<NS::SharedPtr<MTL::Buffer>> volumetricCloudDataBuffers;
    NS::SharedPtr<MTL::Texture> cloudRT;// Cloud render target (quarter res)
    NS::SharedPtr<MTL::Texture> cloudHistoryRT;// Previous frame clouds (for TAA)
    bool volumetricCloudsEnabled = false;
    VolumetricCloudData volumetricCloudSettings;

    // Sun Flare resources
    NS::SharedPtr<MTL::RenderPipelineState> sunFlarePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> sunOcclusionPipeline;
    std::vector<NS::SharedPtr<MTL::Buffer>> sunFlareDataBuffers;
    NS::SharedPtr<MTL::Buffer> sunVisibilityBuffer;// Single float for occlusion result
    bool sunFlareEnabled = false;
    SunFlareData sunFlareSettings;

    // IBL textures
    NS::SharedPtr<MTL::Texture> environmentCubemap;// Captured sky cubemap
    NS::SharedPtr<MTL::Texture> irradianceMap;// Diffuse irradiance cubemap
    NS::SharedPtr<MTL::Texture> prefilterMap;// Pre-filtered specular cubemap (with mipmaps)
    NS::SharedPtr<MTL::Texture> brdfLUT;// BRDF integration LUT
    bool iblNeedsUpdate = true;// Flag to trigger IBL update
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
    NS::SharedPtr<MTL::Texture> tempColorRT;// For ping-pong post-processing (fog, clouds)
    NS::SharedPtr<MTL::Texture> depthStencilRT_MS;
    NS::SharedPtr<MTL::Texture> depthStencilRT;
    NS::SharedPtr<MTL::Texture> normalRT_MS;
    NS::SharedPtr<MTL::Texture> normalRT;
    NS::SharedPtr<MTL::Texture> shadowRT;
    NS::SharedPtr<MTL::Texture> aoRT;

    // Bloom render targets
    NS::SharedPtr<MTL::Texture> bloomBrightnessRT;// Half-res brightness extraction
    std::vector<NS::SharedPtr<MTL::Texture>> bloomPyramidRTs;// Mipmap pyramid for bloom (5 levels)
    NS::SharedPtr<MTL::Texture> bloomResultRT;// Final bloom result
    static constexpr Uint32 BLOOM_PYRAMID_LEVELS = 5;
    float bloomThreshold = 1.0f;
    float bloomStrength = 0.8f;

    // DOF (Tilt-Shift) render targets
    NS::SharedPtr<MTL::Texture> dofCoCRT;// Color + CoC in alpha
    NS::SharedPtr<MTL::Texture> dofBlurRT;// Blurred result
    NS::SharedPtr<MTL::Texture> dofResultRT;// Final DOF composite

    // DOF parameters (Octopath Traveler tilt-shift style)
    struct DOFParams {
        float focusCenter = 0.5f;// Y position of focus band center (0-1)
        float focusWidth = 0.15f;// Width of the in-focus band
        float focusFalloff = 0.8f;// How quickly blur increases outside focus
        float maxBlur = 1.0f;// Maximum blur intensity (0-1)
        float tiltAngle = 0.0f;// Tilt angle in radians (0 = horizontal)
        float bokehRoundness = 0.8f;// Bokeh shape: 0 = hexagonal, 1 = circular
        float blendSharpness = 0.3f;// Transition sharpness
        int sampleCount = 32;// Blur quality (8-64)
    } dofParams;

    // Post-processing parameters
    struct PostProcessParams {
        // Chromatic Aberration
        float chromaticAberrationStrength = 0.01f;// RGB offset strength
        float chromaticAberrationFalloff = 2.0f;// Edge falloff power

        // Vignette
        float vignetteStrength = 0.3f;// Darkening intensity
        float vignetteRadius = 0.8f;// Start radius (0-1)
        float vignetteSoftness = 0.5f;// Transition softness

        // Color Grading
        float saturation = 1.0f;// Color saturation (0-2)
        float contrast = 1.0f;// Contrast (0-2)
        float brightness = 0.0f;// Brightness offset (-1 to 1)
        float temperature = 0.0f;// Color temperature shift (-1 to 1)
        float tint = 0.0f;// Green-magenta tint (-1 to 1)

        // Tone Mapping
        float exposure = 1.0f;// Exposure multiplier
    } postProcessParams;

    // Acceleration structures for ray tracing
    std::vector<NS::SharedPtr<MTL::AccelerationStructure>> BLASs;
    NS::SharedPtr<NS::Array> BLASArray;

    // Rendering statistics
    Uint32 currentInstanceCount = 0;
    Uint32 culledInstanceCount = 0;
    Uint32 drawCount = 0;

    // LOD statistics
    bool lodEnabled = true;  // Enable/disable LOD selection
    Uint32 lodTrianglesRendered = 0;
    Uint32 lodTrianglesOriginal = 0;
    std::array<Uint32, 8> lodLevelCounts = {}; // Count of meshes at each LOD level

private:
    // Internal batch management
    void beginBatch2D();
    void endBatch2D();
    void beginBatch3D();
    void endBatch3D();

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

    // UI rendering (using void* for pimpl idiom to hide implementation)
    void* m_uiRenderer = nullptr;
    Rml::Context* m_uiContext = nullptr;

    // Font rendering
    FontManager m_fontManager;

    void createResources();
    void renderUI();// Internal method called by RmlUiPass
};