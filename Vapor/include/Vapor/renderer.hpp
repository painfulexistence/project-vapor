#pragma once
#include "irenderer.hpp"
#include "rhi.hpp"
#include "render_data.hpp"
#include "render_graph.hpp"
#include "camera.hpp"
#include "graphics.hpp"
#include "font_manager.hpp"
#include "render_scene.hpp"
#include <SDL3/SDL_video.h>
#include <entt/entt.hpp>
#include <functional>
#include <unordered_map>
#include <vector>
#include <memory>

// Forward declarations
namespace Rml {
    class Context;
}

namespace Vapor {
    class DebugDraw;
}

// Batch rendering stats for the RHI renderer. (graphics_batch2d.hpp has a
// richer RHIBatch2DStats but it cannot be included here — it redefines BlendMode,
// which rhi.hpp also defines. getBatch2DStats() is not polymorphic.)
struct RHIBatch2DStats {
    uint32_t drawCalls = 0;
    uint32_t quadCount = 0;
    uint32_t vertexCount = 0;
};

// ============================================================================
// Renderer - High-level renderer that uses RHI (implements IRenderer)
//
// This is the RHI-backed renderer used by the Vulkan backend. The shared
// backend-agnostic types (GraphicsBackend, RenderPath, GpuImageData,
// RenderTextureDesc/Handle) and the polymorphic interface live in irenderer.hpp.
//
// Responsibilities:
// - Manage rendering resources (meshes, materials, textures)
// - Collect drawables each frame
// - Perform culling and sorting
// - Execute draw calls via RHI
// - Multi-pass rendering (pre-pass, compute passes, main draw, post-process)
// - Clustered lighting and ray tracing
// ============================================================================

class Renderer : public IRenderer {
public:
    Renderer() = default;
    ~Renderer() override = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    // Initialize with RHI ownership
    // Note: Use createRenderer() factory function instead of calling initialize() directly
    void initialize(std::unique_ptr<RHI> rhiPtr, GraphicsBackend backend);
    void shutdown() override;

    // ========================================================================
    // Resource Registration (called during scene loading/staging)
    // ========================================================================

    // Register a mesh and return its ID.
    MeshId registerMesh(const std::vector<Vapor::VertexData>& vertices,
                        const std::vector<Uint32>& indices);

    // Register a material and return its ID
    MaterialId registerMaterial(const MaterialDataInput& materialData);

    // Register a texture and return its ID
    TextureId registerTexture(const std::shared_ptr<Vapor::Image>& image);

    // ========================================================================
    // Frame Rendering
    // ========================================================================

    // Begin a frame with camera data
    void beginFrame(const CameraRenderData& camera) override;

    // Submit a drawable to be rendered this frame
    void submitDrawable(const Drawable& drawable);

    // Submit lights
    void submitDirectionalLight(const DirectionalLightData& light);
    void submitPointLight(const PointLightData& light);

    // Execute rendering: culling, sorting, then the RenderGraph passes
    void render();

    // End the frame
    void endFrame() override;

    // ========================================================================
    // Render Graph / Capabilities
    // ========================================================================

    // The frame's pass list. Gameplay code may append CallbackPass lambdas,
    // insert custom RenderPass subclasses, remove built-ins, or toggle
    // passes. Passes whose PassFlags requirements the active backend cannot
    // satisfy (e.g. raytracing on Vulkan) are skipped automatically.
    RenderGraph& getRenderGraph() { return renderGraph; }

    // Feature support of the active backend (raytracing, compute, ...).
    const RHICapabilities& getCapabilities() const { return capabilities; }

    // ========================================================================
    // Render Path Management
    // ========================================================================

    void setRenderPath(RenderPath path) override { currentRenderPath = path; }
    RenderPath getRenderPath() const override { return currentRenderPath; }

    // ========================================================================
    // Scene/ECS Integration
    // ========================================================================

    // Stage a scene (upload meshes, materials, textures)
    void stage(std::shared_ptr<RenderScene> scene) override;

    // Draw a scene with Scene object
    void draw(std::shared_ptr<RenderScene> scene, Camera& camera) override;

    // Draw with ECS registry
    void draw(entt::registry& registry, std::shared_ptr<RenderScene> scene, Camera& camera) override;

    // ========================================================================
    // Screenshot API
    // ========================================================================

    void readPixelsAsync(ScreenshotCallback callback) override;

    // ========================================================================
    // UI Integration
    // ========================================================================

    // Initialize RmlUI rendering
    bool initUI() override;

    // SDL window (logical-size queries for RmlUI/HiDPI); set by createRenderer.
    void setWindow(SDL_Window* w) { window = w; }

    // Get debug draw interface
    std::shared_ptr<Vapor::DebugDraw> getDebugDraw() override;

    // Set ImGui callback (called between NewFrame and Render)
    void setImGuiCallback(std::function<void()> callback) override;
    // Drives the engine ImGui frame: F1 visibility toggle, the always-on frame
    // callback (recording/F2), then — if visible — the app callback and the
    // Engine window (with the engine-window callback). Call after ImGui::NewFrame().
    void invokeImGuiCallback() override;

    // ========================================================================
    // 2D/3D Batch Rendering API
    // ========================================================================

    // setEngineWindowCallback / setImGuiFrameCallback / isImGuiVisible /
    // setImGuiVisible and the m_* callback members are inherited from IRenderer.

    // Upload RGBA pixel data as the video texture sampled by rect lights marked
    // with useVideoTexture = true. Call once per frame after VideoPlayer::update().
    // TODO(RHI): implement via RHI texture upload once rect-light shading is ported.
    void uploadRectLightVideoTexture(const uint8_t* /*rgba*/, uint32_t /*width*/, uint32_t /*height*/) override {}

    // Manual flush (for controlling draw order)
    void flush2D() override;
    void flush3D() override;

    // Quad drawing (2D)
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

    // Quad drawing (3D - world space with depth)
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

    // Rotated quad (2D)
    void drawRotatedQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        float rotation,
        const glm::vec4& color
    ) override;
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
    void drawRect2D(
        const glm::vec2& position,
        const glm::vec2& size,
        const glm::vec4& color,
        float thickness = 1.0f
    ) override;
    void drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) override;
    void drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) override;
    void drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) override;
    void drawTriangleFilled2D(
        const glm::vec2& p0,
        const glm::vec2& p1,
        const glm::vec2& p2,
        const glm::vec4& color
    ) override;

    // Batch statistics
    RHIBatch2DStats getBatch2DStats() const;
    void resetBatch2DStats();

    // ========================================================================
    // Font Rendering API
    // ========================================================================

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

    // ========================================================================
    // Render-to-Texture API
    // ========================================================================

    RenderTextureHandle createRenderTexture(const RenderTextureDesc& desc) override;
    void destroyRenderTexture(RenderTextureHandle handle) override;
    TextureHandle getRenderTextureAsTexture(RenderTextureHandle handle) override;
    void renderToTexture(
        RenderTextureHandle target,
        std::shared_ptr<RenderScene> scene,
        Camera& camera,
        const glm::vec4& clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)
    ) override;
    glm::uvec2 getRenderTextureSize(RenderTextureHandle handle) override;
    Uint64 registerRenderTextureForUI(RenderTextureHandle handle) override;

    // ========================================================================
    // Post-Processing API
    // ========================================================================

    void applyBloom(RenderTextureHandle target, float threshold = 1.0f, float strength = 0.5f) override;
    void applyToneMapping(RenderTextureHandle target, float exposure = 1.0f) override;
    void applyVignette(RenderTextureHandle target, float strength = 0.3f, float radius = 0.8f) override;

    // ========================================================================
    // ECS Particle Integration API
    // ========================================================================

    uint32_t claimParticleSlots(uint32_t count) override;
    void releaseParticleSlots(uint32_t slotBegin, uint32_t count) override;
    void uploadParticles(uint32_t slotBegin,
                         const std::vector<GPUParticleData>& particles) override;
    void setParticleForceField(const ParticleForceField& field) override;
    void setParticleSimPaused(bool paused) override { m_particleSimPaused = paused; }
    void setParticleVisible(bool visible) override { particleVisible = visible; }
    void setParticleDrawList(const std::vector<ParticleDrawPacket>& draws) override;

    // ========================================================================
    // Texture Creation (for sprites/batch rendering)
    // ========================================================================

    TextureHandle createTexture(const std::shared_ptr<Vapor::Image>& img) override;

    // Update an existing texture's contents in place. The image dimensions and
    // channel count must match those used when the texture was created.
    // Intended for streaming sources (e.g. video playback) that re-upload pixel
    // data every frame without reallocating the GPU texture.
    void updateTexture(TextureHandle handle, const std::shared_ptr<Vapor::Image>& img) override;

    // ========================================================================
    // Volume Rendering API (EmberGen density grids)
    // ========================================================================

    // Upload a raw single-channel density grid (width*height*depth bytes,
    // R8_UNORM, tightly packed, slice-major) as a 3D texture — the hook the
    // EmberGen import PR will call with decoded voxel data.
    TextureHandle createVolumeTexture(Uint32 width, Uint32 height, Uint32 depth,
                                      const void* data, size_t size);
    // Point the volume raymarch pass at a density grid with world-space AABB
    // bounds. Pass an invalid handle to fall back to the procedural test grid.
    void setVolumeDensity(TextureHandle density, const glm::vec3& boxMin,
                          const glm::vec3& boxMax);

    // ========================================================================
    // Getters
    // ========================================================================

    RHI* getRHI() const { return rhi.get(); }

    // Stats
    Uint32 getDrawCount() const { return drawCount; }
    Uint32 getCurrentInstanceCount() const { return currentInstanceCount; }
    Uint32 getCulledInstanceCount() const { return culledInstanceCount; }

    // GPU-driven rendering: replace CPU frustum culling + per-object drawIndexed
    // with a compute cull pass that fills an indirect-args buffer, consumed by
    // per-object drawIndexedIndirect. Off by default; when off the existing CPU
    // path is used unchanged.
    void setGpuDrivenCulling(bool enabled) { gpuDrivenMode = enabled ? GpuDrivenMode::Indirect : GpuDrivenMode::Off; }
    bool isGpuDrivenCulling() const { return gpuDrivenIndirect(); }

    // Load an equirectangular HDR image as the IBL environment (see IBLSource).
    void loadHDRI(const std::string& path) override;

private:
    // ========================================================================
    // Internal Rendering Steps
    // ========================================================================

    void performCulling();
    void setupDefaultRenderGraph();
    void drawGpuTimingsImGui();
    void drawCpuTimingsImGui();  // per-pass CPU (command-recording) time
    void drawRenderGraphImGui();
    void sortDrawables();
    void updateBuffers();
    void createDefaultResources();
    void createRenderPipeline();
    void createRenderTargets();
    // Destroy every swapchain-sized target so createRenderTargets() can be
    // re-run after a resize (destruction is deferred inside the backends).
    void destroyRenderTargets();
    Uint32 lastRTWidth = 0, lastRTHeight = 0;  // resize detection
    void createComputePipelines();

    // Multi-pass rendering
    void buildAccelerationStructures();
    void updateFrameData();
    void prePass();
    void normalResolvePass();
    void clusterBuildPass();
    void tileCullingPass();
    void gpuCullPass();  // GPU-driven frustum (+ Hi-Z) cull -> gpuCullArgsBuffer
    void prePassCullPass();  // frustum-only cull before the pre-pass -> prepassCullArgsBuffer
    // Shared cull dispatch: writes one DrawCommand per instance into argsBuffer,
    // optionally applying the Hi-Z occlusion test. gpuCullPass() and
    // prePassCullPass() are thin wrappers (main = frustum+Hi-Z, pre = frustum only).
    void runGpuCull(BufferHandle argsBuffer, bool enableOcclusion);
    void raytraceShadowPass();
    void raytraceAOPass();
    void mainRenderPass();
    void postProcessPass();
    void bloomBrightnessPass();
    void bloomDownsamplePass();
    void bloomUpsamplePass();
    void skyAtmospherePass();
    void lightScatteringPass();
    void volumetricFogPass();
    void volumeRaymarchPass();
    void velocityPass();
    void particlePass();
    void volumetricCloudPass();

    // RmlUI on the RHI (cross-backend): initUI() (declared above with the
    // IRenderer overrides) creates an RmlRendererRHI and registers it as Rml's
    // render interface; renderUI() draws the context at the end of the graph.
    void renderUI();
    void shadowPass();
    void sscsPass();  // screen-space contact shadows (Vulkan fullscreen frag)

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    Frustum extractFrustum(const glm::mat4& viewProj);
    TextureId getOrCreateTexture(const std::shared_ptr<Vapor::Image>& image);
    void bindMaterial(MaterialId materialId);

    // Scene/ECS helpers
    void collectDrawables(std::shared_ptr<RenderScene> scene);
    void collectDrawables(entt::registry& registry, std::shared_ptr<RenderScene> scene);
    // Submit the scene's gathered lights (filled by the game's light systems)
    void submitSceneLights(const std::shared_ptr<RenderScene>& scene);

    // Batch rendering helpers
    void initBatchRendering();
    void shutdownBatchRendering();

    // Registers the renderer-side StatsLog sources ("R"/"RT"/"CULL"). Called
    // from initialize(); driven once per frame by StatsLog::tick() in beginFrame.
    void registerStatsSources();

    // Cached debug-preview texture views (grayscale swizzle / single array
    // layer) for the ImGui panels. Rebuilt when the source RT id changes (a
    // resize recreates the RTs), keyed by a caller-supplied label.
    struct PreviewView { TextureHandle view; Uint32 srcId = 0; };
    std::unordered_map<std::string, PreviewView> previewViews;
    TextureHandle debugView(const char* key, TextureHandle src,
                            TextureSwizzle swizzle, Uint32 layer);

    // Post-processing helpers
    void initPostProcessing();
    void shutdownPostProcessing();

    // ========================================================================
    // RHI Ownership
    // ========================================================================

    std::unique_ptr<RHI> rhi;

    // ========================================================================
    // Backend Info
    // ========================================================================

    GraphicsBackend backend;  // Store backend for ImGui shutdown

    // ========================================================================
    // Render Graph
    // ========================================================================

    RenderGraph renderGraph;
    RHICapabilities capabilities;  // copied from the RHI at initialize()

    // ========================================================================
    // Full PBR shader contract (matches 3d_pbr_normal_mapped.metal)
    // ------------------------------------------------------------------------
    // The shader consumes clusters, rect lights, PSSM cascades and a set of
    // shadow/AO/IBL textures. Until the corresponding passes are ported to
    // the RHI renderer these are bound with NEUTRAL defaults (white shadow =
    // unshadowed, black IBL = no contribution, +inf cascade splits = RT-shadow
    // branch) so the shader runs correctly with every binding satisfied.
    // ========================================================================

    BufferHandle rectLightBuffer;    // fragment buffer(7)
    BufferHandle pssmDataBuffer;     // PSSM cascade data (Vulkan: set1 b2)
    BufferHandle atmosphereDataBuffer;  // sky/atmosphere params (Vulkan: set1 b0 in sky pass)
    AtmosphereRenderData atmosphereData;  // CPU copy (sun direction for god rays, etc.)
    BufferHandle lightScatteringDataBuffer;  // god-ray params (Vulkan: set1 b0 in LS pass)
    Uint32 frameCounter = 0;  // for temporal jitter
    TextureHandle defaultBlackCubemapTex;   // IBL irradiance/prefilter default
    TextureHandle pssmShadowArrayTexture;   // 3-cascade depth array (Vulkan: set2 b6)
    TextureHandle nearShadowMap;            // independent near-field depth map [near, pssmRTMaxDist] (Vulkan: set2 b9)
    static constexpr Uint32 NEAR_SHADOW_MAP_SIZE = Vapor::kNearShadowMapSize;  // shared (irenderer.hpp)
    std::vector<Vapor::RectLight> rectLights;   // gathered from the scene
    std::vector<Vapor::SpotLight> spotLights;   // gathered from the scene
    BufferHandle spotLightBuffer;               // frame-slotted, maxSpotLights

    // ImGui texture previews (RT viewer / material thumbnails)
    void* getImGuiTextureID(TextureHandle handle);
    std::unordered_map<Uint32, void*> imguiTextureCache;  // Vulkan descriptor sets
    void drawGraphicsImGui();

    // Last completed frame's numbers, shown in the Engine window. The panel
    // draws BETWEEN beginFrame (which clears the live per-frame light vectors)
    // and draw() (which refills them), so it must read this snapshot — reading
    // the live vectors there always shows 0.
    struct FrameStats {
        Uint32 totalDrawables = 0;
        Uint32 visibleDrawables = 0;
        Uint32 directionalLights = 0;
        Uint32 pointLights = 0;
        Uint32 rectLights = 0;
        Uint32 spotLights = 0;
        // Main-pass geometry submissions this frame. The clearest "is MDI/GPU-
        // driven actually engaged" signal: CPU/per-object issue ~one draw per
        // object, MDI issues ~one drawIndexedIndirect per material.
        Uint32 mainDrawCalls = 0;
        const char* mainPath = "CPU";  // "CPU" | "Indirect" | "MDI" | "BindlessMDI"
    } lastFrameStats;

    // Tile-cull histogram cache for the "Light Culling Debug" panel. Refreshed
    // by a throttled cluster-buffer readback while that panel is open.
    bool lightCullDebugOpen = false;
    Uint32 cullAvgLights = 0, cullMinLights = 0, cullMaxLights = 0, cullNonEmptyTiles = 0;
    // Reads back the culled cluster buffer into (mn/avg/mx/nonEmpty) over the 2D
    // tile grid. Does a waitIdle, so callers must throttle. Shared by the panel
    // and the StatsLog "CULL" source. Returns false if unavailable.
    bool sampleClusterHistogram(Uint32& mn, Uint32& avg, Uint32& mx, Uint32& nonEmpty);

    // ========================================================================
    // Registered Resources
    // ========================================================================

    std::vector<RenderMesh> meshes;
    std::vector<RenderMaterial> materials;
    std::vector<RenderTexture> textures;

    // Last staged/drawn scene — kept for the ImGui Scene Materials / Scene
    // Lights editors (the panels edit shared Scene data, like native).
    std::shared_ptr<RenderScene> currentScene;

    // Texture cache (path -> TextureId)
    std::unordered_map<std::string, TextureId> textureCache;

    // Mapping from drawable index to instance ID (for correct instance data indexing)
    std::unordered_map<Uint32, Uint32> drawableToInstanceID;
    // Instances uploaded this frame: visible drawables first (ids 0..V-1),
    // then camera-culled ones (shadow casters). Shadow passes bind/draw the
    // full range; main/pre passes keep using only the visible prefix.
    Uint32 totalInstanceCount = 0;

    // ========================================================================
    // Per-Frame Data
    // ========================================================================

    CameraRenderData currentCamera;
    std::vector<Drawable> frameDrawables;
    std::vector<Uint32> visibleDrawables;  // Indices into frameDrawables
    std::vector<DirectionalLightData> directionalLights;
    std::vector<PointLightData> pointLights;

    // ========================================================================
    // GPU Resources
    // ========================================================================

    // Uniform buffers
    BufferHandle cameraUniformBuffer;
    BufferHandle materialUniformBuffer;
    BufferHandle directionalLightBuffer;
    BufferHandle pointLightBuffer;
    BufferHandle frameDataBuffer;
    BufferHandle instanceDataBuffer;
    BufferHandle clusterBuffer;

    // ------------------------------------------------------------------------
    // Frames-in-flight buffer slotting (native renderer parity — its
    // cameraDataBuffers[currentFrameInFlight] et al.). Host-visible
    // updateBuffer() is an immediate memcpy, so rewriting a buffer the GPU may
    // still read from an in-flight frame is a data race. Every buffer that is
    // rewritten per frame is created once per slot; beginFrame() repoints the
    // named alias member (e.g. cameraUniformBuffer) at the current slot so
    // all bind/update sites stay unchanged.
    // ------------------------------------------------------------------------
    // Slot count is not a renderer-side constant: it follows the backend's
    // frames-in-flight (rhi->getMaxFramesInFlight()), the single source of truth
    // for how far the CPU can run ahead of the GPU. One slot per in-flight frame
    // is exactly the no-race minimum. Set once at init, before the first
    // createFrameSlottedBuffer() call.
    Uint32 frameSlotCount = 0;
    struct FrameSlottedBuffer {
        BufferHandle* alias;
        std::vector<BufferHandle> slots;  // frameSlotCount entries
    };
    std::vector<FrameSlottedBuffer> frameSlottedBuffers;
    Uint32 frameSlotIndex = 0;
    void createFrameSlottedBuffer(BufferHandle& alias, const BufferDesc& desc,
                                  const void* initData = nullptr, size_t initSize = 0);

    // Default textures
    TextureId defaultWhiteTexture = INVALID_TEXTURE_ID;
    TextureId defaultNormalTexture = INVALID_TEXTURE_ID;
    TextureId defaultBlackTexture = INVALID_TEXTURE_ID;
    // Neutral ORM (occlusion=1, roughness=1, metallic=0) — the default for
    // materials lacking a metallic/roughness/occlusion map. Using white here
    // (metallic .b = 1.0) rendered every such surface as fully metallic:
    // flat and dark. Matches the native default_orm.png.
    TextureId defaultORMTexture = INVALID_TEXTURE_ID;

    // Default sampler
    SamplerHandle defaultSampler;
    // Point/clamp sampler for depth maps (shadows). Linear filtering of a depth
    // texture is wrong for manual PCF and a slow/emulated path on Metal/MoltenVK.
    SamplerHandle shadowSampler;
    // Linear + clamp-to-edge sampler for fullscreen/bloom passes (repeat would
    // wrap at screen edges; the bloom pyramid needs clamped bilinear taps).
    SamplerHandle clampSampler;

    // Render targets
    TextureHandle colorRT_MSAA;
    TextureHandle colorRT;
    TextureHandle depthStencilRT_MSAA;
    TextureHandle depthStencilRT;
    TextureHandle normalRT_MSAA;
    TextureHandle normalRT;
    TextureHandle shadowRT;
    TextureHandle aoRT;
    TextureHandle sscsRT;   // screen-space contact shadow visibility (R8, min-composited onto the sun shadow)
    // Physically-based pyramid bloom (matches the Metal backend): brightness
    // extract -> progressive downsample chain -> tent-filter upsample chain that
    // accumulates back into pyramid[0] -> composited in PostProcess.
    static constexpr Uint32 BLOOM_PYRAMID_LEVELS = 5;
    TextureHandle bloomBrightness;                    // half-res bright extract
    TextureHandle bloomPyramid[BLOOM_PYRAMID_LEVELS]; // progressively halved
    float bloomThreshold = 1.0f;
    float bloomStrength = 0.8f;

    // Default depth buffer for swapchain rendering (when not using render targets)
    TextureHandle swapchainDepthBuffer;

    // Graphics pipelines
    PipelineHandle mainPipeline;
    PipelineHandle prePassPipeline;
    PipelineHandle postProcessPipeline;
    PipelineHandle bloomBrightPipeline;
    PipelineHandle bloomDownsamplePipeline;
    PipelineHandle bloomUpsamplePipeline;
    // Metal-only: native composites bloom in its own pass (colorRT +
    // pyramid[0] -> tempColorRT, then swap); the Vulkan twin composites
    // inside PostProcess.frag instead.
    PipelineHandle bloomCompositePipeline;
    // Metal-only: native velocity is a compute kernel (3d_velocity.metal);
    // the Vulkan path uses the fullscreen-fragment velocityPipeline below.
    ComputePipelineHandle velocityComputePipeline;
    // Shader handles created for the Metal pass chain (kept for shutdown).
    std::vector<ShaderHandle> metalPassShaders;
    // Post-process tunables (contract of 3d_post_process.metal — the Vulkan
    // PostProcess.frag currently bakes its own constants).
    struct alignas(16) PostProcessParams {
        float chromaticAberrationStrength = 0.01f;
        float chromaticAberrationFalloff = 2.0f;
        float vignetteStrength = 0.3f;
        float vignetteRadius = 0.8f;
        float vignetteSoftness = 0.5f;
        float saturation = 1.0f;
        float contrast = 1.0f;
        float brightness = 0.0f;
        float temperature = 0.0f;
        float tint = 0.0f;
        float exposure = 1.0f;
    };
    PostProcessParams postProcessParams;
    BufferHandle postProcessParamsBuffer;  // Vulkan PostProcess.frag set1 b0
    PipelineHandle atmospherePipeline;
    PipelineHandle lightScatteringPipeline;
    TextureHandle lightScatteringRT;  // half-res god rays
    bool lightScatteringEnabled = true;
    // Persistent god-rays tunables (ImGui-editable, defaults match native).
    // lightScatteringPass() copies this and overwrites the per-frame fields
    // (sunScreenPos/screenSize/sunColor).
    LightScatteringRenderData lightScatteringSettings;
    PipelineHandle volumetricFogPipeline;
    TextureHandle tempColorRT;  // ping-pong target for fog (swapped with colorRT)
    bool volumetricFogEnabled = true;
    // Persistent fog tunables (ImGui-editable). volumetricFogPass() copies this
    // and overwrites the per-frame fields (invViewProj/camera/sun).
    FogRenderData fogSettings;
    // Heterogeneous volume raymarch (EmberGen density grids; rendering only —
    // import/parsing lives in a separate PR). One AABB volume per scene; a
    // procedural 64^3 test grid stands in until setVolumeDensity() gets real
    // data. Reuses tempColorRT ping-pong like fog.
    PipelineHandle volumeRaymarchPipeline;
    TextureHandle volumeDensityTexture;  // active 3D grid (may equal the test grid)
    TextureHandle volumeTestTexture;     // owned procedural test grid
    bool volumeRenderEnabled = false;    // default OFF until real data lands
    VolumeRenderData volumeSettings;     // panel tunables (box/density/albedo/steps)
    // The registry the ECS draw() last rendered with. renderToTexture needs it
    // because the demo's meshes live on ECS entities, not the scene-node tree —
    // the scene-only collectDrawables(scene) finds nothing there. The registry
    // outlives frames (owned by the game), so the cached pointer stays valid.
    entt::registry* lastDrawRegistry = nullptr;
    // Render-to-texture scene view (renderToTexture): dedicated camera and
    // instance buffers. The offscreen view encodes BEFORE the main draw, but
    // host-visible buffer writes are last-write-wins across the whole frame's
    // command stream — sharing cameraUniformBuffer/instanceDataBuffer meant
    // the RTT draw executed with the MAIN view's data (or vice versa).
    BufferHandle rttCameraBuffer;
    BufferHandle rttInstanceBuffer;
    // Camera-motion velocity (motion vectors) — infrastructure for future TAA.
    PipelineHandle velocityPipeline;
    TextureHandle velocityRT;
    BufferHandle prevViewProjBuffer;
    glm::mat4 prevViewProj = glm::mat4(1.0f);
    bool prevViewProjValid = false;
    // RmlUI (cross-backend RHI render interface; owned, see initUI()).
    void* m_uiRenderer = nullptr;
    Rml::Context* m_uiContext = nullptr;
    SDL_Window* window = nullptr;

    // Volumetric clouds (quarter-res raymarch -> temporal resolve -> composite).
    // Off by default; parameters mirror the Metal-tested VolumetricCloudData.
    PipelineHandle cloudRaymarchPipeline;
    PipelineHandle cloudTemporalPipeline;
    PipelineHandle cloudCompositePipeline;
    TextureHandle cloudRT;          // quarter-res current raymarch
    TextureHandle cloudHistoryRT;   // previous resolved frame
    TextureHandle cloudResolvedRT;  // temporal output (swapped with history)
    BufferHandle cloudDataBuffer;
    VolumetricCloudRenderData cloudSettings;  // CPU copy (tunables + wind/time accumulation)
    glm::mat4 cloudPrevViewProj = glm::mat4(1.0f);
    bool cloudPrevViewProjValid = false;
    bool volumetricCloudsEnabled = false;  // default OFF (enable when verifying)

    // GPU particle system (self-contained orbital demo + ECS emitters).
    static constexpr Uint32 MAX_PARTICLES = 3'000'000;
    ComputePipelineHandle particleForcePipeline;
    ComputePipelineHandle particleIntegratePipeline;
    // Instanced billboard pipelines, one per ParticleBlendMode (Additive /
    // AlphaBlend / Multiply) — per-material particle draws pick by packet.
    static constexpr Uint32 PARTICLE_BLEND_COUNT = 3;
    PipelineHandle particleRenderPipelines[PARTICLE_BLEND_COUNT];
    BufferHandle particleBuffer;
    BufferHandle particleSimParamsBuffer;
    BufferHandle particleAttractorBuffer;      // MAX_PARTICLE_ATTRACTORS elements
    // Indirect draw args, MAX_PARTICLE_DRAWS × 16B (VkDrawIndirectCommand /
    // MTLDrawPrimitivesIndirectArguments). CPU-written today; a GPU compact/cull
    // pass can take over writing instanceCount without touching the draw loop.
    BufferHandle particleDrawArgsBuffer;
    Uint32 particleCount = 0;          // = high-water mark of claimed slots; 0 = no ECS emitters yet
    bool particleVisible = true; // hide toggle — gates render only, sim keeps running
    std::vector<ParticleDrawPacket> m_particleDrawList; // set each frame by ParticleRenderSystem

    // ECS particle slot management (first-fit free list).
    struct ParticleSlotRange { uint32_t begin = 0, count = 0; };
    std::vector<ParticleSlotRange> m_particleSlotFreeList;
    bool m_particleFreeListInitialized = false;
    ParticleForceField m_forceField; // set each frame by ParticleForceFieldSystem
    bool m_particleSimPaused = false;

    // Free-list helpers
    uint32_t allocParticleSlots(uint32_t count);
    void freeParticleSlots(uint32_t slotBegin, uint32_t count);
    void ensureParticleFreeList();
    PipelineHandle shadowPipeline;
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    ShaderHandle prePassVertexShader;
    ShaderHandle prePassFragmentShader;
    ShaderHandle postProcessVertexShader;
    ShaderHandle postProcessFragmentShader;
    ShaderHandle bloomBrightShader;
    ShaderHandle bloomDownsampleShader;
    ShaderHandle bloomUpsampleShader;
    ShaderHandle atmosphereVertexShader;
    ShaderHandle atmosphereFragmentShader;
    ShaderHandle lightScatteringShader;
    ShaderHandle volumetricFogShader;
    BufferHandle fogDataBuffer;
    ShaderHandle volumeRaymarchShader;
    BufferHandle volumeDataBuffer;
    ShaderHandle velocityShader;
    ShaderHandle particleForceShader;
    ShaderHandle particleIntegrateShader;
    ShaderHandle particleVertexShader;
    ShaderHandle particleFragmentShader;
    ShaderHandle cloudRaymarchShader;
    ShaderHandle cloudTemporalShader;
    ShaderHandle cloudCompositeShader;
    ShaderHandle shadowVertexShader;
    ShaderHandle shadowFragmentShader;
    static constexpr Uint32 SHADOW_MAP_SIZE = Vapor::kDirectionalShadowMapSize;  // shared (irenderer.hpp)

    // Compute pipelines
    ComputePipelineHandle buildClustersPipeline;
    ComputePipelineHandle cullLightsPipeline;
    ComputePipelineHandle tileCullingPipeline;
    ComputePipelineHandle normalResolvePipeline;
    ComputePipelineHandle raytraceShadowPipeline;
    ComputePipelineHandle raytraceAOPipeline;
    // RT denoising chain (RequiresRaytracing-gated; Metal MSL kernels via RHI)
    ComputePipelineHandle aoTemporalPipeline;
    ComputePipelineHandle aoDenoisePipeline;
    ComputePipelineHandle stochasticShadowPipeline;
    ComputePipelineHandle stochasticShadowTemporalPipeline;
    ComputePipelineHandle stochasticShadowDenoisePipeline;
    // ReSTIR reuse for the stochastic shadow (restirShadowPass): half-res
    // reservoir build + temporal merge, half-res spatial merge + winner rays,
    // then joint bilateral upsample back to the full-res raw target.
    ComputePipelineHandle restirShadowTemporalPipeline;
    ComputePipelineHandle restirShadowResolvePipeline;
    ComputePipelineHandle stochasticShadowUpsamplePipeline;
    // GIBS screen-space denoise ("SVGF-lite"): temporal reprojection + 2x
    // edge-aware a-trous over the GI gather output. The gather writes giRawRT,
    // the chain produces the giResultTexture the PBR samples. Kills the visible
    // surfel-disc seams and residual flicker the surfel-space EMA leaves behind.
    ComputePipelineHandle giTemporalPipeline;
    ComputePipelineHandle giDenoisePipeline;
    TextureHandle giRawRT;              // gather output (pre-denoise)
    TextureHandle giScratchRT;          // a-trous ping-pong
    TextureHandle giHistoryChainRT[2];  // temporal history (RGB gi, A viewZ)
    bool gibsDenoiseEnabled = true;
    Uint32 giHistoryIndex = 0;
    bool giHistoryValid = false;
    glm::mat4 giPrevView{1.0f};         // AO owns `prevView`; GI keeps its own
    bool giPrevViewValid = false;
    ShaderHandle rtShadowShader, rtAOShader, aoTemporalShader, aoDenoiseShader,
                 stochasticShadowShader, stochasticShadowTemporalShader, stochasticShadowDenoiseShader,
                 restirShadowTemporalShader, restirShadowResolveShader, stochasticShadowUpsampleShader,
                 giTemporalShader, giDenoiseShader;
    ShaderHandle prePassMetalVertexShader, prePassMetalFragmentShader;

    // Acceleration structures: one BLAS per registered mesh (index-aligned with
    // `meshes`), a TLAS rebuilt each frame from the visible drawables.
    std::vector<AccelStructHandle> meshBLAS;
    AccelStructHandle sceneTLAS;
    std::vector<AccelStructInstance> tlasInstances;

    // RT AO / point-shadow working set
    TextureHandle albedoRT;          // prepass MRT output 1 (GIBS later)
    TextureHandle aoRawRT;           // noisy RT AO
    TextureHandle aoScratchRT;       // à-trous intermediate
    TextureHandle aoHistoryRT[2];    // temporal ping-pong
    Uint32 aoHistoryIndex = 0;
    bool aoHistoryValid = false;
    glm::mat4 prevView = glm::mat4(1.0f);
    bool prevViewValid = false;
    TextureHandle stochasticShadowRT;         // stochastic raw (full res)
    TextureHandle stochasticShadowHistoryRT;  // temporal history (post-swap = latest)
    TextureHandle stochasticShadowDenoisedRT; // temporal output (swapped with history)
    TextureHandle stochasticShadowHalfRT;     // half-res ReSTIR resolve output (pre-upsample)
    // (frameDataBuffer for the RT kernels' random seeds is declared above.)
    bool aoEnabled = true;
    // AO method (native aoMethod): 0 = ray traced, 1 = screen space (SSAO).
    // On non-RT backends SSAO is the only option and is used regardless.
    int aoMethod = 0;
    ComputePipelineHandle ssaoPipeline;  // Metal: 3d_ssao.metal compute kernel
    ShaderHandle ssaoShader;
    // Vulkan AO chain twins — fullscreen fragment passes (the RHI compute path
    // cannot sample depth on Vulkan; same pattern as Velocity/clouds).
    PipelineHandle vkSsaoPipeline;              // SSAO.frag -> aoRawRT (R16F)
    PipelineHandle vkSscsPipeline;              // SSCS.frag -> sscsRT (Vulkan fullscreen contact shadow)
    ShaderHandle   vkSscsShader;
    ComputePipelineHandle sscsComputePipeline;  // 3d_sscs.metal -> sscsRT (Metal compute contact shadow)
    ShaderHandle   sscsMetalShader;
    PipelineHandle vkAoTemporalPipeline;        // AOTemporal.frag -> history (RGBA16F)
    PipelineHandle vkAoDenoisePipelineRGBA;     // AODenoise.frag -> scratch (RGBA16F)
    PipelineHandle vkAoDenoisePipelineR16;      // AODenoise.frag -> aoRT (R16F)
    ShaderHandle vkSsaoShader, vkAoTemporalShader, vkAoDenoiseShader;
    BufferHandle aoTemporalDataBuffer;          // {mat4 prevView; uint historyValid}
    // PSSM: distance where RT near-field shadows hand over to the cascades
    // (native pssmRTMaxDist, panel-tunable 5..200).
    float pssmRTMaxDist = 12.0f;  // near shadow extent [near, this] (character scale); cascades beyond
    Uint32 pssmPcfSampleCount = 16;  // PCF taps: 4/8/16/32 (honoured by both Metal and Vulkan shaders)
    float  pssmCascadeBlendRange = 2.0f;  // cascade<->cascade blend width (view units)
    bool   pssmDebugVisualize = false;    // tint cascades for debugging
    // Independent near-field shadow map extent (view-space metres). The near map
    // covers [near, nearShadowEnd]; cascades take over beyond it. 0 = disabled.
    float nearShadowEnd = 8.0f;
    // Screen-space contact shadows (min-composited onto the sun shadow term). Opt-in.
    bool  sscsEnabled = false;
    float sscsLength = 0.3f;      // view-space march distance (contact scale, metres)
    float sscsThickness = 0.3f;   // occluder depth window
    Uint32 sscsSteps = 12;
    float sscsBias = 0.02f;       // view-space start offset (self-occlusion guard)
    // Stochastic RT shadows for the analytic lights (point R / rect G / spot B
    // channels). Metal RT only. Default OFF ("Directional only"): Vulkan has
    // no equivalent path yet, and the default keeps the two backends' output
    // aligned — opt in via the panel's "All shadows", where ReSTIR + the
    // denoise chain keep the noise down.
    bool stochasticShadowsEnabled = false;
    // ReSTIR denoise for the stochastic shadows: per-pixel weighted reservoirs
    // over light samples with temporal + spatial reuse on a HALF-RES grid, so
    // the ray budget lands on the light that dominates each pixel; a joint
    // bilateral upsample returns to full res before the accumulator. Falls
    // back to the legacy full-res uniform-pick kernel when off (or when the
    // reservoir allocation fails). Buffers exist only while the path runs.
    bool restirShadowsEnabled = true;
    BufferHandle restirReservoirHistory;  // post-spatial reservoirs (frame N-1), half grid
    BufferHandle restirReservoirScratch;  // pass-1 output within the frame, half grid
    // History is valid only when the pass ran last frame too — derived from
    // frame contiguity so every skip path (toggles, invalid TLAS, resize,
    // graph edits) invalidates it without needing to remember to.
    bool restirHistoryValid = false;
    Uint32 restirLastFrame = 0;
    // Panel-exposed: point candidates, spatial taps, spatial radius, point
    // M-clamp. Fixed defaults (no control drives them): rect/spot candidate
    // counts and the rect M-clamp. M clamps are multiples of the per-frame
    // candidate count and bound how long a reservoir dwells on one winner —
    // kept low so the temporal accumulator still averages across winner
    // switches. Reservoirs select LIGHTS only (the rect quad point is
    // re-jittered every frame, see restir_shadow_common.metal), so the rect
    // clamp too governs selection stability only, never penumbra sampling.
    Uint32 restirPointCandidates = 8;
    Uint32 restirRectCandidates = 4;   // fixed
    Uint32 restirSpotCandidates = 4;   // fixed
    Uint32 restirSpatialTaps = 4;
    float restirSpatialRadius = 16.0f;  // full-res px (pre-scaled for the half grid)
    float restirPointMClamp = 8.0f;
    float restirRectMClamp = 8.0f;      // fixed
    // Dataflow guards: the accumulator only runs on frames the stochastic
    // pass actually wrote (TLAS ready, pipelines built), and the PBR only
    // samples a history that has been accumulated at least once — otherwise
    // both would read undefined texture memory when the feature turns on.
    // stochasticShadowDenoiseRan additionally routes the PBR to the edge-aware
    // filtered copy when the denoise pass produced one this frame.
    bool stochasticShadowWritten = false;         // this frame
    bool stochasticShadowHistoryWritten = false;  // ever (since last RT rebuild)
    bool stochasticShadowDenoiseRan = false;      // this frame
    // Stochastic point-shadow debug view (native stochasticShadowDebugMode):
    // 0 = visibility, 1 = tile light-count heatmap, 2 = ReSTIR winner id,
    // 3 = ReSTIR reservoir confidence (modes 2-3 need the ReSTIR path).
    Uint32 stochasticShadowDebugMode = 0;
    // Vulkan Main-pass perf isolation (RHIMain.frag mainDebugFlags, push offset 96):
    // bit0 = skip point-light loop, bit1 = skip shadow PCF. Panel-driven.
    Uint32 mainDebugFlags = 0;

    // Sun/lens flare (Metal MSL for now; GLSL twin lands with the IBL round).
    // (tileCullingPipeline is declared with the other compute pipelines above.)
    ShaderHandle tileCullingShader;
    // Vulkan tile culling twin (TileLightCull.comp) + its params buffer.
    ComputePipelineHandle vkTileCullPipeline;
    ShaderHandle vkTileCullShader;

    // GPU-driven rendering (Vulkan): compute frustum cull -> per-object indirect
    // draw. Off by default; toggle with setGpuDrivenCulling().
    ComputePipelineHandle gpuCullPipeline;
    ShaderHandle gpuCullShader;
    BufferHandle gpuCullArgsBuffer;  // DrawCommand[MAX_INSTANCES], frame-slotted
    // Frustum-only cull output for the GPU-driven pre-pass (Option A): runs
    // before the pre-pass (Hi-Z not built yet), so the depth pass draws indirect
    // instead of from the CPU-culled visibleDrawables. Same layout as
    // gpuCullArgsBuffer; the main GpuCull still adds Hi-Z occlusion afterwards.
    BufferHandle prepassCullArgsBuffer;
    // Pre-pass goes fully GPU-driven (indirect, no CPU cull) when the MDI/Bindless
    // instance layout is active — merged buffers + material ranges exist there.
    // Runtime toggle so the CPU-cull pre-pass can be compared/restored.
    bool gpuDrivenPrePass = true;

    // GPU-driven geometry submission method (mutually exclusive — the same object
    // can't go through both the vertex pipeline and mesh shaders):
    //   Off      - CPU cull + drawIndexed (default; existing path untouched)
    //   Indirect - compute cull (GpuCull.comp) -> per-object / MDI indirect draw
    // Hi-Z occlusion (below) is orthogonal and applies to whichever mode is active.
    enum class GpuDrivenMode { Off, Indirect, BindlessMDI };
    GpuDrivenMode gpuDrivenMode = GpuDrivenMode::Off;
    bool gpuDrivenActive()   const { return gpuDrivenMode != GpuDrivenMode::Off; }
    // Indirect covers BOTH compute-cull modes: plain Indirect (per-object /
    // per-material MDI draws) and BindlessMDI share the cull pass, Hi-Z, and
    // the instance pipeline; they differ only in how the main pass submits.
    bool gpuDrivenIndirect() const {
        return gpuDrivenMode == GpuDrivenMode::Indirect ||
               gpuDrivenMode == GpuDrivenMode::BindlessMDI;
    }
    bool gpuDrivenBindless() const { return gpuDrivenMode == GpuDrivenMode::BindlessMDI; }
    // Whether this frame will use the merged-buffer MDI instance layout (plain
    // MDI or Bindless MDI). Deterministic from mode + backend + caps (no prior-
    // frame state), so render() can consult it BEFORE updateBuffers sets
    // m_mdiInstanceLayout — used to gate CPU culling off for Option A's
    // GPU-driven pre-pass. Kept in sync with the `mdi` local in collectDrawables.
    bool mdiLayoutActive() const {
        const bool bindless = gpuDrivenBindless() && capabilities.bindlessTextures &&
            (backend == GraphicsBackend::Metal ? capabilities.indirectCommandBuffers
                                               : capabilities.multiDrawIndirect);
        return bindless ||
               (gpuDrivenIndirect() && gpuDrivenMDI &&
                ((backend == GraphicsBackend::Vulkan && capabilities.multiDrawIndirect) ||
                 backend == GraphicsBackend::Metal));
    }
    // Whether the depth pre-pass runs fully GPU-driven this frame (so the CPU
    // frustum cull is redundant and skipped): the indirect MDI pre-pass.
    // Consulted in render() BEFORE updateBuffers, so it uses only mode/caps (no
    // per-frame buffer state).
    bool gpuDrivenPrePassActive() const {
        return gpuDrivenPrePass && mdiLayoutActive();
    }
    // Hi-Z occlusion culling (requires a GPU-driven mode). A depth pyramid built
    // from the PrePass depth; the cull compute rejects instances whose screen
    // AABB is fully behind the recorded occluders. Off by default; the reduce
    // pass and the cull's occlusion branch both no-op unless this is on.
    bool gpuOcclusionCulling = false;
    void setGpuOcclusionCulling(bool e) { gpuOcclusionCulling = e; }
    bool isGpuOcclusionCulling() const { return gpuOcclusionCulling; }
    TextureHandle hizTexture;         // R32F, full mip chain, max-depth pyramid
    TextureHandle hizScratchTexture;  // staging for the single-texture build
    Uint32 hizWidth = 0, hizHeight = 0, hizMipCount = 0;
    PipelineHandle hizReducePipeline;
    ShaderHandle hizReduceFS;  // Vulkan reuses the fullscreen VS; Metal is one file
    void hizBuildPass();

    // True single-call multi-draw indirect (Vulkan only): one vkCmdDrawIndexedIndirect
    // per material batch over a merged scene vertex/index buffer, instead of one
    // indirect draw per object. Metal keeps the per-object loop (single-call MDI
    // there needs Indirect Command Buffers). Sub-option of the Indirect mode.
    bool gpuDrivenMDI = false;
    // Bindless MDI draw mode: the whole scene in ONE submission with material
    // textures fetched bindlessly by materialID (no per-material CPU loop).
    //   Metal:  the cull kernel encodes draws into a real
    //           MTLIndirectCommandBuffer, replayed with one
    //           executeCommandsInBuffer; materials come from an argument table
    //           at fragment buffer 13.
    //   Vulkan: one native vkCmdDrawIndexedIndirect over every instance (the
    //           same cull-written args buffer as plain MDI); materials come
    //           from a descriptor-indexed runtime array at set 3.
    // Selected as its own GpuDrivenMode (mutually exclusive with plain MDI),
    // gated on capabilities.bindlessTextures plus the per-backend submission
    // cap (indirectCommandBuffers / multiDrawIndirect).
    IndirectCommandBufferHandle sceneICB;      // Metal only: GPU-encoded commands
    ComputePipelineHandle gpuCullICBPipeline;  // Metal only: ICB-encoding cull
    ShaderHandle gpuCullICBShader;
    PipelineHandle mainPipelineBindless;  // mainPipeline twin with the bindless fragment
    ShaderHandle fragmentShaderBindless;  // Metal: kBindlessMaterials specialization;
                                          // Vulkan: RHIMainBindless.frag.spv
    BufferHandle bindlessMaterialTable;   // 6 texture slots per material (see RHI docs)
    bool m_bindlessTableDirty = true;     // rewrite entries when materials/textures change
    void ensureBindlessMaterialTable();
    // Metal only: ICB-compatible pipelines reject DIRECT fragment texture
    // arguments, so the bindless fragment takes the 10 per-frame system
    // textures (Metal contract slots 6-15) through a second single-entry
    // argument table at buffer(14). Entries are rewritten only when the
    // resolved handle changes (cache below) — rewriting every frame would race
    // in-flight replays of the shared table.
    BufferHandle bindlessSystemTable;
    TextureHandle m_bindlessSysCache[10];
    // Note: the single Metal ICB is shared across frames in flight — Metal's
    // automatic hazard tracking serializes the next frame's cull (write)
    // against the previous frame's executeICB (read). Correct, at the cost of
    // some overlap; per-frame ICB slots are a follow-up if timings say so.
    // True while this frame's InstanceData carries MERGED-buffer vertex/index
    // offsets (MDI layout, set in updateBuffers). Metal shaders that pull
    // vertices via instances[iid].vertexOffset + vertex_id (pre-pass, shadow)
    // must then read the MERGED vertex buffer, not the per-mesh one — the
    // per-mesh buffers are far smaller than the offsets baked into the
    // instances, so mixing them fetches garbage (full-screen depth corruption).
    bool m_mdiInstanceLayout = false;
    std::vector<Vapor::VertexData> m_mergedVertices;  // CPU accumulation (registerMesh)
    std::vector<Uint32> m_mergedIndices;              // mesh-local indices; rebased via vertexOffset
    BufferHandle mergedVertexBuffer;
    BufferHandle mergedIndexBuffer;
    bool m_mergedGeometryDirty = false;
    // Per-material contiguous instance ranges in the material-sorted InstanceData
    // buffer, from the last MDI updateBuffers(): {material, {firstSlot, count}}.
    std::vector<std::pair<MaterialId, std::pair<Uint32, Uint32>>> m_materialRanges;
    void ensureMergedGeometry();  // (re)build merged GPU buffers from CPU data

    BufferHandle lightCullDataBuffer;
    PipelineHandle sunFlarePipeline;
    ShaderHandle sunFlareVertexShader, sunFlareFragmentShader;
    BufferHandle sunFlareDataBuffer;
    SunFlareRenderData sunFlareSettings;
    bool sunFlareEnabled = false;  // native default
    void sunFlarePass();

    // IBL-from-sky chain (Metal MSL; runs once, re-runs when iblNeedsUpdate).
    TextureHandle environmentCubemap;   // 512 cube, mips (sky capture)
    TextureHandle irradianceMap;        // 32 cube
    TextureHandle prefilterMap;         // 128 cube, 5 mips
    TextureHandle brdfLUTTex;           // 512 2D
    BufferHandle iblCaptureDataBuffer;  // per-draw face/roughness (36 variants)
    PipelineHandle skyCapturePipeline, irradiancePipeline, prefilterPipeline, brdfLUTPipeline;
    ShaderHandle skyCaptureVS, skyCaptureFS, irradianceVS, irradianceFS,
                 prefilterVS, prefilterFS, brdfVS, brdfFS;
    bool iblNeedsUpdate = true;
    static constexpr Uint32 PREFILTER_MIP_LEVELS = 5;
    void iblCapturePass();

    // HDRI environment source (ported from the native Metal renderer). When set,
    // iblCapturePass converts the equirect map into environmentCubemap (instead of
    // the sky capture) and the rest of the IBL chain runs unchanged. The IBL chain
    // is currently Metal-only on the RHI path, so HDRI IBL applies to Metal-via-RHI.
    enum class IBLSource { Sky, HDRI };
    IBLSource iblSource = IBLSource::Sky;
    TextureHandle equirectHDRITexture;         // RGBA32F equirect source
    PipelineHandle equirectToCubemapPipeline;
    ShaderHandle equirectToCubemapVS, equirectToCubemapFS;

    // GIBS surfel GI (RequiresRaytracing; Metal MSL kernels via RHI compute).
    // GIBSData itself lives in graphics_gibs.hpp (included in renderer.cpp only
    // to keep this header clear of the split-header landmines).
    ComputePipelineHandle surfelGenPipeline, surfelUpdatePipeline, surfelClearPipeline,
                          surfelInsertPipeline, surfelRTPipeline, surfelTemporalPipeline, giSamplePipeline;
    ShaderHandle gibsShaders[6];
    ShaderHandle gibsUpdateShader;  // surfelUpdate entry (aging + free-list eviction)
    BufferHandle surfelBuffer, cellHeadBuffer, surfelNextBuffer, surfelFreeListBuffer,
                 surfelCounterBuffer, gibsDataBuffer;
    TextureHandle giResultTexture;
    glm::mat4 gibsPrevViewProj = glm::mat4(1.0f);
    Uint32 gibsActiveSurfels = 0;
    Uint32 gibsMaxSurfels = 500000;   // Medium quality (native default)
    Uint32 gibsRaysPerSurfel = 4;
    float gibsResolutionScale = 0.5f;
    glm::vec3 gibsWorldMin = glm::vec3(-64.0f);  // native GIBSManager default
    glm::vec3 gibsWorldMax = glm::vec3(64.0f);
    bool gibsEnabled = false;  // bring-up default off
    // Panel state: quality preset combo (native gibsQuality; 1 = Medium, the
    // values gibsMaxSurfels/RaysPerSurfel/ResolutionScale default to) and a
    // deferred surfel reset (applied at the top of gibsPass, never mid-frame).
    int gibsQuality = 1;
    bool gibsResetRequested = false;
    void gibsPass();

    void aoTemporalPass();
    void aoDenoisePass();
    void stochasticShadowPass();
    bool restirShadowPass();  // false = couldn't run, caller uses the legacy kernel
    void stochasticShadowTemporalPass();
    void stochasticShadowDenoisePass();

    // Acceleration structures (for ray tracing)
    std::vector<AccelStructHandle> BLASs;  // Bottom-level acceleration structures (one per mesh)
    AccelStructHandle TLAS;                 // Top-level acceleration structure

    // ========================================================================
    // Batch Rendering Resources
    // ========================================================================

    struct Vertex2D {
        glm::vec3 position;   // vec3 for 2D/3D compatibility
        glm::vec4 color;
        glm::vec2 texCoord;
        float texIndex;       // Texture array index (future: batching textures)
        int entityID;         // For editor picking
        // Stride pad: 2d_batch.metal raw-fetches Batch2DVertexIn at a 48-byte
        // stride (its layout ends in a float _pad). Without this the Metal
        // vertex fetch walked off-stride after the first vertex and every
        // batch quad/sprite rendered as garbage or not at all.
        float _pad0 = 0.0f;
    };

    struct BatchRenderer {
        static constexpr uint32_t MaxQuads = 10000;
        static constexpr uint32_t MaxVertices = MaxQuads * 4;
        static constexpr uint32_t MaxIndices = MaxQuads * 6;

        BufferHandle vertexBuffer;  // current frame's slot (see nextFrame())
        BufferHandle indexBuffer;   // static quad index pattern — write-once, unslotted
        // Vertex data is rewritten every frame; per-frame slots keep the CPU
        // from overwriting a buffer an in-flight frame's draw still reads
        // (native parity: batch2DVertexBuffers[currentFrameInFlight]).
        static constexpr uint32_t kSlots = 3;
        BufferHandle vertexBufferSlots[kSlots];
        uint32_t slotIndex = 0;
        PipelineHandle pipeline;    // HDR colorRT variant (in-scene batches)
        ShaderHandle vertexShader;
        ShaderHandle fragmentShader;

        std::vector<Vertex2D> vertices;
        std::vector<uint32_t> indices;
        uint32_t quadCount = 0;

        TextureHandle whiteTexture;  // Default white texture for colored quads
        SamplerHandle sampler;       // Sampler used for the batch texture
        // Quads are grouped into segments by texture; flush() issues one draw
        // per segment. Texture switches therefore never force a mid-frame
        // flush (draws are only legal inside a render pass).
        struct Segment {
            TextureHandle texture;
            uint32_t quadStart = 0;
            uint32_t quadCount = 0;
        };
        std::vector<Segment> segments;
        TextureHandle pendingTexture;  // texture for the next quad added

        // Store RHI and current view-projection for auto-flush
        RHI* currentRHI = nullptr;
        glm::mat4 currentViewProj = glm::mat4(1.0f);
        bool canAutoFlush = false;
        // Backend decides the flush() binding contract (Metal buffer indices
        // vs Vulkan push constants / vertex input).
        GraphicsBackend rhiBackend = GraphicsBackend::Vulkan;

        // Stats
        uint32_t drawCalls = 0;
        uint32_t totalQuads = 0;

        void init(RHI* rhi, GraphicsBackend backend, bool is3D, TextureHandle defaultTex, SamplerHandle samplerHandle);
        // Advance to the next vertex-buffer slot; call once per frame.
        void nextFrame();
        // Set the texture for subsequent quads (invalid = white). Recorded as
        // a segment split; the actual draws happen in flush().
        void setTexture(TextureHandle texture);
        void accountQuadSegment();
        void shutdown(RHI* rhi);
        void flush(RHI* rhi, const glm::mat4& viewProj, PipelineHandle overridePipeline = {});
        void beginBatch(RHI* rhi, const glm::mat4& viewProj);
        void reset();
        void addQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, int entityID = -1);
        void addQuad(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
        void addQuad(
            const glm::mat4& transform,
            const glm::vec2* texCoords,
            const glm::vec4& tint,
            int entityID = -1
        );
        // Filled triangle as a degenerate quad (v3 duplicates v2, so the
        // second triangle of the quad's 0,1,2 / 2,3,0 indices is zero-area).
        void addTriangle(
            const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2,
            const glm::vec4& color, int entityID = -1
        );
    };

    BatchRenderer batch2D;
    BatchRenderer batch3D;
    RHIBatch2DStats batch2DStats;

    // ========================================================================
    // Font Rendering Resources
    // ========================================================================

    std::unique_ptr<FontManager> fontManager;

    // ========================================================================
    // Render Texture Resources
    // ========================================================================

    struct RenderTextureResource {
        TextureHandle colorTexture;
        TextureHandle depthTexture;
        uint32_t width;
        uint32_t height;
        PixelFormat format;
        bool isHDR;
        bool hasDepth;
    };

    std::vector<RenderTextureResource> renderTextures;

    // ========================================================================
    // Post-Processing Resources
    // ========================================================================

    // (Pyramid bloom pipelines/shaders are declared with the other graphics
    // pipelines above; the earlier compute-bloom / tone-mapping scaffolding was
    // never implemented and has been removed.)

    // Vignette
    ComputePipelineHandle vignettePipeline;
    ShaderHandle vignetteShader;

    // ========================================================================
    // UI & Debug Resources
    // ========================================================================

    std::function<void()> imGuiCallback;
    std::shared_ptr<Vapor::DebugDraw> debugDraw;
    ScreenshotCallback screenshotCallback;
    bool screenshotRequested = false;

    // Screenshot state
    struct PendingScreenshot {
        BufferHandle buffer;
        ScreenshotCallback callback;
        Uint32 width;
        Uint32 height;
        Uint32 frameIndex; // Frame index when captured
    };
    std::vector<PendingScreenshot> pendingScreenshots;

    void processPendingScreenshots();

    // ========================================================================
    // Configuration
    // ========================================================================

    const Uint32 MAX_INSTANCES = 5000;// Increased for large scenes like Bistro (2911 instances)
    // Frames-in-flight comes from the RHI: rhi->getMaxFramesInFlight()
    const Uint32 MSAA_SAMPLE_COUNT = 4;
    Uint32 maxDirectionalLights = 4;
    Uint32 maxPointLights = 1024;
    Uint32 maxRectLights = 32;
    Uint32 maxSpotLights = 64;

    // Clustering configuration
    Uint32 clusterGridSizeX = 16;
    Uint32 clusterGridSizeY = 16;
    Uint32 clusterGridSizeZ = 24;

    // Render state
    RenderPath currentRenderPath = RenderPath::Forward;
    glm::vec4 clearColor = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f);
    double clearDepth = 1.0;

    // Frame state
    Uint32 currentFrameInFlight = 0;
    Uint32 frameNumber = 0;
    float time = 0.0f;
    float deltaTime = 0.016f;
    bool isInitialized = false;
    // ImGui callbacks/visibility (m_imGuiCallback, m_engineWindowCallback,
    // m_imGuiFrameCallback, m_imGuiVisible) are inherited from IRenderer.

    // Stats
    Uint32 drawCount = 0;
    Uint32 currentInstanceCount = 0;
    Uint32 culledInstanceCount = 0;
    double m_cpuPreGraphMs = 0.0;  // CPU cost of cull + sort + buffer upload (per frame)
};

// The createRenderer() factory is declared in irenderer.hpp and returns a
// std::unique_ptr<IRenderer> (Renderer for Vulkan, Renderer_Metal for Metal).
