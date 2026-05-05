#pragma once
#include "renderer.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <unordered_map>
#include <memory>

#include "graphics.hpp"


class Renderer_Vulkan final : public Renderer {
public:
    Renderer_Vulkan();

    ~Renderer_Vulkan();

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

    VkPipeline createPipeline(const std::string& filename1, const std::string& filename2);
    VkPipeline createRenderPipeline(const std::string& vertShader, const std::string& fragShader);
    VkPipeline createPrePassPipeline(const std::string& vertShader, const std::string& fragShader);
    VkPipeline createPostProcessPipeline(const std::string& vertShader, const std::string& fragShader);

    VkPipeline createComputePipeline(const std::string& filename, VkPipelineLayout layout);

    VkShaderModule createShaderModule(const std::string& code);

    RenderTargetHandle createRenderTarget(RenderTargetUsage usage, VkFormat format);

    TextureHandle createTexture(const std::shared_ptr<Image>& img) override;

    // ===== Interface parity stubs (no functional implementation) =====

    bool initUI() override { return false; }
    std::shared_ptr<Vapor::DebugDraw> getDebugDraw() override { return nullptr; }

    void flush2D() override {}
    void flush3D() override {}

    void drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) override {}
    void drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) override {}
    void drawQuad2D(const glm::vec2& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f)) override {}
    void drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) override {}
    void drawQuad2D(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1) override {}

    void drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) override {}
    void drawQuad3D(const glm::vec3& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f)) override {}
    void drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID = -1) override {}
    void drawQuad3D(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1) override {}

    void drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color) override {}
    void drawRotatedQuad2D(const glm::vec2& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f)) override {}

    void drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness = 1.0f) override {}
    void drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness = 1.0f) override {}

    void drawRect2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness = 1.0f) override {}
    void drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) override {}
    void drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32) override {}
    void drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) override {}
    void drawTriangleFilled2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) override {}

    Batch2DStats getBatch2DStats() const override { return {}; }
    void resetBatch2DStats() override {}

    FontHandle loadFont(const std::string& path, float baseSize) override { return {}; }
    void unloadFont(FontHandle handle) override {}
    void drawText2D(FontHandle font, const std::string& text, const glm::vec2& position, float scale = 1.0f, const glm::vec4& color = glm::vec4(1.0f)) override {}
    void drawText3D(FontHandle font, const std::string& text, const glm::vec3& worldPosition, float scale = 1.0f, const glm::vec4& color = glm::vec4(1.0f)) override {}
    glm::vec2 measureText(FontHandle font, const std::string& text, float scale = 1.0f) override { return {}; }
    float getFontLineHeight(FontHandle font, float scale = 1.0f) override { return 0.0f; }

    BufferHandle createBuffer(BufferUsage usage, VkDeviceSize size);

    BufferHandle createBufferMapped(BufferUsage usage, VkDeviceSize size, void** mappedDataPtr);

    BufferHandle createVertexBuffer(std::vector<VertexData> vertices);

    BufferHandle createIndexBuffer(std::vector<Uint32> indices);

    VkBuffer getBuffer(BufferHandle handle) const;
    VkDeviceMemory getBufferMemory(BufferHandle handle) const;

    VkImage getTexture(TextureHandle handle) const;
    VkImageView getTextureView(TextureHandle handle) const;
    VkDeviceMemory getTextureMemory(TextureHandle handle) const;

    VkImage getRenderTarget(RenderTargetHandle handle) const;
    VkImageView getRenderTargetView(RenderTargetHandle handle) const;
    VkDeviceMemory getRenderTargetMemory(RenderTargetHandle handle) const;

    VkPipeline getPipeline(PipelineHandle handle) const;

private:
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;
    Uint32 graphicsFamilyIdx;
    Uint32 presentFamilyIdx;

    VkSwapchainKHR swapchain;
    VkFormat swapchainImageFormat;
    VkExtent2D swapchainExtent;

    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> renderFences;

    VkPipelineLayout renderPipelineLayout;
    VkPipelineLayout prePassPipelineLayout;
    VkPipelineLayout postProcessPipelineLayout;
    VkPipeline renderPipeline;
    VkPipeline prePassPipeline;
    VkPipeline postProcessPipeline;

    VkPipelineLayout tileCullingPipelineLayout;
    VkPipeline tileCullingPipeline;

    VkRenderPass prePass;
    VkRenderPass renderPass;
    VkRenderPass postProcessPass;
    std::vector<VkFramebuffer> prePassFramebuffers;
    std::vector<VkFramebuffer> renderFramebuffers;
    std::vector<VkFramebuffer> postProcessFramebuffers;

    VkDescriptorPool set0DescriptorPool;
    VkDescriptorPool set1DescriptorPool;
    VkDescriptorPool set2DescriptorPool;
    VkDescriptorSetLayout emptySetLayout; // required because VK_EXT_graphics_pipeline_library not supported
    VkDescriptorSetLayout set0Layout;
    VkDescriptorSetLayout set1Layout;
    VkDescriptorSetLayout set2Layout;
    std::vector<VkDescriptorSet> set0s; // global
    std::vector<VkDescriptorSet> set1s; // 1 set per material
    std::vector<VkDescriptorSet> set2s;

    RenderTargetHandle msaaColorImage;
    RenderTargetHandle msaaDepthImage;
    RenderTargetHandle resolveColorImage;

    TextureHandle defaultAlbedoTexture;
    TextureHandle defaultNormalTexture;
    TextureHandle defaultORMTexture;
    TextureHandle defaultEmissiveTexture;
    // TextureHandle defaultDisplacementTexture;
    VkSampler defaultSampler;

    std::vector<BufferHandle> cameraDataBuffers;
    std::vector<void*> cameraDataBuffersMapped;
    std::vector<BufferHandle> instanceDataBuffers;
    std::vector<void*> instanceDataBuffersMapped;
    std::vector<BufferHandle> directionalLightBuffers;
    std::vector<void*> directionalLightBuffersMapped;
    std::vector<BufferHandle> pointLightBuffers;
    std::vector<void*> pointLightBuffersMapped;
    std::vector<BufferHandle> clusterBuffers;
    std::vector<BufferHandle> lightCullDataBuffers;

    std::vector<InstanceData> instances;

    Uint32 nextBufferID = 0;
    Uint32 nextImageID = 0;
    Uint32 nextPipelineID = 0;
    Uint32 nextInstanceID = 0;
    Uint32 nextMaterialID = 0;
    std::unordered_map<Uint32, VkBuffer> buffers;
    std::unordered_map<Uint32, VkDeviceMemory> bufferMemories;
    std::unordered_map<Uint32, VkImage> images;
    std::unordered_map<Uint32, VkDeviceMemory> imageMemories;
    std::unordered_map<Uint32, VkImageView> imageViews;
    std::unordered_map<Uint32, VkPipeline> pipelines;
    std::unordered_map<std::shared_ptr<Material>, VkDescriptorSet> materialTextureSets;
    std::unordered_map<std::shared_ptr<Material>, Uint32> materialIDs;

    RenderPath currentRenderPath = RenderPath::Forward;

    // Particle system
    static constexpr Uint32 MAX_PARTICLES = 50000;
    bool particleSystemEnabled = true;
    Uint32 particleCount = MAX_PARTICLES;

    VkDescriptorPool particleDescriptorPool;
    VkDescriptorSetLayout particleComputeSetLayout;
    VkDescriptorSetLayout particleRenderSetLayout;
    std::vector<VkDescriptorSet> particleComputeSets;
    std::vector<VkDescriptorSet> particleRenderSets;

    VkPipelineLayout particleComputePipelineLayout;
    VkPipeline particleForcePipeline;
    VkPipeline particleIntegratePipeline;

    VkPipelineLayout particleRenderPipelineLayout;
    VkPipeline particleRenderPipeline;

    std::vector<BufferHandle> particleBuffers;              // Double buffered for compute
    std::vector<BufferHandle> particleSimParamsBuffers;     // Simulation parameters
    std::vector<void*> particleSimParamsBuffersMapped;
    std::vector<BufferHandle> particleAttractorBuffers;     // Attractor data
    std::vector<void*> particleAttractorBuffersMapped;

    void initParticleSystem();
    void updateParticleSimulation(VkCommandBuffer cmd, float deltaTime, const glm::vec3& attractorPos);
    void renderParticles(VkCommandBuffer cmd);
    void cleanupParticleSystem();

    void createResources();
};