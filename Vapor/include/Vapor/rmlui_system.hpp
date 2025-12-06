#pragma once

#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>
#include <SDL3/SDL.h>
#include <chrono>

namespace Vapor {

/**
 * RmlUI System Interface implementation using SDL3
 * Handles time, logging, clipboard, and cursor management
 */
class RmlUiSystem : public Rml::SystemInterface {
public:
    RmlUiSystem();
    ~RmlUiSystem() override;

    // Get elapsed time since initialization
    double GetElapsedTime() override;

    // Log message handling
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

    // Mouse cursor management
    void SetMouseCursor(const Rml::String& cursor_name) override;

    // Clipboard operations
    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;

    // Keyboard activation (for mobile platforms)
    void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override;
    void DeactivateKeyboard() override;

private:
    double m_start_time;
    SDL_Cursor* m_cursors[20];  // Cache for different cursor types (increased for SDL3)
};

} // namespace Vapor

