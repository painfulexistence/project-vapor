#pragma once

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Types.h>
#include <SDL3/SDL.h>
#include <memory>
#include <string>

namespace Vapor {

    class RmlUiSystem;

    /**
     * RmlUI Manager - manages RmlUI context and document lifecycle (business logic only)
     * Does not handle rendering - rendering is delegated to the backend renderer
     * Integrated into EngineCore as a subsystem
     */
    class RmlUiManager {
    public:
        RmlUiManager();
        ~RmlUiManager();

        // Initialize RmlUI with window dimensions (no device needed)
        // This only sets up the system interface - call FinalizeInitialization() after renderer sets RenderInterface
        bool Initialize(int width, int height);

        // Finalize initialization after RenderInterface is set (creates context, loads fonts, etc.)
        bool FinalizeInitialization();

        // Shutdown and cleanup
        void Shutdown();

        // Update per-frame (updates context, does not render)
        void Update(float deltaTime);

        // Handle window resize
        void OnResize(int width, int height);

        // Document management
        Rml::ElementDocument* LoadDocument(const std::string& filename);
        Rml::ElementDocument* ReloadDocument(const std::string& filename);
        void UnloadDocument(Rml::ElementDocument* document);
        void ShowDocument(const std::string& id);
        void HideDocument(const std::string& id);

        // Input handling - SDL3 events
        // Process a single SDL event and route to RmlUI
        bool ProcessEvent(const SDL_Event& event);

        // Individual event processing methods (for direct use if needed)
        void ProcessKeyDown(SDL_Scancode scancode, int key_modifier);
        void ProcessKeyUp(SDL_Scancode scancode, int key_modifier);
        void ProcessTextInput(char character);
        void ProcessMouseMove(int x, int y, int key_modifier);
        void ProcessMouseButtonDown(int button_index, int key_modifier);
        void ProcessMouseButtonUp(int button_index, int key_modifier);
        void ProcessMouseWheel(float wheel_delta, int key_modifier);

        // Get the main context
        Rml::Context* GetContext() const {
            return m_context;
        }

        // Check if initialized
        bool IsInitialized() const {
            return m_initialized;
        }

    private:
        std::unique_ptr<RmlUiSystem> m_system;
        Rml::Context* m_context = nullptr;

        int m_width = 0;
        int m_height = 0;
        bool m_initialized = false;
    };

}// namespace Vapor
