#pragma once

#include <memory>
#include <string>

namespace Rml {
    class Context;
}

namespace Vapor {

/**
 * Off-screen UI renderer for a single Surface.
 *
 * Owns a render target texture, a Rml::RenderInterface, and a Rml::Context.
 * Multiple independent instances can coexist (one per CAPI Surface).
 *
 * The caller must have run EngineCore::init() before create().
 */
class UIRenderer {
public:
    virtual ~UIRenderer() = default;

    virtual bool initialize() = 0;
    virtual void renderFrame() = 0;// Renders owned Rml::Context to owned texture
    virtual void resize(int width, int height) = 0;
    virtual void* getSharedTexture() = 0;
    virtual void shutdown() = 0;

    // Document management
    virtual void        loadDocument(const std::string& path) = 0;
    virtual void        reloadDocument() = 0;
    virtual Rml::Context* getContext() = 0;

    // Input (logical coordinates matching surface dimensions)
    virtual void injectMouseEvent(double x, double y, int button) = 0;
    virtual void injectKeyEvent(int sdlScancode, int pressed) = 0;

    // Platform factory — returns nullptr on unsupported platforms.
    static std::unique_ptr<UIRenderer> create(int width, int height);
};

}// namespace Vapor
