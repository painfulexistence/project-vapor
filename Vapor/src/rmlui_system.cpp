#include "rmlui_system.hpp"
// #include <spdlog/spdlog.h>
#include <SDL3/SDL.h>
#include <chrono>

namespace Vapor {

    RmlUiSystem::RmlUiSystem() {
        auto now = std::chrono::high_resolution_clock::now();
        m_start_time = std::chrono::duration<double>(now.time_since_epoch()).count();

        // Initialize cursor cache (will be created on demand)
        for (int i = 0; i < 20; ++i) {// Increased for SDL3
            m_cursors[i] = nullptr;
        }
    }

    RmlUiSystem::~RmlUiSystem() {
        // Cleanup cursors
        for (int i = 0; i < 20; ++i) {// Increased for SDL3
            if (m_cursors[i]) {
                SDL_DestroyCursor(m_cursors[i]);
                m_cursors[i] = nullptr;
            }
        }
    }

    double RmlUiSystem::GetElapsedTime() {
        // Use SDL3's time API
        return SDL_GetTicks() / 1000.0;
    }

    bool RmlUiSystem::LogMessage(Rml::Log::Type type, const Rml::String& message) {
        switch (type) {
        case Rml::Log::LT_ALWAYS:
        case Rml::Log::LT_ERROR:
            // spdlog::error("[RmlUi] {}", message);
            break;
        case Rml::Log::LT_WARNING:
            // spdlog::warn("[RmlUi] {}", message);
            break;
        case Rml::Log::LT_INFO:
            // spdlog::info("[RmlUi] {}", message);
            break;
        case Rml::Log::LT_DEBUG:
            // spdlog::debug("[RmlUi] {}", message);
            break;
        default:
            // spdlog::trace("[RmlUi] {}", message);
            break;
        }
        return true;
    }

    void RmlUiSystem::SetMouseCursor(const Rml::String& cursor_name) {
        SDL_SystemCursor sdl_cursor = SDL_SYSTEM_CURSOR_DEFAULT;

        // Map RmlUI cursor names to SDL3 cursor types (SDL3 renamed some constants)
        if (cursor_name == "arrow") {
            sdl_cursor = SDL_SYSTEM_CURSOR_DEFAULT;
        } else if (cursor_name == "move") {
            sdl_cursor = SDL_SYSTEM_CURSOR_MOVE;// SDL3: was SDL_SYSTEM_CURSOR_SIZEALL
        } else if (cursor_name == "pointer") {
            sdl_cursor = SDL_SYSTEM_CURSOR_POINTER;// SDL3: was SDL_SYSTEM_CURSOR_HAND
        } else if (cursor_name == "resize") {
            sdl_cursor = SDL_SYSTEM_CURSOR_NWSE_RESIZE;// SDL3: was SDL_SYSTEM_CURSOR_SIZENWSE
        } else if (cursor_name == "cross") {
            sdl_cursor = SDL_SYSTEM_CURSOR_CROSSHAIR;
        } else if (cursor_name == "text") {
            sdl_cursor = SDL_SYSTEM_CURSOR_TEXT;
        } else if (cursor_name == "unavailable") {
            sdl_cursor = SDL_SYSTEM_CURSOR_NOT_ALLOWED;// SDL3: was SDL_SYSTEM_CURSOR_NOTALLOWED
        } else {
            // spdlog::trace("[RmlUi] Unknown cursor name: {}", cursor_name);
            return;
        }

        // Cache cursor if not already created
        // SDL3 has more cursor types, so we need a larger cache
        int cursor_index = static_cast<int>(sdl_cursor);
        if (cursor_index >= 0 && cursor_index < 20) {// Increased from 10 to 20 for SDL3
            if (!m_cursors[cursor_index]) {
                m_cursors[cursor_index] = SDL_CreateSystemCursor(sdl_cursor);
            }
            if (m_cursors[cursor_index]) {
                SDL_SetCursor(m_cursors[cursor_index]);
            }
        }
    }

    void RmlUiSystem::SetClipboardText(const Rml::String& text) {
        // SDL3 clipboard API
        if (SDL_SetClipboardText(text.c_str()) != 0) {
            // spdlog::warn("[RmlUi] Failed to set clipboard text: {}", SDL_GetError());
        }
    }

    void RmlUiSystem::GetClipboardText(Rml::String& text) {
        // SDL3 clipboard API
        char* clipboard_text = SDL_GetClipboardText();
        if (clipboard_text) {
            text = clipboard_text;
            SDL_free(clipboard_text);
        } else {
            text = "";
        }
    }

    void RmlUiSystem::ActivateKeyboard(Rml::Vector2f caret_position, float line_height) {
        // On mobile platforms, this would show the virtual keyboard
        // For desktop, this is typically a no-op
        (void)caret_position;
        (void)line_height;
    }

    void RmlUiSystem::DeactivateKeyboard() {
        // On mobile platforms, this would hide the virtual keyboard
    }

}// namespace Vapor
