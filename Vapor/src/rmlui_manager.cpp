#include "rmlui_manager.hpp"
#include "rmlui_system.hpp"
#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <SDL3/SDL.h>
#include <fmt/core.h>
// #include <spdlog/spdlog.h>

namespace Vapor {

    RmlUiManager::RmlUiManager() = default;

    RmlUiManager::~RmlUiManager() {
        Shutdown();
    }

    bool RmlUiManager::Initialize(int width, int height) {
        if (m_initialized) {
            // spdlog::warn("RmlUiManager already initialized");
            return true;
        }

        m_width = width;
        m_height = height;

        // Create system interface
        m_system = std::make_unique<RmlUiSystem>();

        // Set system interface
        // Note: RenderInterface and Rml::Initialise() will be called by the backend renderer
        Rml::SetSystemInterface(m_system.get());

        m_initialized = true;
        fmt::print("RmlUiManager initialized (system interface set)\n");

        return true;
    }

    bool RmlUiManager::FinalizeInitialization() {
        if (!m_initialized) {
            fmt::print("RmlUiManager::FinalizeInitialization: Not initialized\n");
            return false;
        }

        if (m_context) {
            fmt::print("RmlUiManager::FinalizeInitialization: Already finalized\n");
            return true;// Already finalized
        }

        // Initialize RmlUi (requires RenderInterface to be set first)
        if (!Rml::Initialise()) {
            fmt::print("RmlUiManager::FinalizeInitialization: Rml::Initialise() failed\n");
            return false;
        }

        // Load fonts
        if (!Rml::LoadFontFace(SDL_GetBasePath() + std::string("assets/fonts/Arial Black.ttf"))) {
            fmt::print("RmlUiManager::FinalizeInitialization: Warning - failed to load font\n");
        }

        // Create the main UI context
        m_context = Rml::CreateContext("main", Rml::Vector2i(m_width, m_height));
        if (!m_context) {
            fmt::print("RmlUiManager::FinalizeInitialization: Failed to create context\n");
            Rml::Shutdown();
            return false;
        }

        // Initialize debugger (useful for development)
        Rml::Debugger::Initialise(m_context);

        fmt::print("RmlUI finalized successfully ({}x{})\n", m_width, m_height);
        return true;
    }

    void RmlUiManager::Shutdown() {
        if (!m_initialized) return;

        if (m_context) {
            Rml::Debugger::Shutdown();
            Rml::RemoveContext(m_context->GetName());
            m_context = nullptr;
        }

        Rml::Shutdown();

        m_system.reset();

        m_initialized = false;
        // spdlog::info("RmlUi shutdown complete");
    }

    void RmlUiManager::Update(float deltaTime) {
        if (!m_initialized || !m_context) return;

        // Update the context (this does not render, only updates logic/animations)
        m_context->Update();
    }

    void RmlUiManager::OnResize(int width, int height) {
        m_width = width;
        m_height = height;

        if (m_context) {
            m_context->SetDimensions(Rml::Vector2i(width, height));
        }
    }

    Rml::ElementDocument* RmlUiManager::LoadDocument(const std::string& filename) {
        if (!m_context) {
            fmt::print("RmlUiManager::LoadDocument: RmlUi not initialized\n");
            return nullptr;
        }
        std::string path = SDL_GetBasePath() + filename;
        fmt::print("RmlUiManager::LoadDocument: Attempting to load: {}\n", path);
        Rml::ElementDocument* document = m_context->LoadDocument(path);
        if (!document) {
            fmt::print("RmlUiManager::LoadDocument: Failed to load document: {}\n", path);
            return nullptr;
        }

        fmt::print("RmlUiManager::LoadDocument: Successfully loaded document: {}\n", path);
        return document;
    }

    Rml::ElementDocument* RmlUiManager::ReloadDocument(const std::string& filename) {
        if (!m_context) return nullptr;

        std::string path = SDL_GetBasePath() + filename;
        fmt::print("RmlUiManager::ReloadDocument: Looking for document with path: {}\n", path);

        // Debug: List all documents
        int num_docs = m_context->GetNumDocuments();
        fmt::print("RmlUiManager::ReloadDocument: Open documents: {}\n", num_docs);
        for (int i = 0; i < num_docs; ++i) {
            auto* doc = m_context->GetDocument(i);
            fmt::print("  Doc {}: {}\n", i, doc->GetId());
        }

        // Find existing document by ID (which is the path)
        Rml::ElementDocument* document = m_context->GetDocument(path);
        if (document) {
            fmt::print("RmlUiManager::ReloadDocument: Found existing document, closing it.\n");
            document->Close();
        } else {
            fmt::print("RmlUiManager::ReloadDocument: Document not found in context.\n");
        }

        // Reload
        Rml::ElementDocument* newDoc = LoadDocument(filename);
        if (newDoc) {
            newDoc->Show();
            fmt::print("RmlUiManager::ReloadDocument: New document loaded and shown.\n");
        }
        return newDoc;
    }

    void RmlUiManager::UnloadDocument(Rml::ElementDocument* document) {
        if (document) {
            document->Close();
        }
    }

    void RmlUiManager::ShowDocument(const std::string& id) {
        if (!m_context) return;

        Rml::ElementDocument* document = m_context->GetDocument(id);
        if (document) {
            document->Show();
        } else {
            // spdlog::warn("Document not found: {}", id);
        }
    }

    void RmlUiManager::HideDocument(const std::string& id) {
        if (!m_context) return;

        Rml::ElementDocument* document = m_context->GetDocument(id);
        if (document) {
            document->Hide();
        }
    }

    // Helper function to convert SDL scancode to RmlUI key identifier
    static Rml::Input::KeyIdentifier SDLScancodeToRmlKey(SDL_Scancode scancode) {
        // Basic mapping - can be extended as needed
        switch (scancode) {
        case SDL_SCANCODE_BACKSPACE:
            return Rml::Input::KI_BACK;
        case SDL_SCANCODE_TAB:
            return Rml::Input::KI_TAB;
        case SDL_SCANCODE_RETURN:
            return Rml::Input::KI_RETURN;
        case SDL_SCANCODE_ESCAPE:
            return Rml::Input::KI_ESCAPE;
        case SDL_SCANCODE_SPACE:
            return Rml::Input::KI_SPACE;
        case SDL_SCANCODE_LEFT:
            return Rml::Input::KI_LEFT;
        case SDL_SCANCODE_UP:
            return Rml::Input::KI_UP;
        case SDL_SCANCODE_RIGHT:
            return Rml::Input::KI_RIGHT;
        case SDL_SCANCODE_DOWN:
            return Rml::Input::KI_DOWN;
        case SDL_SCANCODE_DELETE:
            return Rml::Input::KI_DELETE;
        case SDL_SCANCODE_HOME:
            return Rml::Input::KI_HOME;
        case SDL_SCANCODE_END:
            return Rml::Input::KI_END;
        case SDL_SCANCODE_PAGEUP:
            return Rml::Input::KI_PRIOR;
        case SDL_SCANCODE_PAGEDOWN:
            return Rml::Input::KI_NEXT;
        default:
            // For letter keys, map scancode to key identifier
            if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
                return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_A + (scancode - SDL_SCANCODE_A));
            }
            if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
                return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_1 + (scancode - SDL_SCANCODE_1));
            }
            if (scancode == SDL_SCANCODE_0) {
                return Rml::Input::KI_0;
            }
            return Rml::Input::KI_UNKNOWN;
        }
    }

    // Helper function to get key modifier from SDL event
    static int GetKeyModifier(const SDL_Event& event) {
        int modifier = 0;
        SDL_Keymod mod = SDL_GetModState();

        // For keyboard events, use the modifier from the event itself
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            mod = (SDL_Keymod)event.key.mod;
        }
        // For mouse events, use current keyboard modifier state

        if (mod & SDL_KMOD_CTRL) modifier |= Rml::Input::KM_CTRL;
        if (mod & SDL_KMOD_SHIFT) modifier |= Rml::Input::KM_SHIFT;
        if (mod & SDL_KMOD_ALT) modifier |= Rml::Input::KM_ALT;
        if (mod & SDL_KMOD_GUI) modifier |= Rml::Input::KM_META;
        return modifier;
    }

    // Main event processing method - routes SDL events to RmlUI
    bool RmlUiManager::ProcessEvent(const SDL_Event& event) {
        if (!m_initialized || !m_context) {
            return false;// RmlUI not initialized, event not consumed
        }

        int key_modifier = GetKeyModifier(event);
        bool consumed = false;

        switch (event.type) {
        case SDL_EVENT_KEY_DOWN: {
            ProcessKeyDown(event.key.scancode, key_modifier);
            // Check if RmlUI consumed the event (if hovering over UI element)
            consumed = m_context->GetHoverElement() != nullptr;
            break;
        }
        case SDL_EVENT_KEY_UP: {
            ProcessKeyUp(event.key.scancode, key_modifier);
            consumed = m_context->GetHoverElement() != nullptr;
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            // SDL3 text input event
            for (int i = 0; event.text.text[i] != '\0'; ++i) {
                ProcessTextInput(event.text.text[i]);
            }
            // Text input is typically consumed if there's a hover element
            consumed = m_context->GetHoverElement() != nullptr;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            ProcessMouseMove(event.motion.x, event.motion.y, key_modifier);
            consumed = m_context->GetHoverElement() != nullptr;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            ProcessMouseButtonDown(event.button.button, key_modifier);
            consumed = m_context->GetHoverElement() != nullptr;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            ProcessMouseButtonUp(event.button.button, key_modifier);
            consumed = m_context->GetHoverElement() != nullptr;
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            // SDL3 uses precise scrolling
            float wheel_delta = event.wheel.y;
            ProcessMouseWheel(wheel_delta, key_modifier);
            consumed = m_context->GetHoverElement() != nullptr;
            break;
        }
        case SDL_EVENT_WINDOW_RESIZED: {
            int width = event.window.data1;
            int height = event.window.data2;
            OnResize(width, height);
            break;
        }
        default:
            break;
        }

        return consumed;
    }

    // Input handling methods - SDL3 scancode based
    void RmlUiManager::ProcessKeyDown(SDL_Scancode scancode, int key_modifier) {
        if (!m_context) return;

        Rml::Input::KeyIdentifier key = SDLScancodeToRmlKey(scancode);
        if (key != Rml::Input::KI_UNKNOWN) {
            m_context->ProcessKeyDown(key, key_modifier);
        }
    }

    void RmlUiManager::ProcessKeyUp(SDL_Scancode scancode, int key_modifier) {
        if (!m_context) return;

        Rml::Input::KeyIdentifier key = SDLScancodeToRmlKey(scancode);
        if (key != Rml::Input::KI_UNKNOWN) {
            m_context->ProcessKeyUp(key, key_modifier);
        }
    }

    void RmlUiManager::ProcessTextInput(char character) {
        if (m_context) {
            m_context->ProcessTextInput(static_cast<Rml::Character>(character));
        }
    }

    void RmlUiManager::ProcessMouseMove(int x, int y, int key_modifier) {
        if (m_context) {
            m_context->ProcessMouseMove(x, y, key_modifier);
        }
    }

    void RmlUiManager::ProcessMouseButtonDown(int button_index, int key_modifier) {
        if (m_context) {
            // SDL button indices: 1=left, 2=middle, 3=right
            // RmlUI expects: 0=left, 1=right, 2=middle
            int rml_button = 0;
            if (button_index == SDL_BUTTON_LEFT)
                rml_button = 0;
            else if (button_index == SDL_BUTTON_RIGHT)
                rml_button = 1;
            else if (button_index == SDL_BUTTON_MIDDLE)
                rml_button = 2;
            else
                return;

            m_context->ProcessMouseButtonDown(rml_button, key_modifier);
        }
    }

    void RmlUiManager::ProcessMouseButtonUp(int button_index, int key_modifier) {
        if (m_context) {
            int rml_button = 0;
            if (button_index == SDL_BUTTON_LEFT)
                rml_button = 0;
            else if (button_index == SDL_BUTTON_RIGHT)
                rml_button = 1;
            else if (button_index == SDL_BUTTON_MIDDLE)
                rml_button = 2;
            else
                return;

            m_context->ProcessMouseButtonUp(rml_button, key_modifier);
        }
    }

    void RmlUiManager::ProcessMouseWheel(float wheel_delta, int key_modifier) {
        if (m_context) {
            m_context->ProcessMouseWheel(Rml::Vector2f(0, wheel_delta), key_modifier);
        }
    }

}// namespace Vapor
