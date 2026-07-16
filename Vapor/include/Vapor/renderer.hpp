#pragma once
#include "irenderer.hpp"
#include "rhi.hpp"
#include "render_data.hpp"
#include "render_graph.hpp"
#include "camera.hpp"
#include "graphics.hpp"
#include "font_manager.hpp"
#include "scene.hpp"
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

    // Register a mesh and return its ID
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
    void stage(std::shared_ptr<Scene> scene) override;

    // Draw a scene with Scene object
    void draw(std::shared_ptr<Scene> scene, Camera& camera) override;

    // Draw with ECS registry
    void draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera) override;

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
    void uploadRectLightVideoTexture(const uint8_t* /*rgba*/, uint32_t /*width*/, uint32_t /*height*/) {}

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
        std::shared_ptr<Scene> scene,
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

private:
    // ========================================================================
    // Internal Rendering Steps
    // ========================================================================

    void performCulling();
    void setupDefaultRenderGraph();
    void drawGpuTimingsImGui();
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
    void collectDrawables(std::shared_ptr<Scene> scene);
    void collectDrawables(entt::registry& registry, std::shared_ptr<Scene> scene);
    // Submit the scene's gathered lights (filled by the game's light systems)
    void submitSceneLights(const std::shared_ptr<Scene>& scene);

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
    std::shared_ptr<Scene> currentScene;

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
    static constexpr Uint32 kFrameSlots = 3;  // >= max frames in flight on any backend
    struct FrameSlottedBuffer {
        BufferHandle* alias;
        BufferHandle slots[kFrameSlots];
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

    // GPU particle system (self-contained orbital demo).
    static constexpr Uint32 MAX_PARTICLES = 8192;
    ComputePipelineHandle particleForcePipeline;
    ComputePipelineHandle particleIntegratePipeline;
    PipelineHandle particleRenderPipeline;     // instanced billboards
    BufferHandle particleBuffer;
    BufferHandle particleSimParamsBuffer;
    BufferHandle particleAttractorBuffer;
    Uint32 particleCount = 0;
    bool particleSystemEnabled = true;
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
    ComputePipelineHandle stochasticPointShadowPipeline;
    ComputePipelineHandle pointShadowTemporalPipeline;
    ComputePipelineHandle pointShadowDenoisePipeline;
    // ReSTIR reuse for the stochastic shadow (restirShadowPass): reservoir
    // build + temporal merge, then spatial merge + winner visibility rays.
    ComputePipelineHandle restirShadowTemporalPipeline;
    ComputePipelineHandle restirShadowResolvePipeline;
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
                 pointShadowShader, pointShadowTemporalShader, pointShadowDenoiseShader,
                 restirShadowTemporalShader, restirShadowResolveShader,
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
    TextureHandle pointShadowRT;         // stochastic raw
    TextureHandle pointShadowHistoryRT;  // temporal history (post-swap = latest)
    TextureHandle pointShadowDenoisedRT; // temporal output (swapped with history)
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
    // channels). Metal RT only. Default ON ("All shadows" in the panel) now
    // that ReSTIR keeps the noise down; backends without ray tracing skip the
    // pass and render point/rect/spot unshadowed as before, so this is the one
    // place Metal+RT output intentionally diverges from the Vulkan path.
    bool stochasticShadowsEnabled = true;
    // ReSTIR denoise for the stochastic shadows: per-pixel weighted reservoirs
    // over light samples with temporal + spatial reuse, so the one shadow ray
    // per domain lands on the light (and quad point) that dominates the pixel.
    // Falls back to the legacy uniform-pick kernel when off (or when the
    // reservoir allocation fails). Buffers exist only while the path runs.
    bool restirShadowsEnabled = true;
    BufferHandle restirReservoirHistory;  // post-spatial reservoirs (frame N-1)
    BufferHandle restirReservoirScratch;  // pass-1 output within the frame
    // History is valid only when the pass ran last frame too — derived from
    // frame contiguity so every skip path (toggles, invalid TLAS, resize,
    // graph edits) invalidates it without needing to remember to.
    bool restirHistoryValid = false;
    Uint32 restirLastFrame = 0;
    // Tunables (candidates/taps/radius/M-clamp are panel-exposed; the rect and
    // spot candidate counts are fixed defaults). M clamps are multiples of the
    // per-frame candidate count: they bound how long the reservoir can dwell
    // on one winner, so keep them low enough that the temporal accumulator's
    // ~14-frame EMA still averages across winner switches. Reservoirs select
    // LIGHTS only — the rect quad point is re-jittered every frame (see
    // restir_shadow_common.metal), so the rect clamp too governs selection
    // stability only, never penumbra sampling.
    Uint32 restirPointCandidates = 8;
    Uint32 restirRectCandidates = 4;
    Uint32 restirSpotCandidates = 4;
    Uint32 restirSpatialTaps = 4;
    float restirSpatialRadius = 16.0f;
    float restirPointMClamp = 8.0f;
    float restirRectMClamp = 8.0f;
    // Dataflow guards for the default-on chain: the accumulator only runs on
    // frames the stochastic pass actually wrote (TLAS ready, pipelines built),
    // and the PBR only samples a history that has been accumulated at least
    // once — otherwise both would read undefined texture memory at startup.
    // pointShadowDenoiseRan additionally routes the PBR to the edge-aware
    // filtered copy when the denoise pass produced one this frame.
    bool pointShadowWritten = false;         // this frame
    bool pointShadowHistoryWritten = false;  // ever (since last RT rebuild)
    bool pointShadowDenoiseRan = false;      // this frame
    // Stochastic point-shadow debug view (native pointShadowDebugMode):
    // 0 = visibility, 1 = tile light-count heatmap, 2 = ReSTIR winner id,
    // 3 = ReSTIR reservoir confidence (modes 2-3 need the ReSTIR path).
    Uint32 pointShadowDebugMode = 0;
    // Vulkan Main-pass perf isolation (RHIMain.frag mainDebugFlags, push offset 96):
    // bit0 = skip point-light loop, bit1 = skip shadow PCF. Panel-driven.
    Uint32 mainDebugFlags = 0;

    // Sun/lens flare (Metal MSL for now; GLSL twin lands with the IBL round).
    // (tileCullingPipeline is declared with the other compute pipelines above.)
    ShaderHandle tileCullingShader;
    // Vulkan tile culling twin (TileLightCull.comp) + its params buffer.
    ComputePipelineHandle vkTileCullPipeline;
    ShaderHandle vkTileCullShader;
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
    void stochasticPointShadowPass();
    bool restirShadowPass();  // false = couldn't run, caller uses the legacy kernel
    void pointShadowTemporalPass();
    void pointShadowDenoisePass();

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
};

// The createRenderer() factory is declared in irenderer.hpp and returns a
// std::unique_ptr<IRenderer> (Renderer for Vulkan, Renderer_Metal for Metal).
