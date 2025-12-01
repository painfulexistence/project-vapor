#include "Vapor/rmlui.hpp"
#include <fmt/core.h>

namespace Vapor {

RmlUiManager::RmlUiManager() {
}

RmlUiManager::~RmlUiManager() {
    if (initialized) {
        shutdown();
    }
}

bool RmlUiManager::init(SDL_Window* window, RmlUiRenderer* renderer) {
    if (initialized) {
        fmt::print("[RmlUi] Already initialized\n");
        return true;
    }

    // Create system interface
    systemInterface = std::make_unique<RmlUi_SystemInterface_SDL3>();

    // Create render interface
    renderInterface = std::make_unique<RmlUi_RenderInterface>();
    renderInterface->setRenderer(renderer);

    // Set interfaces
    Rml::SetSystemInterface(systemInterface.get());
    Rml::SetRenderInterface(renderInterface.get());

    // Initialize RmlUi
    if (!Rml::Initialise()) {
        fmt::print("[RmlUi] Failed to initialize RmlUi\n");
        return false;
    }

    // Get window size
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    renderInterface->setViewportDimensions(width, height);

    // Create main context
    mainContext = Rml::CreateContext("main", Rml::Vector2i(width, height));
    if (!mainContext) {
        fmt::print("[RmlUi] Failed to create main context\n");
        Rml::Shutdown();
        return false;
    }

    initialized = true;
    fmt::print("[RmlUi] Initialized successfully ({}x{})\n", width, height);
    return true;
}

void RmlUiManager::shutdown() {
    if (!initialized) {
        return;
    }

    if (mainContext) {
        Rml::RemoveContext(mainContext->GetName());
        mainContext = nullptr;
    }

    Rml::Shutdown();

    renderInterface.reset();
    systemInterface.reset();

    initialized = false;
    fmt::print("[RmlUi] Shutdown complete\n");
}

Rml::Context* RmlUiManager::createContext(const Rml::String& name, int width, int height) {
    if (!initialized) {
        fmt::print("[RmlUi] Not initialized\n");
        return nullptr;
    }
    return Rml::CreateContext(name, Rml::Vector2i(width, height));
}

Rml::Context* RmlUiManager::getContext(const Rml::String& name) {
    if (!initialized) {
        return nullptr;
    }
    return Rml::GetContext(name);
}

bool RmlUiManager::processEvent(const SDL_Event& event) {
    if (!initialized || !mainContext) {
        return false;
    }
    return RmlUi_SystemInterface_SDL3::ProcessEvent(mainContext, event);
}

void RmlUiManager::update() {
    if (!initialized || !mainContext) {
        return;
    }
    mainContext->Update();
}

void RmlUiManager::beginFrame() {
    if (!initialized || !renderInterface) {
        return;
    }
    renderInterface->beginFrame();
}

void RmlUiManager::endFrame() {
    if (!initialized || !renderInterface) {
        return;
    }
    renderInterface->endFrame();
}

void RmlUiManager::setViewportDimensions(int width, int height) {
    if (!initialized) {
        return;
    }

    if (renderInterface) {
        renderInterface->setViewportDimensions(width, height);
    }

    if (mainContext) {
        mainContext->SetDimensions(Rml::Vector2i(width, height));
    }
}

bool RmlUiManager::loadFont(const Rml::String& path) {
    if (!initialized) {
        return false;
    }
    return Rml::LoadFontFace(path);
}

} // namespace Vapor
