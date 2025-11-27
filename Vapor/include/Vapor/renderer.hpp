#pragma once
#include "rhi.hpp"
#include "render_data.hpp"
#include "camera.hpp"
#include "graphics.hpp"
#include <vector>
#include <memory>

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
    MeshId registerMesh(const std::vector<VertexData>& vertices,
                        const std::vector<Uint32>& indices);

    // Register a material and return its ID
    MaterialId registerMaterial(const MaterialDataInput& materialData);

    // Register a texture and return its ID
    TextureId registerTexture(const std::shared_ptr<Image>& image);

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

    // Execute rendering (culling, sorting, draw calls)
    void render();

    // End the frame
    void endFrame();

    // ========================================================================
    // Render Path Management
    // ========================================================================

    void setRenderPath(RenderPath path);
    RenderPath getRenderPath() const { return currentRenderPath; }

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
    TextureId getOrCreateTexture(const std::shared_ptr<Image>& image);
    void bindMaterial(MaterialId materialId);

    // ========================================================================
    // RHI Ownership
    // ========================================================================

    std::unique_ptr<RHI> rhi;

    // ========================================================================
    // Backend Info
    // ========================================================================

    GraphicsBackend backend;  // Store backend for ImGui shutdown

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
    // Configuration
    // ========================================================================

    const Uint32 MAX_INSTANCES = 1000;
    const Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    const Uint32 MSAA_SAMPLE_COUNT = 4;
    Uint32 maxDirectionalLights = 4;
    Uint32 maxPointLights = 256;

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
