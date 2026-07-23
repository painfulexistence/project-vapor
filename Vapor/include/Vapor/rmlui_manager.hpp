#pragma once

#include "ui_document_registry.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Types.h>
#include <SDL3/SDL.h>
#include <memory>
#include <string>

namespace Vapor {

    class RmlUiSystem;

    // Registers the engine's default RmlUi font faces. The single source of
    // truth for which fonts ship: called by RmlUiManager (the main-UI
    // bootstrap both backends share) AND by the CAPI's standalone UIRenderer
    // (which runs without an RmlUiManager). Returns false if any face failed.
    bool loadDefaultFontFaces();

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

        // ── Handle-based document management ─────────────────────────────
        // Preferred API: data components hold UIDocumentHandles (value
        // semantics, serializer/inspector-friendly), never raw document
        // pointers. The Rml context owns the documents; the registry maps
        // handles onto them and survives hot reloads.

        // Load a document and register it. Invalid handle on failure.
        UIDocumentHandle LoadDocumentHandle(const std::string& filename);

        // Current pointer for a handle; null when the handle is stale (closed)
        // or the last reload failed. Do not cache across frames — re-resolve.
        Rml::ElementDocument* Resolve(UIDocumentHandle h) const {
            return m_documents.resolve(h);
        }

        // Bumps on every reload; callers re-attach (re-query elements,
        // re-bind listeners) when the version they attached against moved.
        Uint32 DocumentVersion(UIDocumentHandle h) const {
            return m_documents.version(h);
        }

        // Close the document and retire the handle in one step, so nothing can
        // resolve the document during Rml's deferred-destruction window.
        void CloseDocument(UIDocumentHandle h);

        // Hot reload: closes the old document, loads the same path, keeps the
        // handle stable and bumps its version. Detach pages (drop listeners /
        // element pointers) BEFORE calling this — see PageSystem::reload.
        void ReloadDocument(UIDocumentHandle h);

        UIDocumentRegistry& documents() {
            return m_documents;
        }
        const UIDocumentRegistry& documents() const {
            return m_documents;
        }

        // ── Raw-pointer document management (transitional) ───────────────
        // Kept for callers that manage document lifetime themselves. New code
        // should use the handle API above.
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
        UIDocumentRegistry m_documents;

        int m_width = 0;
        int m_height = 0;
        bool m_initialized = false;
    };

}// namespace Vapor
