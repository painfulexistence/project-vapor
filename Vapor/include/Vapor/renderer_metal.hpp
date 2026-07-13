#pragma once
#include "irenderer.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#include <array>
#include <cmath>
#include <algorithm>
#include <memory>
#include <mutex>
#include <os/signpost.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "debug_draw.hpp"
#include "graphics.hpp"
#include "graphics_gpu_structs.hpp"  // global GPU-layout structs (InstanceData, …) matching the .metal shaders
#include "graphics_batch2d.hpp"  // Batch2DStats, Batch2DVertex, Batch2DBlendMode
#include "graphics_effects.hpp"  // WaterData, VolumetricFogData, VolumetricCloudData, LightScatteringData, SunFlareData, Particle
#include "graphics_gibs.hpp"     // GIBSQuality, GIBSData, Surfel

// Forward declarations
namespace Rml {
    class Context;
}

class Renderer_Metal;
class PrePass;
class TLASBuildPass;
class NormalResolvePass;
class VelocityPass;
class TileCullingPass;
class RaytraceShadowPass;
class RaytraceAOPass;
class AOTemporalPass;
class AODenoisePass;
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

class PSSMShadowPass;
class PSSMResolvePass;
class SSCSPass;
class StochasticPointShadowPass;
class PointShadowTemporalPass;

// GIBS forward declarations
namespace Vapor { class GIBSManager; }
class SurfelGenerationPass;
class SurfelHashBuildPass;
class SurfelRaytracingPass;
class GIBSTemporalPass;
class GIBSSamplePass;

struct MetalGpuPassTiming {
    std::string name;
    double gpuTimeMs = 0.0;
    uint64_t estimatedBytes = 0; // estimated minimum memory traffic (attachment load/store + pass-reported)
};

// Signpost log for render pass intervals — shows up in Instruments' Points of
// Interest track so passes can be matched by name against the GPU timeline.
inline os_log_t vaporRenderGraphLog() {
    static os_log_t log = os_log_create("com.projectvapor.rendergraph", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    return log;
}

inline uint64_t vaporPixelFormatBytes(MTL::PixelFormat fmt) {
    switch (fmt) {
        case MTL::PixelFormatR8Unorm: return 1;
        case MTL::PixelFormatR16Float:
        case MTL::PixelFormatRG8Unorm: return 2;
        case MTL::PixelFormatDepth32Float_Stencil8: return 5;
        case MTL::PixelFormatRG16Float:
        case MTL::PixelFormatRGBA16Float: return 8;
        case MTL::PixelFormatRG32Float: return 8;
        case MTL::PixelFormatRGBA32Float: return 16;
        default: return 4; // RGBA8/BGRA8(+sRGB), R32Float, RG11B10, RGB10A2, Depth32Float, ...
    }
}

// Minimum bytes an attachment moves between tile and device memory, given its
// load/store actions. MS textures load/store sampleCount layers; a resolve
// writes one.
inline uint64_t vaporAttachmentTrafficBytes(const MTL::Texture* tex, MTL::LoadAction load, MTL::StoreAction store) {
    uint64_t bytes1x = static_cast<uint64_t>(tex->width()) * tex->height() * vaporPixelFormatBytes(tex->pixelFormat());
    uint64_t sc = tex->sampleCount();
    uint64_t total = 0;
    if (load == MTL::LoadActionLoad) total += bytes1x * sc;
    if (store == MTL::StoreActionStore || store == MTL::StoreActionStoreAndMultisampleResolve) total += bytes1x * sc;
    if (store == MTL::StoreActionMultisampleResolve || store == MTL::StoreActionStoreAndMultisampleResolve) total += bytes1x;
    return total;
}

class MetalRenderPass {
public:
    explicit MetalRenderPass(Renderer_Metal* renderer) : renderer(renderer) {}
    virtual ~MetalRenderPass() = default;
    virtual void execute() = 0;
    virtual const char* getName() const = 0;

    bool enabled = true;

    // Timing context — set by MetalRenderGraph before each execute().
    // Slot layout: beginIdx = end slot of previous pass (or frame-start slot for first pass).
    //              endIdx   = end slot of this pass.
    // beginIdx[N] == endIdx[N-1], guaranteed by MetalRenderGraph — no prevEnd heuristic needed.
    MTL::CounterSampleBuffer* m_timingSampleBuf = nullptr;
    NS::UInteger m_timingBeginIdx = MTL::CounterDontSample;
    NS::UInteger m_timingEndIdx   = MTL::CounterDontSample;
    // True only for the first pass that actually runs; it is responsible for sampling the frame-start slot.
    bool m_sampleBegin = false;
    // Set to true by a timing helper when wantEnd=true (end slot written); stays false on early-return.
    bool m_didWriteTimingSamples = false;
    // Estimated minimum memory traffic this frame. Render passes accumulate attachment
    // load/store traffic automatically via applyTimingToRenderDesc; compute/blit passes
    // report their texture/buffer traffic manually via addTrafficEstimate. Reset by
    // MetalRenderGraph before each execute(). Excludes texture sampling unless reported.
    uint64_t m_estimatedBytes = 0;

    void addTrafficEstimate(uint64_t bytes) {
        m_estimatedBytes += bytes;
    }

    // Attach counter-sample endpoints to an existing MTLRenderPassDescriptor.
    // wantBegin: first encoder of a multi-encoder pass (suppressed automatically for non-first passes).
    // wantEnd:   last encoder of this pass; setting this marks the pass as having written samples.
    void applyTimingToRenderDesc(MTL::RenderPassDescriptor* desc, bool wantBegin, bool wantEnd) {
        if (!m_timingSampleBuf) return;
        NS::UInteger startIdx = (wantBegin && m_sampleBegin) ? m_timingBeginIdx : MTL::CounterDontSample;
        NS::UInteger endIdx   = wantEnd ? m_timingEndIdx : MTL::CounterDontSample;
        if (startIdx != MTL::CounterDontSample || endIdx != MTL::CounterDontSample) {
            auto* att = desc->sampleBufferAttachments()->object(0);
            att->setSampleBuffer(m_timingSampleBuf);
            att->setStartOfVertexSampleIndex(startIdx);
            att->setEndOfFragmentSampleIndex(endIdx);
        }
        if (wantEnd) m_didWriteTimingSamples = true;
        for (int i = 0; i < 8; ++i) {
            auto* color = desc->colorAttachments()->object(i);
            if (!color->texture()) continue;
            m_estimatedBytes += vaporAttachmentTrafficBytes(color->texture(), color->loadAction(), color->storeAction());
        }
        auto* depth = desc->depthAttachment();
        if (depth->texture()) {
            m_estimatedBytes += vaporAttachmentTrafficBytes(depth->texture(), depth->loadAction(), depth->storeAction());
        }
    }

    NS::SharedPtr<MTL::ComputePassDescriptor> makeTimedComputeDesc(bool wantBegin, bool wantEnd) {
        auto desc = NS::TransferPtr(MTL::ComputePassDescriptor::computePassDescriptor());
        if (m_timingSampleBuf) {
            NS::UInteger startIdx = (wantBegin && m_sampleBegin) ? m_timingBeginIdx : MTL::CounterDontSample;
            NS::UInteger endIdx   = wantEnd ? m_timingEndIdx : MTL::CounterDontSample;
            if (startIdx != MTL::CounterDontSample || endIdx != MTL::CounterDontSample) {
                auto* att = desc->sampleBufferAttachments()->object(0);
                att->setSampleBuffer(m_timingSampleBuf);
                att->setStartOfEncoderSampleIndex(startIdx);
                att->setEndOfEncoderSampleIndex(endIdx);
            }
            if (wantEnd) m_didWriteTimingSamples = true;
        }
        return desc;
    }

    NS::SharedPtr<MTL::BlitPassDescriptor> makeTimedBlitDesc(bool wantBegin, bool wantEnd) {
        auto desc = NS::TransferPtr(MTL::BlitPassDescriptor::blitPassDescriptor());
        if (m_timingSampleBuf) {
            NS::UInteger startIdx = (wantBegin && m_sampleBegin) ? m_timingBeginIdx : MTL::CounterDontSample;
            NS::UInteger endIdx   = wantEnd ? m_timingEndIdx : MTL::CounterDontSample;
            if (startIdx != MTL::CounterDontSample || endIdx != MTL::CounterDontSample) {
                auto* att = desc->sampleBufferAttachments()->object(0);
                att->setSampleBuffer(m_timingSampleBuf);
                att->setStartOfEncoderSampleIndex(startIdx);
                att->setEndOfEncoderSampleIndex(endIdx);
            }
            if (wantEnd) m_didWriteTimingSamples = true;
        }
        return desc;
    }

    NS::SharedPtr<MTL::AccelerationStructurePassDescriptor> makeTimedAccelDesc(bool wantBegin, bool wantEnd) {
        auto desc = NS::TransferPtr(MTL::AccelerationStructurePassDescriptor::accelerationStructurePassDescriptor());
        if (m_timingSampleBuf) {
            NS::UInteger startIdx = (wantBegin && m_sampleBegin) ? m_timingBeginIdx : MTL::CounterDontSample;
            NS::UInteger endIdx   = wantEnd ? m_timingEndIdx : MTL::CounterDontSample;
            if (startIdx != MTL::CounterDontSample || endIdx != MTL::CounterDontSample) {
                auto* att = desc->sampleBufferAttachments()->object(0);
                att->setSampleBuffer(m_timingSampleBuf);
                att->setStartOfEncoderSampleIndex(startIdx);
                att->setEndOfEncoderSampleIndex(endIdx);
            }
            if (wantEnd) m_didWriteTimingSamples = true;
        }
        return desc;
    }

protected:
    Renderer_Metal* renderer;
};

class MetalRenderGraph {
public:
    struct PassSampleInfo {
        std::string name;
        NS::UInteger beginIdx;
        NS::UInteger endIdx;
        uint64_t estimatedBytes;
    };

    void addPass(std::unique_ptr<MetalRenderPass> pass) {
        passes.push_back(std::move(pass));
    }

    void execute(MTL::CommandBuffer* cmd = nullptr, MTL::CounterSampleBuffer* sampleBuf = nullptr) {
        passTimingInfo.clear();
        // nextSlot starts at 0 (frame-start slot).
        // It advances by 1 only when a pass actually writes its end sample, so:
        //   slot 0        = frame start (written by the first pass via wantBegin && m_sampleBegin)
        //   slot K        = end of the K-th pass that ran (= begin of the K+1-th pass)
        // PassSampleInfo stores (beginIdx=K, endIdx=K+1), so beginIdx[N] == endIdx[N-1] by construction.
        // The completion handler can therefore compute end-begin directly with no heuristics.
        NS::UInteger nextSlot = 0;
        bool needFrameStart = true;
        os_log_t spLog = vaporRenderGraphLog();
        for (auto& pass : passes) {
            if (!pass->enabled) continue;
            if (cmd && sampleBuf) {
                pass->m_timingSampleBuf       = sampleBuf;
                pass->m_timingBeginIdx        = nextSlot;
                pass->m_timingEndIdx          = nextSlot + 1;
                pass->m_sampleBegin           = needFrameStart;
                pass->m_didWriteTimingSamples = false;
                pass->m_estimatedBytes        = 0;
            }
            const char* passName = pass->getName();
            // Debug group: names this pass's encoders in Xcode GPU captures.
            if (cmd) cmd->pushDebugGroup(NS::String::string(passName, NS::UTF8StringEncoding));
            // Signpost: names this pass in Instruments (Points of Interest track).
            // Marks CPU encode time; match against the GPU track via the debug groups.
            os_signpost_id_t spid = os_signpost_id_make_with_pointer(spLog, pass.get());
            os_signpost_interval_begin(spLog, spid, "MetalRenderPass", "%{public}s", passName);
            pass->execute();
            os_signpost_interval_end(spLog, spid, "MetalRenderPass");
            if (cmd) cmd->popDebugGroup();
            if (cmd && sampleBuf) {
                if (pass->m_didWriteTimingSamples) {
                    passTimingInfo.push_back({passName, nextSlot, nextSlot + 1, pass->m_estimatedBytes});
                    nextSlot++;            // this pass's end slot becomes next pass's begin slot
                    needFrameStart = false;
                }
                pass->m_timingSampleBuf = nullptr;
            }
        }
    }

    void clear() {
        passes.clear();
    }

    std::vector<PassSampleInfo> passTimingInfo;

private:
    std::vector<std::unique_ptr<MetalRenderPass>> passes;
};


class EquirectToCubemapPass;

class Renderer_Metal final : public IRenderer {// Must be public or factory function won't work
    friend class PrePass;
    friend class TLASBuildPass;
    friend class NormalResolvePass;
    friend class VelocityPass;
    friend class TileCullingPass;
    friend class RaytraceShadowPass;
    friend class RaytraceAOPass;
    friend class AOTemporalPass;
    friend class AODenoisePass;
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
    friend class PSSMShadowPass;
    friend class PSSMResolvePass;
    friend class SSCSPass;
    friend class StochasticPointShadowPass;
    friend class PointShadowTemporalPass;
    friend class EquirectToCubemapPass;

    // GIBS (Global Illumination Based on Surfels) passes
    friend class SurfelGenerationPass;
    friend class SurfelHashBuildPass;
    friend class SurfelRaytracingPass;
    friend class GIBSTemporalPass;
    friend class GIBSSamplePass;

public:
    Renderer_Metal();

    ~Renderer_Metal();

    // Native lifecycle (called by the createRendererMetal factory). Not part of
    // IRenderer; shutdown() below is the polymorphic entry point.
    void init(SDL_Window* window);
    void deinit();
    void shutdown() override { deinit(); }

    virtual void stage(std::shared_ptr<Scene> scene) override;

    // Frame model matching IRenderer / the RHI renderer:
    //   beginFrame()  → acquire drawable + command buffer, backend ImGui NewFrame
    //   (caller: ImGui::NewFrame())
    //   invokeImGuiCallback() → engine debug panel + app/engine callbacks
    //   draw()        → run the render passes (no ImGui pass, no present)
    //   (caller: ImGui::Render())
    //   endFrame()    → render ImGui draw data + present
    void beginFrame(const CameraRenderData& camera) override;
    void invokeImGuiCallback() override;
    void endFrame() override;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) override;
    virtual void draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera) override;

    virtual void readPixelsAsync(ScreenshotCallback callback) override;
    void uploadRectLightVideoTexture(const uint8_t* rgba, uint32_t width, uint32_t height) override;

    // IBL source: load an equirectangular .hdr file as the environment map.
    // After calling this the sky atmosphere is no longer used for IBL.
    // Place your .hdr files under: <assets>/textures/env/
    void loadHDRI(const std::string& path);

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

    // Batch statistics (native Metal renderer's own type; not part of IRenderer).
    Batch2DStats getBatch2DStats() const {
        return batch2DStats;
    }
    void resetBatch2DStats() {
        batch2DStats = {};
    }

    NS::SharedPtr<MTL::RenderPipelineState>
        createPipeline(const std::string& filename, bool isHDR, bool isColorOnly, Uint32 sampleCount);
    NS::SharedPtr<MTL::ComputePipelineState> createComputePipeline(const std::string& filename);
    NS::SharedPtr<MTL::ComputePipelineState> createComputePipeline(const std::string& filename, const std::string& functionName);

    TextureHandle createTexture(const std::shared_ptr<Vapor::Image>& img) override;
    void updateTexture(TextureHandle handle, const std::shared_ptr<Vapor::Image>& img) override;

    // ===== Render-to-Texture API =====
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

    // Render texture post-processing
    void applyBloom(RenderTextureHandle target, float threshold = 1.0f, float strength = 0.5f) override;
    void applyToneMapping(RenderTextureHandle target, float exposure = 1.0f) override;
    void applyVignette(RenderTextureHandle target, float strength = 0.3f, float radius = 0.8f) override;

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

    BufferHandle createVertexBuffer(const std::vector<Vapor::VertexData>& vertices);
    BufferHandle createIndexBuffer(const std::vector<Uint32>& indices);
    BufferHandle createStorageBuffer(const std::vector<Vapor::VertexData>& vertices);

    NS::SharedPtr<MTL::Buffer> getBuffer(BufferHandle handle) const;
    NS::SharedPtr<MTL::Texture> getTexture(TextureHandle handle) const;
    NS::SharedPtr<MTL::RenderPipelineState> getPipeline(PipelineHandle handle) const;

protected:
    MetalRenderGraph graph;

    // Configuration constants + frame state (previously provided by the shared
    // Renderer base; IRenderer no longer carries these, so the native Metal
    // renderer owns them directly).
    const Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    const Uint32 MSAA_SAMPLE_COUNT = 4;
    const Uint32 MAX_INSTANCES = 5000;// Increased for large scenes like Bistro
    const Uint32 MAX_DIRECTIONAL_LIGHTS = 4;
    const Uint32 MAX_POINT_LIGHTS = 1024;
    const Uint32 MAX_RECT_LIGHTS = 32;
    glm::vec4 clearColor = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f);
    double clearDepth = 1.0;
    Uint32 clusterGridSizeX = 16;
    Uint32 clusterGridSizeY = 16;
    Uint32 clusterGridSizeZ = 24;
    Uint32 numClusters = clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;
    Uint32 currentFrameInFlight = 0;
    Uint32 frameNumber = 0;
    bool isInitialized = false;
    int calculateMipmapLevelCount(Uint32 width, Uint32 height) const {
        return static_cast<int>(std::floor(std::log2(std::max(width, height))) + 1);
    }

    // GPU pass timing (Apple Silicon, MTLCounterSampleBuffer timestamps)
    static constexpr NS::UInteger GPU_TIMER_SAMPLE_COUNT = 64;
    NS::SharedPtr<MTL::CounterSampleBuffer> gpuTimerSampleBuffer;
    std::vector<MetalGpuPassTiming> gpuPassTimings;
    std::mutex gpuTimingMutex;
    bool gpuTimingSupported = false;
    bool gpuTimingEnabled = false;

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
    enum class IBLSource { Sky, HDRI };
    IBLSource iblSource = IBLSource::Sky;

    NS::SharedPtr<MTL::RenderPipelineState> prePassPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> drawPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> iridescentPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> equirectToCubemapPipeline;
    NS::SharedPtr<MTL::RenderPipelineState> postProcessPipeline;

    NS::SharedPtr<MTL::ComputePipelineState> buildClustersPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> cullLightsPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> tileCullingPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> normalResolvePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> velocityPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> raytraceShadowPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> raytraceAOPipeline; // 3d_raytrace_ao.metal
    NS::SharedPtr<MTL::ComputePipelineState> ssaoPipeline;       // 3d_ssao.metal (same bindings, no TLAS)
    NS::SharedPtr<MTL::ComputePipelineState> aoTemporalPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> aoDenoisePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> stochasticPointShadowPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> pointShadowTemporalPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> pssmResolvePipeline;
    NS::SharedPtr<MTL::RenderPipelineState> pssmShadowPipeline;
    NS::SharedPtr<MTL::DepthStencilState> pssmDepthStencilState;
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
    // When texture slots overflow (>16 unique textures), the current batch is
    // saved to batch2DSubBatches and a new batch starts automatically.
    struct Batch2DSubBatch {
        std::vector<Batch2DVertex>      vertices;
        std::vector<Uint32>             indices;
        std::array<TextureHandle, 16>   textureSlots;
        Uint32                          textureSlotCount = 1;
    };
    std::vector<Batch2DSubBatch> batch2DSubBatches;
    std::vector<Batch2DVertex> batch2DVertices;
    std::vector<Uint32> batch2DIndices;
    std::array<TextureHandle, 16> batch2DTextureSlots;
    Uint32 batch2DTextureSlotIndex = 1;// 0 = white texture
    glm::mat4 batch2DProjection = glm::mat4(1.0f);
    Batch2DBlendMode batch2DBlendMode = Batch2DBlendMode::Alpha;
    Batch2DStats batch2DStats;
    bool batch2DActive = false;

    void splitBatch2D(); // flush current batch into sub-batches, reset slots

    // 3D Batch CPU-side state (world space, with depth)
    std::vector<Batch2DVertex> batch3DVertices;
    std::vector<Uint32> batch3DIndices;
    std::array<TextureHandle, 16> batch3DTextureSlots;
    Uint32 batch3DTextureSlotIndex = 1;
    glm::mat4 batch3DProjection = glm::mat4(1.0f);
    Batch2DBlendMode batch3DBlendMode = Batch2DBlendMode::Alpha;
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
    NS::SharedPtr<MTL::Buffer> rectLightBuffer;
    NS::SharedPtr<MTL::Texture> rectLightVideoTexture; // updated each frame via uploadRectLightVideoTexture
    NS::SharedPtr<MTL::Buffer> materialDataBuffer;
    NS::SharedPtr<MTL::Buffer> atmosphereDataBuffer;
    NS::SharedPtr<MTL::Buffer> iblCaptureDataBuffer;
    std::vector<NS::SharedPtr<MTL::Buffer>> clusterBuffers;

    // Light scattering (God Rays) resources
    std::vector<NS::SharedPtr<MTL::Buffer>> lightScatteringDataBuffers;
    NS::SharedPtr<MTL::Texture> lightScatteringRT;// Half-resolution scattering texture
    bool lightScatteringEnabled = true;
    // AO toggle: skips the whole AO chain and binds a white texture in its place
    bool aoEnabled = true;
    // Raygen method for the AO chain: 0 = ray traced, 1 = screen space (see AO ImGui section)
    int aoMethod = 0;
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
    bool m_supportsRaytracing = false;
    VolumetricCloudData volumetricCloudSettings;

    // Sun Flare resources
    NS::SharedPtr<MTL::RenderPipelineState> sunFlarePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> sunOcclusionPipeline;
    std::vector<NS::SharedPtr<MTL::Buffer>> sunFlareDataBuffers;
    NS::SharedPtr<MTL::Buffer> sunVisibilityBuffer;// Single float for occlusion result
    bool sunFlareEnabled = false;
    SunFlareData sunFlareSettings;

    // IBL textures
    NS::SharedPtr<MTL::Texture> environmentCubemap;  // Captured sky cubemap (or converted HDRI)
    NS::SharedPtr<MTL::Texture> equirectHDRITexture; // Loaded equirectangular HDRI (2D, RGBA32Float)
    NS::SharedPtr<MTL::Texture> irradianceMap;       // Diffuse irradiance cubemap
    NS::SharedPtr<MTL::Texture> prefilterMap;        // Pre-filtered specular cubemap (with mipmaps)
    NS::SharedPtr<MTL::Texture> brdfLUT;             // BRDF integration LUT
    bool iblNeedsUpdate = true;
    std::vector<NS::SharedPtr<MTL::Buffer>> accelInstanceBuffers;
    std::vector<NS::SharedPtr<MTL::Buffer>> TLASScratchBuffers;
    std::vector<NS::SharedPtr<MTL::AccelerationStructure>> TLASBuffers;

    // Instance data
    // instanceBatches: material → list of (mesh, instanceArrayIndex) for rasterization draw calls
    struct MeshDraw { std::shared_ptr<Vapor::Mesh> mesh; uint32_t instanceIndex; };
    std::vector<::InstanceData> instances;
    std::vector<::InstanceData> pendingEcsInstances;
    std::unordered_map<std::shared_ptr<Vapor::Material>, std::vector<MeshDraw>> pendingEcsBatches;
    std::vector<MTL::AccelerationStructureInstanceDescriptor> pendingEcsAccelInstances;
    std::vector<MTL::AccelerationStructureInstanceDescriptor> accelInstances;
    std::unordered_map<std::shared_ptr<Vapor::Material>, std::vector<MeshDraw>> instanceBatches;

    // Render targets
    NS::SharedPtr<MTL::Texture> colorRT_MS;
    NS::SharedPtr<MTL::Texture> colorRT;
    NS::SharedPtr<MTL::Texture> tempColorRT;// For ping-pong post-processing (fog, clouds)
    NS::SharedPtr<MTL::Texture> depthStencilRT_MS;
    NS::SharedPtr<MTL::Texture> depthStencilRT;
    NS::SharedPtr<MTL::Texture> normalRT_MS;
    NS::SharedPtr<MTL::Texture> normalRT;
    NS::SharedPtr<MTL::Texture> albedoRT_MS;  // PrePass albedo output (for GIBS)
    NS::SharedPtr<MTL::Texture> albedoRT;
    NS::SharedPtr<MTL::Texture> shadowRT;
    NS::SharedPtr<MTL::Texture> shadowRTGrayView; // swizzle view (r,r,r,1) for ImGui preview
    // Screen-space contact shadows, min-composited onto the directional shadow.
    NS::SharedPtr<MTL::Texture> sscsRT;
    NS::SharedPtr<MTL::Texture> sscsRTGrayView;
    NS::SharedPtr<MTL::ComputePipelineState> sscsPipeline;
    NS::SharedPtr<MTL::Texture> pointShadowRT;       // R16F, raw stochastic point shadow
    NS::SharedPtr<MTL::Texture> pointShadowDenoisedRT; // R16F, temporally denoised
    NS::SharedPtr<MTL::Texture> pointShadowHistoryRT;  // R16F, history for temporal
    NS::SharedPtr<MTL::Texture> pointShadowRTGrayView;        // swizzle (r,r,r,1) for ImGui
    NS::SharedPtr<MTL::Texture> pointShadowDenoisedRTGrayView; // swizzle (r,r,r,1) for ImGui
    NS::SharedPtr<MTL::Texture> aoRT;
    NS::SharedPtr<MTL::Texture> velocityRT; // RG16Float camera-motion vectors (see 3d_velocity.metal)
    glm::mat4 prevViewProj = glm::mat4(1.0f);
    bool prevViewProjValid = false;

    NS::SharedPtr<MTL::Texture> aoRTGrayView;     // swizzle view (r,r,r,1) of aoRT for ImGui preview
    // AO denoise chain (raygen → temporal → à-trous → aoRT), full res for now (ADR-008)
    NS::SharedPtr<MTL::Texture> aoRawRT;          // R16Float, noisy raygen output
    NS::SharedPtr<MTL::Texture> aoHistoryRT[2];   // RGBA16F ping-pong: (ao, view-space depth, oct normal)
    NS::SharedPtr<MTL::Texture> aoScratchRT;      // RGBA16F, à-trous intermediate
    uint32_t aoHistoryIndex = 0;                  // aoHistoryRT[aoHistoryIndex] holds the latest history
    bool aoHistoryValid = false;
    glm::mat4 prevView = glm::mat4(1.0f);
    bool prevViewValid = false;

    // PSSM shadow maps: 2D texture array, 3 cascades × 4096×4096 Depth32
    NS::SharedPtr<MTL::Texture> pssmShadowMaps;
    // Per-slice texture2d views used only for ImGui display
    std::array<NS::SharedPtr<MTL::Texture>, 3> pssmShadowMapViews;
    // Screen-space resolved PSSM shadow (camera-aligned, for intuitive debug display)
    NS::SharedPtr<MTL::Texture> pssmShadowScreenRT;
    NS::SharedPtr<MTL::Texture> pssmShadowScreenRTGrayView; // swizzle (r,r,r,1) for ImGui
    std::vector<NS::SharedPtr<MTL::Buffer>> pssmDataBuffers;
    static constexpr uint32_t PSSM_CASCADE_COUNT = 3;
    static constexpr uint32_t PSSM_SHADOW_MAP_SIZE = 4096;
    float pssmRTMaxDist = 50.0f; // view-space depth where RT shadow ends and PSSM begins

    // PSSM PCF and blend settings
    float pssmRTBlendScale = 0.05f;       // RT↔PSSM cross-fade width as a fraction of (far - rtEnd)
    float pssmCascadeBlendRange = 10.0f;  // blend range between PSSM cascades (view-space units)
    uint32_t pssmPcfSampleCount = 16;     // PCF sample count: 4, 8, 16, or 32
    bool pssmDebugVisualize = false;      // visualize cascade regions with colors
    // Screen-space contact shadow settings (opt-in: enable in the Shadow Debug panel)
    bool sscsEnabled = false;
    float sscsLength = 0.3f;      // view-space march distance (contact scale, metres)
    float sscsThickness = 0.3f;   // occluder depth window
    uint32_t sscsSteps = 12;
    float sscsBias = 0.02f;

    // Stochastic point shadow debug: 0 = visibility, 1 = tile light-count heatmap
    uint32_t pointShadowDebugMode = 0;
    // Perf-isolation flags for the shared PBR shader (buffer 12): bit0 = skip
    // point-light loop, bit1 = skip shadow. Mirrors the RHI path's mainDebugFlags.
    uint32_t mainDebugFlags = 0;

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

    // ===== GIBS (Global Illumination Based on Surfels) =====
    std::unique_ptr<Vapor::GIBSManager> gibsManager;
    // Experimental — off by default; toggle at runtime via the ImGui
    // "Global Illumination (GIBS)" section. See docs/GIBS_DESIGN.md.
    bool gibsEnabled = false;
    GIBSQuality gibsQuality = GIBSQuality::Low;

    // GIBS Compute Pipelines
    NS::SharedPtr<MTL::ComputePipelineState> surfelGenerationPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> surfelClearCellHeadsPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> surfelInsertPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> surfelRaytracingPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> surfelRaytracingSimplePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> gibsTemporalPipeline;
    NS::SharedPtr<MTL::ComputePipelineState> gibsSamplePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> gibsUpsamplePipeline;
    NS::SharedPtr<MTL::ComputePipelineState> gibsCompositePipeline;

    // Acceleration structures for ray tracing
    std::vector<NS::SharedPtr<MTL::AccelerationStructure>> BLASs;
    NS::SharedPtr<NS::Array> BLASArray;

    // Rendering statistics
    Uint32 currentInstanceCount = 0;
    Uint32 culledInstanceCount = 0;
    Uint32 drawCount = 0;

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
    std::unordered_map<std::shared_ptr<Vapor::Material>, Uint32> materialIDs;

    // Render texture internal data
    struct RenderTextureData {
        NS::SharedPtr<MTL::Texture> colorTexture;
        NS::SharedPtr<MTL::Texture> tempTexture;// For ping-pong post-processing
        NS::SharedPtr<MTL::Texture> depthTexture;
        TextureHandle textureHandle;// Handle for using as sampler texture
        Uint32 width = 0;
        Uint32 height = 0;
        bool hdr = false;
        Uint32 sampleCount = 1;
    };
    Uint32 nextRenderTextureID = 0;
    std::unordered_map<Uint32, RenderTextureData> renderTextures;

    RenderPath currentRenderPath = RenderPath::Forward;

    // UI rendering (using void* for pimpl idiom to hide implementation)
    void* m_uiRenderer = nullptr;
    Rml::Context* m_uiContext = nullptr;
    std::vector<ScreenshotCallback> m_pendingScreenshots;

    // Font rendering
    FontManager m_fontManager;

    void createResources();
    void renderUI();// Internal method called by RmlUiPass
};