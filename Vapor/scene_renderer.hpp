#pragma once
#include "rhi.hpp"
#include "render_data.hpp"
#include "camera.hpp"
#include <vector>
#include <memory>

// ============================================================================
// SceneRenderer - High-level renderer that uses RHI
//
// Responsibilities:
// - Manage rendering resources (meshes, materials, textures)
// - Collect drawables each frame
// - Perform culling and sorting
// - Execute draw calls via RHI
// ============================================================================

class SceneRenderer {
public:
    SceneRenderer() = default;
    ~SceneRenderer() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    void initialize(RHI* rhi);
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
    // Getters
    // ========================================================================

    RHI* getRHI() const { return rhi; }

private:
    // ========================================================================
    // Internal Rendering Steps
    // ========================================================================

    void performCulling();
    void sortDrawables();
    void updateBuffers();
    void executeDrawCalls();
    void createDefaultResources();
    void createRenderPipeline();

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    Frustum extractFrustum(const glm::mat4& viewProj);
    TextureId getOrCreateTexture(const std::shared_ptr<Image>& image);
    void bindMaterial(MaterialId materialId);

    // ========================================================================
    // RHI Reference
    // ========================================================================

    RHI* rhi = nullptr;

    // ========================================================================
    // Registered Resources
    // ========================================================================

    std::vector<RenderMesh> meshes;
    std::vector<RenderMaterial> materials;
    std::vector<RenderTexture> textures;

    // Texture cache (path -> TextureId)
    std::unordered_map<std::string, TextureId> textureCache;

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

    // Default textures
    TextureId defaultWhiteTexture = INVALID_TEXTURE_ID;
    TextureId defaultNormalTexture = INVALID_TEXTURE_ID;
    TextureId defaultBlackTexture = INVALID_TEXTURE_ID;

    // Default sampler
    SamplerHandle defaultSampler;

    // Pipeline
    PipelineHandle mainPipeline;
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;

    // ========================================================================
    // Configuration
    // ========================================================================

    const Uint32 MAX_INSTANCES = 1000;
    Uint32 maxDirectionalLights = 4;
    Uint32 maxPointLights = 256;
};
