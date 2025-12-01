#pragma once

// RmlUi integration for Vapor Engine
// This header includes all necessary components for using RmlUi with SDL3 and Metal

#include "rmlui_system.hpp"
#include "rmlui_render.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <memory>

namespace Vapor {

// Forward declaration
class RmlUiRenderer;

// RmlUi Manager - handles initialization and lifecycle of RmlUi
class RmlUiManager {
public:
    RmlUiManager();
    ~RmlUiManager();

    // Initialize RmlUi with SDL3 window and renderer backend
    // Must be called after renderer->init()
    bool init(SDL_Window* window, RmlUiRenderer* renderer);

    // Shutdown RmlUi
    void shutdown();

    // Create a new context with the given name and dimensions
    Rml::Context* createContext(const Rml::String& name, int width, int height);

    // Get a context by name
    Rml::Context* getContext(const Rml::String& name);

    // Get the main/default context
    Rml::Context* getMainContext() { return mainContext; }

    // Process SDL3 event - call this for each event in your event loop
    // Returns true if the event was consumed by RmlUi
    bool processEvent(const SDL_Event& event);

    // Update RmlUi - call this once per frame before rendering
    void update();

    // Begin frame - call before Context::Render()
    void beginFrame();

    // End frame - call after Context::Render()
    void endFrame();

    // Set the viewport dimensions (call when window is resized)
    void setViewportDimensions(int width, int height);

    // Load a font file
    bool loadFont(const Rml::String& path);

    // Get the system interface
    RmlUi_SystemInterface_SDL3* getSystemInterface() { return systemInterface.get(); }

    // Get the render interface
    RmlUi_RenderInterface* getRenderInterface() { return renderInterface.get(); }

private:
    std::unique_ptr<RmlUi_SystemInterface_SDL3> systemInterface;
    std::unique_ptr<RmlUi_RenderInterface> renderInterface;
    Rml::Context* mainContext = nullptr;
    bool initialized = false;
};

} // namespace Vapor
