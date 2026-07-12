#ifdef __APPLE__

#include "ui_renderer.hpp"
#include "rmlui_system.hpp"
#include "Vapor/rml_renderer_metal.hpp"
#include "Vapor/file_system.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace Vapor {

// ── Global Rml state (shared across all UIRendererMetal instances) ────────────
// Thread-safe: multiple surfaces may be created/destroyed from different threads.

namespace {

static std::unique_ptr<RmlUiSystem> s_rmlSystem;
static std::mutex                   s_rmlMutex;
static std::atomic<bool>            s_rmlInitialized{false};
static std::atomic<int>             s_instanceCount{0};
static std::atomic<int>             s_nextContextId{0};

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

        m_rmlRenderer = std::make_unique<RmlRendererMetal>(m_device.get());
        if (!m_rmlRenderer->initialize()) {
            fmt::print("[UIRenderer] RmlRendererMetal init failed\n");
            return false;
        }

        // Thread-safe RmlUI initialization
        {
            std::lock_guard<std::mutex> lock(s_rmlMutex);
            if (!s_rmlInitialized.load()) {
                s_rmlSystem = std::make_unique<RmlUiSystem>();
                Rml::SetSystemInterface(s_rmlSystem.get());
                Rml::SetRenderInterface(m_rmlRenderer.get());
                if (!Rml::Initialise()) {
                    fmt::print("[UIRenderer] Rml::Initialise() failed\n");
                    s_rmlSystem.reset();
                    return false;
                }
                auto fontPath = FileSystem::instance().resolvePath("fonts/NotoSans-SemiBold.ttf");
                if (fontPath) Rml::LoadFontFace(*fontPath);
                s_rmlInitialized.store(true);
            } else {
                Rml::SetRenderInterface(m_rmlRenderer.get());
            }
        }

        m_contextName = fmt::format("surface_{}", s_nextContextId.fetch_add(1));
        m_context = Rml::CreateContext(m_contextName, Rml::Vector2i(m_width, m_height));
        if (!m_context) {
            fmt::print("[UIRenderer] Failed to create Rml context '{}'\n", m_contextName);
            return false;
        }

        s_instanceCount.fetch_add(1);
        m_active = true;
        fmt::print("[UIRenderer] Surface '{}' ready ({}x{})\n", m_contextName, m_width, m_height);
        return true;
    }

    void renderFrame() override {
        if (!m_context || !m_renderTarget || !m_commandQueue || !m_rmlRenderer) return;

        Rml::SetRenderInterface(m_rmlRenderer.get());
        m_context->Update();

        auto* cmd = m_commandQueue->commandBuffer();
        if (!cmd) return;

        // Create render pass (clear to transparent black)
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto ca = passDesc->colorAttachments()->object(0);
        ca->setTexture(m_renderTarget.get());
        ca->setLoadAction(MTL::LoadActionClear);
        ca->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        ca->setStoreAction(MTL::StoreActionStore);

        auto* encoder = cmd->renderCommandEncoder(passDesc.get());

        int fbW = static_cast<int>(m_renderTarget->width());
        int fbH = static_cast<int>(m_renderTarget->height());
        m_rmlRenderer->setEncoder(encoder, m_width, m_height, fbW, fbH);
        m_context->Render();
        m_rmlRenderer->clearEncoder();

        encoder->endEncoding();
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

        // Thread-safe RmlUI shutdown (only when last instance is destroyed)
        if (s_instanceCount.fetch_sub(1) == 1) {
            std::lock_guard<std::mutex> lock(s_rmlMutex);
            if (s_rmlInitialized.load()) {
                Rml::Shutdown();
                s_rmlInitialized.store(false);
                s_rmlSystem.reset();
            }
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

    void injectMouseEvent(double x, double y, int button) override {
        if (!m_context) return;
        m_context->ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), 0);
        if (button != 0) {
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

    NS::SharedPtr<MTL::Device>        m_device;
    NS::SharedPtr<MTL::CommandQueue>  m_commandQueue;
    NS::SharedPtr<MTL::Texture>       m_renderTarget;
    std::unique_ptr<RmlRendererMetal> m_rmlRenderer;
    Rml::Context*                     m_context = nullptr;
    std::string                       m_contextName;
    std::string                       m_activeDocPath;
    int  m_width, m_height;
    bool m_active = false;
};

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<UIRenderer> UIRenderer::create(int width, int height) {
    auto r = std::make_unique<UIRendererMetal>(width, height);
    if (!r->initialize()) return nullptr;
    return r;
}

} // namespace Vapor

#endif // __APPLE__
