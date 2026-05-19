#pragma once

#include <memory>

namespace Rml {
    class Context;
}

namespace Vapor {

/**
 * Platform-independent UI renderer.
 *
 * Owns a render target texture (MTLTexture on Apple, VkImage on Vulkan, …)
 * and a Rml::RenderInterface. Created once by the CAPI layer in
 * Vapor_CreateSharedSurface; drives Rml::Context::Render() each tick without
 * requiring a full swapchain or SDL window.
 *
 * The renderer also calls Rml::SetRenderInterface and
 * RmlUiManager::FinalizeInitialization during initialize(), so the caller
 * must have already run EngineCore::init() and initRmlUI().
 */
class UIRenderer {
public:
    virtual ~UIRenderer() = default;

    virtual bool initialize() = 0;
    virtual void renderFrame(Rml::Context* ctx) = 0;
    virtual void resize(int width, int height) = 0;
    virtual void* getSharedTexture() = 0;
    virtual void shutdown() = 0;

    // Platform factory — returns nullptr on unsupported platforms.
    static std::unique_ptr<UIRenderer> create(int width, int height);
};

}// namespace Vapor
