#ifdef __APPLE__

#include "ui_renderer.hpp"
#include "rmlui_system.hpp"
#include "helper.hpp"
#include "Vapor/file_system.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <RmlUi/Core.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <memory>
#include <string>
#include <unordered_map>

namespace Vapor {

// ── RmlUiMetal ───────────────────────────────────────────────────────────────
// Rml::RenderInterface backed by metal-cpp.
// Self-contained — no dependency on Renderer_Metal or SDL.

class RmlUiMetal : public Rml::RenderInterface {
public:
    explicit RmlUiMetal(MTL::Device* device) : m_device(device) {
        m_transform = Rml::Matrix4f::Identity();
    }

    ~RmlUiMetal() override { shutdown(); }

    bool initialize() {
        if (!m_device) return false;
        createDefaultWhiteTexture();
        createPipelineState();
        return m_pipelineState.get() != nullptr;
    }

    void shutdown() {
        m_geometry.clear();
        m_textures.clear();
        m_pipelineState.reset();
        m_depthStencilState.reset();
        m_defaultWhiteTexture.reset();
    }

    void beginFrame(MTL::CommandBuffer* cmd, MTL::Texture* target, int logicalW, int logicalH) {
        if (!cmd || !target) return;

        m_logicalWidth  = logicalW;
        m_logicalHeight = logicalH;

        int fbW = static_cast<int>(target->width());
        int fbH = static_cast<int>(target->height());
        m_scaleX = logicalW > 0 ? static_cast<float>(fbW) / logicalW : 1.0f;
        m_scaleY = logicalH > 0 ? static_cast<float>(fbH) / logicalH : 1.0f;

        m_currentCommandBuffer = cmd;
        m_currentRenderTarget  = target;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto ca = passDesc->colorAttachments()->object(0);
        ca->setTexture(target);
        ca->setLoadAction(MTL::LoadActionClear);
        ca->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        ca->setStoreAction(MTL::StoreActionStore);
        m_currentPassDesc = std::move(passDesc);

        m_currentEncoder = cmd->renderCommandEncoder(m_currentPassDesc.get());

        MTL::Viewport vp;
        vp.originX = 0; vp.originY = 0;
        vp.width   = fbW; vp.height = fbH;
        vp.znear   = 0;   vp.zfar   = 1;
        m_currentEncoder->setViewport(vp);

        MTL::ScissorRect sr;
        sr.x = 0; sr.y = 0;
        sr.width  = static_cast<NS::UInteger>(fbW);
        sr.height = static_cast<NS::UInteger>(fbH);
        m_currentEncoder->setScissorRect(sr);
    }

    void endFrame() {
        if (m_currentEncoder) {
            m_currentEncoder->endEncoding();
            m_currentEncoder = nullptr;
        }
        m_currentCommandBuffer = nullptr;
        m_currentRenderTarget  = nullptr;
        m_currentPassDesc.reset();
    }

    // ── Rml::RenderInterface ─────────────────────────────────────────────────

    Rml::CompiledGeometryHandle CompileGeometry(
        Rml::Span<const Rml::Vertex> vertices,
        Rml::Span<const int>         indices) override
    {
        if (!m_device) return 0;

        auto vb = NS::TransferPtr(m_device->newBuffer(
            vertices.size() * sizeof(Rml::Vertex), MTL::ResourceStorageModeShared));
        memcpy(vb->contents(), vertices.data(), vertices.size() * sizeof(Rml::Vertex));

        auto ib = NS::TransferPtr(m_device->newBuffer(
            indices.size() * sizeof(int), MTL::ResourceStorageModeShared));
        memcpy(ib->contents(), indices.data(), indices.size() * sizeof(int));

        Rml::CompiledGeometryHandle h = m_nextGeom++;
        m_geometry[h] = { vb, ib, static_cast<NS::UInteger>(indices.size()) };
        return h;
    }

    void RenderGeometry(
        Rml::CompiledGeometryHandle geometry,
        Rml::Vector2f               translation,
        Rml::TextureHandle          texture) override
    {
        if (!m_currentEncoder || !m_pipelineState) return;

        auto it = m_geometry.find(geometry);
        if (it == m_geometry.end()) return;
        const auto& geom = it->second;

        m_currentEncoder->setRenderPipelineState(m_pipelineState.get());
        m_currentEncoder->setDepthStencilState(m_depthStencilState.get());
        m_currentEncoder->setCullMode(MTL::CullModeNone);

        glm::mat4 proj  = glm::ortho(0.0f, (float)m_logicalWidth, (float)m_logicalHeight, 0.0f, -1.0f, 1.0f);
        glm::mat4 xform = glm::make_mat4(m_transform.data());
        xform = glm::translate(xform, glm::vec3(translation.x, translation.y, 0.0f));

        struct Uniforms { glm::mat4 proj; glm::mat4 xform; } u;
        u.proj = proj; u.xform = xform;
        m_currentEncoder->setVertexBytes(&u, sizeof(u), 0);
        m_currentEncoder->setVertexBuffer(geom.vertexBuffer.get(), 0, 1);

        MTL::Texture* tex = m_defaultWhiteTexture.get();
        if (texture) {
            auto ti = m_textures.find(texture);
            if (ti != m_textures.end()) tex = ti->second.texture.get();
        }
        m_currentEncoder->setFragmentTexture(tex, 0);

        if (m_scissor.enabled) {
            int fbH = static_cast<int>(m_logicalHeight * m_scaleY);
            MTL::ScissorRect sr;
            sr.x      = static_cast<NS::UInteger>(m_scissor.x * m_scaleX);
            sr.y      = static_cast<NS::UInteger>(fbH - (m_scissor.y + m_scissor.height) * m_scaleY);
            sr.width  = static_cast<NS::UInteger>(m_scissor.width  * m_scaleX);
            sr.height = static_cast<NS::UInteger>(m_scissor.height * m_scaleY);
            m_currentEncoder->setScissorRect(sr);
        }

        if (geom.indexCount > 0) {
            m_currentEncoder->drawIndexedPrimitives(
                MTL::PrimitiveTypeTriangle, geom.indexCount,
                MTL::IndexTypeUInt32, geom.indexBuffer.get(), 0);
        }
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle h) override { m_geometry.erase(h); }

    void EnableScissorRegion(bool enable) override { m_scissor.enabled = enable; }
    void SetScissorRegion(Rml::Rectanglei r) override {
        m_scissor.x = r.Left(); m_scissor.y = r.Top();
        m_scissor.width = r.Width(); m_scissor.height = r.Height();
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return 0; }

    Rml::TextureHandle GenerateTexture(
        Rml::Span<const Rml::byte> source,
        Rml::Vector2i              dims) override
    {
        if (!m_device) return 0;
        auto desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        desc->setWidth(dims.x); desc->setHeight(dims.y);
        desc->setUsage(MTL::TextureUsageShaderRead);
        desc->setStorageMode(MTL::StorageModeShared);
        auto tex = NS::TransferPtr(m_device->newTexture(desc.get()));
        if (!tex) return 0;
        MTL::Region region(0, 0, 0, dims.x, dims.y, 1);
        tex->replaceRegion(region, 0, source.data(), dims.x * 4);
        Rml::TextureHandle h = m_nextTex++;
        m_textures[h] = { tex, dims.x, dims.y };
        return h;
    }

    void ReleaseTexture(Rml::TextureHandle h) override { m_textures.erase(h); }

    void SetTransform(const Rml::Matrix4f* t) override {
        m_transform = t ? *t : Rml::Matrix4f::Identity();
    }

private:
    void createDefaultWhiteTexture() {
        auto desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        desc->setWidth(1); desc->setHeight(1);
        desc->setUsage(MTL::TextureUsageShaderRead);
        desc->setStorageMode(MTL::StorageModeShared);
        m_defaultWhiteTexture = NS::TransferPtr(m_device->newTexture(desc.get()));
        if (m_defaultWhiteTexture) {
            uint8_t px[4] = {255, 255, 255, 255};
            MTL::Region r(0, 0, 0, 1, 1, 1);
            m_defaultWhiteTexture->replaceRegion(r, 0, px, 4);
        }
    }

    void createPipelineState() {
        std::string src = readFile("assets/shaders/rmlui.metal");
        auto code = NS::String::string(src.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* err = nullptr;
        auto lib = NS::TransferPtr(m_device->newLibrary(code, nullptr, &err));
        if (!lib) { fmt::print("[UIRenderer] Failed to compile rmlui.metal\n"); return; }

        auto vf = lib->newFunction(NS::String::string("vertexMain",   NS::StringEncoding::UTF8StringEncoding));
        auto ff = lib->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));
        if (!vf || !ff) { lib->release(); return; }

        auto vd = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
        auto pa = vd->attributes()->object(0);
        pa->setFormat(MTL::VertexFormatFloat2);
        pa->setOffset(offsetof(Rml::Vertex, position)); pa->setBufferIndex(1);
        auto ca = vd->attributes()->object(1);
        ca->setFormat(MTL::VertexFormatUChar4Normalized);
        ca->setOffset(offsetof(Rml::Vertex, colour)); ca->setBufferIndex(1);
        auto ta = vd->attributes()->object(2);
        ta->setFormat(MTL::VertexFormatFloat2);
        ta->setOffset(offsetof(Rml::Vertex, tex_coord)); ta->setBufferIndex(1);
        auto lay = vd->layouts()->object(1);
        lay->setStride(sizeof(Rml::Vertex));
        lay->setStepFunction(MTL::VertexStepFunctionPerVertex); lay->setStepRate(1);

        auto pd = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
        pd->setVertexFunction(vf); pd->setFragmentFunction(ff);
        pd->setVertexDescriptor(vd.get());
        auto col = pd->colorAttachments()->object(0);
        col->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
        col->setBlendingEnabled(true);
        col->setRgbBlendOperation(MTL::BlendOperationAdd);
        col->setAlphaBlendOperation(MTL::BlendOperationAdd);
        col->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
        col->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
        col->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        col->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        m_pipelineState = NS::TransferPtr(m_device->newRenderPipelineState(pd.get(), &err));

        auto ds = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
        ds->setDepthCompareFunction(MTL::CompareFunctionAlways);
        ds->setDepthWriteEnabled(false);
        m_depthStencilState = NS::TransferPtr(m_device->newDepthStencilState(ds.get()));

        vf->release(); ff->release(); lib->release();
    }

    struct Geom {
        NS::SharedPtr<MTL::Buffer> vertexBuffer;
        NS::SharedPtr<MTL::Buffer> indexBuffer;
        NS::UInteger indexCount;
    };
    struct TexData {
        NS::SharedPtr<MTL::Texture> texture;
        int width, height;
    };

    MTL::Device*               m_device               = nullptr;
    MTL::CommandBuffer*        m_currentCommandBuffer = nullptr;
    MTL::RenderCommandEncoder* m_currentEncoder       = nullptr;
    MTL::Texture*              m_currentRenderTarget  = nullptr;
    NS::SharedPtr<MTL::RenderPassDescriptor> m_currentPassDesc;

    NS::SharedPtr<MTL::RenderPipelineState> m_pipelineState;
    NS::SharedPtr<MTL::DepthStencilState>   m_depthStencilState;
    NS::SharedPtr<MTL::Texture>             m_defaultWhiteTexture;

    std::unordered_map<Rml::CompiledGeometryHandle, Geom>   m_geometry;
    std::unordered_map<Rml::TextureHandle,          TexData> m_textures;
    Rml::CompiledGeometryHandle m_nextGeom = 1;
    Rml::TextureHandle          m_nextTex  = 1;

    int   m_logicalWidth  = 0, m_logicalHeight = 0;
    float m_scaleX        = 1.0f, m_scaleY     = 1.0f;

    struct { bool enabled = false; int x=0,y=0,width=0,height=0; } m_scissor;
    Rml::Matrix4f m_transform;
};

// ── Global Rml state (shared across all UIRendererMetal instances) ────────────

namespace {

static std::unique_ptr<RmlUiSystem> s_rmlSystem;
static bool s_rmlInitialized = false;
static int  s_instanceCount  = 0;
static int  s_nextContextId  = 0;

static Rml::Input::KeyIdentifier sdlScancodeToRmlKey(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_BACKSPACE: return Rml::Input::KI_BACK;
    case SDL_SCANCODE_TAB:       return Rml::Input::KI_TAB;
    case SDL_SCANCODE_RETURN:    return Rml::Input::KI_RETURN;
    case SDL_SCANCODE_ESCAPE:    return Rml::Input::KI_ESCAPE;
    case SDL_SCANCODE_SPACE:     return Rml::Input::KI_SPACE;
    case SDL_SCANCODE_LEFT:      return Rml::Input::KI_LEFT;
    case SDL_SCANCODE_UP:        return Rml::Input::KI_UP;
    case SDL_SCANCODE_RIGHT:     return Rml::Input::KI_RIGHT;
    case SDL_SCANCODE_DOWN:      return Rml::Input::KI_DOWN;
    case SDL_SCANCODE_DELETE:    return Rml::Input::KI_DELETE;
    case SDL_SCANCODE_HOME:      return Rml::Input::KI_HOME;
    case SDL_SCANCODE_END:       return Rml::Input::KI_END;
    case SDL_SCANCODE_PAGEUP:    return Rml::Input::KI_PRIOR;
    case SDL_SCANCODE_PAGEDOWN:  return Rml::Input::KI_NEXT;
    default:
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
            return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_A + (sc - SDL_SCANCODE_A));
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
            return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_1 + (sc - SDL_SCANCODE_1));
        if (sc == SDL_SCANCODE_0)
            return Rml::Input::KI_0;
        return Rml::Input::KI_UNKNOWN;
    }
}

} // namespace

// ── UIRendererMetal ──────────────────────────────────────────────────────────

class UIRendererMetal : public UIRenderer {
public:
    UIRendererMetal(int width, int height) : m_width(width), m_height(height) {}
    ~UIRendererMetal() override { shutdown(); }

    bool initialize() override {
        m_device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
        if (!m_device) {
            fmt::print("[UIRenderer] No Metal device available\n");
            return false;
        }
        m_commandQueue = NS::TransferPtr(m_device->newCommandQueue());
        if (!m_commandQueue) return false;
        if (!createRenderTarget(m_width, m_height)) return false;

        m_rmlRenderer = std::make_unique<RmlUiMetal>(m_device.get());
        if (!m_rmlRenderer->initialize()) {
            fmt::print("[UIRenderer] RmlUiMetal init failed\n");
            return false;
        }

        if (!s_rmlInitialized) {
            s_rmlSystem = std::make_unique<RmlUiSystem>();
            Rml::SetSystemInterface(s_rmlSystem.get());
            Rml::SetRenderInterface(m_rmlRenderer.get());
            if (!Rml::Initialise()) {
                fmt::print("[UIRenderer] Rml::Initialise() failed\n");
                return false;
            }
            auto fontPath = FileSystem::instance().resolvePath("fonts/Arial Black.ttf");
            if (fontPath) Rml::LoadFontFace(*fontPath);
            s_rmlInitialized = true;
        } else {
            // Switch global render interface to this surface's renderer
            Rml::SetRenderInterface(m_rmlRenderer.get());
        }

        m_contextName = fmt::format("surface_{}", s_nextContextId++);
        m_context = Rml::CreateContext(m_contextName, Rml::Vector2i(m_width, m_height));
        if (!m_context) {
            fmt::print("[UIRenderer] Failed to create Rml context '{}'\n", m_contextName);
            return false;
        }

        ++s_instanceCount;
        m_active = true;
        fmt::print("[UIRenderer] Surface '{}' ready ({}x{})\n", m_contextName, m_width, m_height);
        return true;
    }

    void renderFrame() override {
        if (!m_context || !m_renderTarget || !m_commandQueue || !m_rmlRenderer) return;

        // Switch global render interface before rendering this surface
        Rml::SetRenderInterface(m_rmlRenderer.get());
        m_context->Update();

        auto* cmd = m_commandQueue->commandBuffer();
        if (!cmd) return;
        m_rmlRenderer->beginFrame(cmd, m_renderTarget.get(), m_width, m_height);
        m_context->Render();
        m_rmlRenderer->endFrame();
        cmd->commit();
    }

    void resize(int width, int height) override {
        if (width == m_width && height == m_height) return;
        m_width  = width;
        m_height = height;
        createRenderTarget(width, height);
        if (m_context) m_context->SetDimensions(Rml::Vector2i(width, height));
    }

    void* getSharedTexture() override {
        return static_cast<void*>(m_renderTarget.get());
    }

    void shutdown() override {
        if (!m_active) return;
        m_active = false;

        if (m_context) {
            Rml::RemoveContext(m_contextName);
            m_context = nullptr;
        }
        if (m_rmlRenderer) { m_rmlRenderer->shutdown(); m_rmlRenderer.reset(); }
        m_renderTarget.reset();
        m_commandQueue.reset();
        m_device.reset();

        if (--s_instanceCount == 0 && s_rmlInitialized) {
            Rml::Shutdown();
            s_rmlInitialized = false;
            s_rmlSystem.reset();
        }
    }

    void loadDocument(const std::string& path) override {
        if (!m_context) return;
        auto resolved = FileSystem::instance().resolvePath(path);
        std::string absPath = resolved ? *resolved : path;
        Rml::SetRenderInterface(m_rmlRenderer.get());
        auto* doc = m_context->LoadDocument(absPath);
        if (doc) {
            doc->Show();
            m_activeDocPath = path;
        } else {
            fmt::print("[UIRenderer] Failed to load document: {}\n", absPath);
        }
    }

    void reloadDocument() override {
        if (!m_context || m_activeDocPath.empty()) return;
        auto resolved = FileSystem::instance().resolvePath(m_activeDocPath);
        std::string absPath = resolved ? *resolved : m_activeDocPath;
        Rml::SetRenderInterface(m_rmlRenderer.get());
        auto* doc = m_context->GetDocument(absPath);
        if (doc) doc->Close();
        auto* newDoc = m_context->LoadDocument(absPath);
        if (newDoc) newDoc->Show();
    }

    Rml::Context* getContext() override { return m_context; }

    // button: 0=move only; 1/2/3=SDL left/middle/right press; negative=release
    void injectMouseEvent(double x, double y, int button) override {
        if (!m_context) return;
        m_context->ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), 0);
        if (button != 0) {
            // Map SDL button index → RmlUI button index (0=left, 1=right, 2=middle)
            int absBtn = button < 0 ? -button : button;
            int rmlBtn = (absBtn == 1) ? 0 : (absBtn == 3) ? 1 : 2;
            if (button > 0) m_context->ProcessMouseButtonDown(rmlBtn, 0);
            else            m_context->ProcessMouseButtonUp(rmlBtn, 0);
        }
    }

    void injectKeyEvent(int sdlScancode, int pressed) override {
        if (!m_context) return;
        auto key = sdlScancodeToRmlKey(static_cast<SDL_Scancode>(sdlScancode));
        if (key != Rml::Input::KI_UNKNOWN) {
            if (pressed) m_context->ProcessKeyDown(key, 0);
            else         m_context->ProcessKeyUp(key, 0);
        }
    }

private:
    bool createRenderTarget(int width, int height) {
        auto desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
        desc->setWidth(static_cast<NS::UInteger>(width));
        desc->setHeight(static_cast<NS::UInteger>(height));
        desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        desc->setStorageMode(MTL::StorageModeShared);
        m_renderTarget = NS::TransferPtr(m_device->newTexture(desc.get()));
        if (!m_renderTarget) {
            fmt::print("[UIRenderer] Failed to create render target\n");
            return false;
        }
        return true;
    }

    NS::SharedPtr<MTL::Device>       m_device;
    NS::SharedPtr<MTL::CommandQueue> m_commandQueue;
    NS::SharedPtr<MTL::Texture>      m_renderTarget;
    std::unique_ptr<RmlUiMetal>      m_rmlRenderer;
    Rml::Context*                    m_context      = nullptr;
    std::string                      m_contextName;
    std::string                      m_activeDocPath;
    int  m_width, m_height;
    bool m_active = false;
};

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<UIRenderer> UIRenderer::create(int width, int height)
{
    auto r = std::make_unique<UIRendererMetal>(width, height);
    if (!r->initialize()) return nullptr;
    return r;
}

}// namespace Vapor

#endif // __APPLE__
