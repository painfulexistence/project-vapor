#pragma once
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
#include <vector>
#include <memory>

// Forward declarations
namespace Rml {
    class Context;
}

namespace Vapor {
    class DebugDraw;
}

// ============================================================================
// Renderer - High-level renderer that uses RHI
//
// Responsibilities:
// - Manage rendering resources (meshes, materials, textures)
// - Collect drawables each frame
// - Perform culling and sorting
// - Execute draw calls via RHI
// - Multi-pass rendering (pre-pass, compute passes, main draw, post-process)
// - Clustered lighting and ray tracing
// ============================================================================

// Graphics backend selection
enum class GraphicsBackend {
    Metal,
    Vulkan
};

// Render path selection
enum class RenderPath {
    Forward,    // Simple forward rendering
    Deferred,   // Deferred rendering
    Clustered   // Clustered forward/deferred with tiled light culling
};

// Screenshot callback
struct GpuImageData {
    std::vector<uint8_t> data;
    uint32_t width;
    uint32_t height;
    uint32_t channelCount;
};

using ScreenshotCallback = std::function<void(const GpuImageData&)>;

// Batch rendering stats
struct Batch2DStats {
    uint32_t drawCalls = 0;
    uint32_t quadCount = 0;
    uint32_t vertexCount = 0;
};

// Render texture descriptor
struct RenderTextureDesc {
    uint32_t width = 1920;
    uint32_t height = 1080;
    PixelFormat format = PixelFormat::RGBA8_UNORM;
    bool isHDR = false;
    bool hasDepth = true;
    uint32_t sampleCount = 1;
};

// Render texture handle
struct RenderTextureHandle {
    uint32_t id = UINT32_MAX;
    bool isValid() const { return id != UINT32_MAX; }
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    // Initialize with RHI ownership
    // Note: Use createRenderer() factory function instead of calling initialize() directly
    void initialize(std::unique_ptr<RHI> rhiPtr, GraphicsBackend backend);
    void shutdown();

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
    void beginFrame(const CameraRenderData& camera);

    // Submit a drawable to be rendered this frame
    void submitDrawable(const Drawable& drawable);

    // Submit lights
    void submitDirectionalLight(const DirectionalLightData& light);
    void submitPointLight(const PointLightData& light);

    // Execute rendering: culling, sorting, then the RenderGraph passes
    void render();

    // End the frame
    void endFrame();

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

    void setRenderPath(RenderPath path);
    RenderPath getRenderPath() const { return currentRenderPath; }

    // ========================================================================
    // Scene/ECS Integration
    // ========================================================================

    // Stage a scene (upload meshes, materials, textures)
    void stage(std::shared_ptr<Scene> scene);

    // Draw a scene with Scene object
    void draw(std::shared_ptr<Scene> scene, Camera& camera);

    // Draw with ECS registry
    void draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera);

    // ========================================================================
    // Screenshot API
    // ========================================================================

    void readPixelsAsync(ScreenshotCallback callback);

    // ========================================================================
    // UI Integration
    // ========================================================================

    // Initialize RmlUI rendering
    bool initUI();

    // Get debug draw interface
    std::shared_ptr<Vapor::DebugDraw> getDebugDraw();

    // Set ImGui callback (called between NewFrame and Render)
    void setImGuiCallback(std::function<void()> callback);
    // Drives the engine ImGui frame: F1 visibility toggle, the always-on frame
    // callback (recording/F2), then — if visible — the app callback and the
    // Engine window (with the engine-window callback). Call after ImGui::NewFrame().
    void invokeImGuiCallback();

    // ========================================================================
    // 2D/3D Batch Rendering API
    // ========================================================================

    // Register a callback invoked inside the Engine ImGui window each frame,
    // after the built-in Graphics section. Use this to append engine-level
    // panels (e.g. Recording) without re-opening the window from outside.
    // Only runs while the engine overlay is visible (see setImGuiVisible).
    void setEngineWindowCallback(std::function<void()> callback) {
        m_engineWindowCallback = std::move(callback);
    }

    // Register a callback invoked every frame right after ImGui::NewFrame(),
    // before any window is opened and regardless of overlay visibility. Use for
    // per-frame engine logic that must keep running with the UI hidden — e.g.
    // recording frame capture and the F2 start/stop hotkey.
    void setImGuiFrameCallback(std::function<void()> callback) {
        m_imGuiFrameCallback = std::move(callback);
    }

    // Engine ImGui overlay visibility, toggled at runtime with F1.
    bool isImGuiVisible() const { return m_imGuiVisible; }
    void setImGuiVisible(bool visible) { m_imGuiVisible = visible; }

    // Upload RGBA pixel data as the video texture sampled by rect lights marked
    // with useVideoTexture = true. Call once per frame after VideoPlayer::update().
    // TODO(RHI): implement via RHI texture upload once rect-light shading is ported.
    void uploadRectLightVideoTexture(const uint8_t* /*rgba*/, uint32_t /*width*/, uint32_t /*height*/) {}

    // Manual flush (for controlling draw order)
    void flush2D();
    void flush3D();

    // Quad drawing (2D)
    void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
    void drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);
    void drawQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    );
    void drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
    void drawQuad2D(
        const glm::mat4& transform,
        TextureHandle texture,
        const glm::vec2* texCoords,
        const glm::vec4& tintColor = glm::vec4(1.0f),
        int entityID = -1
    );

    // Quad drawing (3D - world space with depth)
    void drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);
    void drawQuad3D(
        const glm::vec3& position,
        const glm::vec2& size,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    );
    void drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
    void drawQuad3D(
        const glm::mat4& transform,
        TextureHandle texture,
        const glm::vec2* texCoords,
        const glm::vec4& tintColor = glm::vec4(1.0f),
        int entityID = -1
    );

    // Rotated quad (2D)
    void drawRotatedQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        float rotation,
        const glm::vec4& color
    );
    void drawRotatedQuad2D(
        const glm::vec2& position,
        const glm::vec2& size,
        float rotation,
        TextureHandle texture,
        const glm::vec4& tintColor = glm::vec4(1.0f)
    );

    // Line drawing
    void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness = 1.0f);
    void drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness = 1.0f);

    // Shape drawing
    void drawRect2D(
        const glm::vec2& position,
        const glm::vec2& size,
        const glm::vec4& color,
        float thickness = 1.0f
    );
    void drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32);
    void drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32);
    void drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color);
    void drawTriangleFilled2D(
        const glm::vec2& p0,
        const glm::vec2& p1,
        const glm::vec2& p2,
        const glm::vec4& color
    );

    // Batch statistics
    Batch2DStats getBatch2DStats() const;
    void resetBatch2DStats();

    // ========================================================================
    // Font Rendering API
    // ========================================================================

    FontHandle loadFont(const std::string& path, float baseSize);
    void unloadFont(FontHandle handle);
    void drawText2D(
        FontHandle font,
        const std::string& text,
        const glm::vec2& position,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    );
    void drawText3D(
        FontHandle font,
        const std::string& text,
        const glm::vec3& worldPosition,
        float scale = 1.0f,
        const glm::vec4& color = glm::vec4(1.0f)
    );
    glm::vec2 measureText(FontHandle font, const std::string& text, float scale = 1.0f);
    float getFontLineHeight(FontHandle font, float scale = 1.0f);

    // ========================================================================
    // Render-to-Texture API
    // ========================================================================

    RenderTextureHandle createRenderTexture(const RenderTextureDesc& desc);
    void destroyRenderTexture(RenderTextureHandle handle);
    TextureHandle getRenderTextureAsTexture(RenderTextureHandle handle);
    void renderToTexture(
        RenderTextureHandle target,
        std::shared_ptr<Scene> scene,
        Camera& camera,
        const glm::vec4& clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)
    );
    glm::uvec2 getRenderTextureSize(RenderTextureHandle handle);
    Uint64 registerRenderTextureForUI(RenderTextureHandle handle);

    // ========================================================================
    // Post-Processing API
    // ========================================================================

    void applyBloom(RenderTextureHandle target, float threshold = 1.0f, float strength = 0.5f);
    void applyToneMapping(RenderTextureHandle target, float exposure = 1.0f);
    void applyVignette(RenderTextureHandle target, float strength = 0.3f, float radius = 0.8f);

    // ========================================================================
    // Texture Creation (for sprites/batch rendering)
    // ========================================================================

    TextureHandle createTexture(const std::shared_ptr<Vapor::Image>& img);

    // Update an existing texture's contents in place. The image dimensions and
    // channel count must match those used when the texture was created.
    // Intended for streaming sources (e.g. video playback) that re-upload pixel
    // data every frame without reallocating the GPU texture.
    void updateTexture(TextureHandle handle, const std::shared_ptr<Vapor::Image>& img);

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

    // Last completed frame's numbers, shown in the Engine window
    struct FrameStats {
        Uint32 totalDrawables = 0;
        Uint32 visibleDrawables = 0;
        Uint32 directionalLights = 0;
        Uint32 pointLights = 0;
    } lastFrameStats;

    // ========================================================================
    // Registered Resources
    // ========================================================================

    std::vector<RenderMesh> meshes;
    std::vector<RenderMaterial> materials;
    std::vector<RenderTexture> textures;

    // Texture cache (path -> TextureId)
    std::unordered_map<std::string, TextureId> textureCache;

    // Mapping from drawable index to instance ID (for correct instance data indexing)
    std::unordered_map<Uint32, Uint32> drawableToInstanceID;

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

    // Default textures
    TextureId defaultWhiteTexture = INVALID_TEXTURE_ID;
    TextureId defaultNormalTexture = INVALID_TEXTURE_ID;
    TextureId defaultBlackTexture = INVALID_TEXTURE_ID;

    // Default sampler
    SamplerHandle defaultSampler;

    // Render targets
    TextureHandle colorRT_MSAA;
    TextureHandle colorRT;
    TextureHandle depthStencilRT_MSAA;
    TextureHandle depthStencilRT;
    TextureHandle normalRT_MSAA;
    TextureHandle normalRT;
    TextureHandle shadowRT;
    TextureHandle aoRT;

    // Default depth buffer for swapchain rendering (when not using render targets)
    TextureHandle swapchainDepthBuffer;

    // Graphics pipelines
    PipelineHandle mainPipeline;
    PipelineHandle prePassPipeline;
    PipelineHandle postProcessPipeline;
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    ShaderHandle prePassVertexShader;
    ShaderHandle prePassFragmentShader;
    ShaderHandle postProcessVertexShader;
    ShaderHandle postProcessFragmentShader;

    // Compute pipelines
    ComputePipelineHandle buildClustersPipeline;
    ComputePipelineHandle cullLightsPipeline;
    ComputePipelineHandle tileCullingPipeline;
    ComputePipelineHandle normalResolvePipeline;
    ComputePipelineHandle raytraceShadowPipeline;
    ComputePipelineHandle raytraceAOPipeline;

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
    };

    struct BatchRenderer {
        static constexpr uint32_t MaxQuads = 10000;
        static constexpr uint32_t MaxVertices = MaxQuads * 4;
        static constexpr uint32_t MaxIndices = MaxQuads * 6;

        BufferHandle vertexBuffer;
        BufferHandle indexBuffer;
        PipelineHandle pipeline;
        ShaderHandle vertexShader;
        ShaderHandle fragmentShader;

        std::vector<Vertex2D> vertices;
        std::vector<uint32_t> indices;
        uint32_t quadCount = 0;

        TextureHandle whiteTexture;  // Default white texture for colored quads
        SamplerHandle sampler;       // Sampler used for the batch texture
        // Texture for the quads batched since the last flush. One texture per
        // batch: switching textures flushes the pending quads first.
        TextureHandle currentTexture;

        // Store RHI and current view-projection for auto-flush
        RHI* currentRHI = nullptr;
        glm::mat4 currentViewProj = glm::mat4(1.0f);
        bool canAutoFlush = false;

        // Stats
        uint32_t drawCalls = 0;
        uint32_t totalQuads = 0;

        void init(RHI* rhi, GraphicsBackend backend, bool is3D, TextureHandle defaultTex, SamplerHandle samplerHandle);
        // Set the texture for subsequent quads (invalid = white). Flushes the
        // pending batch when the texture changes.
        void setTexture(TextureHandle texture);
        void shutdown(RHI* rhi);
        void flush(RHI* rhi, const glm::mat4& viewProj);
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
    };

    BatchRenderer batch2D;
    BatchRenderer batch3D;
    Batch2DStats batch2DStats;

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

    // Bloom
    std::vector<TextureHandle> bloomMips;
    ComputePipelineHandle bloomDownsamplePipeline;
    ComputePipelineHandle bloomUpsamplePipeline;
    ShaderHandle bloomDownsampleShader;
    ShaderHandle bloomUpsampleShader;

    // Tone mapping
    ComputePipelineHandle toneMappingPipeline;
    ShaderHandle toneMappingShader;

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
    const Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    const Uint32 MSAA_SAMPLE_COUNT = 4;
    Uint32 maxDirectionalLights = 4;
    Uint32 maxPointLights = 1024;
    Uint32 maxRectLights = 32;

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
    std::function<void()> m_engineWindowCallback;
    std::function<void()> m_imGuiFrameCallback;
    bool m_imGuiVisible = true;

    // Stats
    Uint32 drawCount = 0;
    Uint32 currentInstanceCount = 0;
    Uint32 culledInstanceCount = 0;
};

// ============================================================================
// Factory Functions
// ============================================================================

// Create a Renderer with the specified backend
// The RHI is created internally and owned by the Renderer
std::unique_ptr<Renderer> createRenderer(GraphicsBackend backend, SDL_Window* window);
