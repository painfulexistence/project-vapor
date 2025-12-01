#pragma once
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>

namespace Vapor {

// RmlUi System Interface implementation using SDL3
class RmlUi_SystemInterface_SDL3 : public Rml::SystemInterface {
public:
    RmlUi_SystemInterface_SDL3();
    ~RmlUi_SystemInterface_SDL3() override = default;

    // Get elapsed time since application start
    double GetElapsedTime() override;

    // Set the mouse cursor style
    void SetMouseCursor(const Rml::String& cursor_name) override;

    // Set clipboard text
    void SetClipboardText(const Rml::String& text) override;

    // Get clipboard text
    void GetClipboardText(Rml::String& text) override;

    // Activate keyboard input (for IME)
    void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override;

    // Deactivate keyboard input
    void DeactivateKeyboard() override;

    // Log message
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

    // Convert SDL3 key code to RmlUi key identifier
    static Rml::Input::KeyIdentifier ConvertKey(SDL_Keycode sdl_key);

    // Convert SDL3 key modifiers to RmlUi key modifiers
    static int ConvertKeyModifiers(SDL_Keymod sdl_mods);

    // Process SDL3 event for RmlUi
    // Returns true if the event was consumed by RmlUi
    static bool ProcessEvent(Rml::Context* context, const SDL_Event& event);

private:
    SDL_Cursor* cursor_default = nullptr;
    SDL_Cursor* cursor_move = nullptr;
    SDL_Cursor* cursor_pointer = nullptr;
    SDL_Cursor* cursor_resize = nullptr;
    SDL_Cursor* cursor_cross = nullptr;
    SDL_Cursor* cursor_text = nullptr;
    SDL_Cursor* cursor_unavailable = nullptr;
};

} // namespace Vapor
