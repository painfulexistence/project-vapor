#include "Vapor/rmlui_system.hpp"
#include <fmt/core.h>
#include <RmlUi/Core/Context.h>

namespace Vapor {

RmlUi_SystemInterface_SDL3::RmlUi_SystemInterface_SDL3() {
    cursor_default = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    cursor_move = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    cursor_pointer = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    cursor_resize = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
    cursor_cross = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    cursor_text = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    cursor_unavailable = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
}

double RmlUi_SystemInterface_SDL3::GetElapsedTime() {
    return static_cast<double>(SDL_GetTicks()) / 1000.0;
}

void RmlUi_SystemInterface_SDL3::SetMouseCursor(const Rml::String& cursor_name) {
    SDL_Cursor* cursor = cursor_default;

    if (cursor_name.empty() || cursor_name == "arrow") {
        cursor = cursor_default;
    } else if (cursor_name == "move") {
        cursor = cursor_move;
    } else if (cursor_name == "pointer") {
        cursor = cursor_pointer;
    } else if (cursor_name == "resize") {
        cursor = cursor_resize;
    } else if (cursor_name == "cross") {
        cursor = cursor_cross;
    } else if (cursor_name == "text") {
        cursor = cursor_text;
    } else if (cursor_name == "unavailable") {
        cursor = cursor_unavailable;
    } else if (cursor_name.find("resize") != Rml::String::npos) {
        cursor = cursor_resize;
    }

    if (cursor) {
        SDL_SetCursor(cursor);
    }
}

void RmlUi_SystemInterface_SDL3::SetClipboardText(const Rml::String& text) {
    SDL_SetClipboardText(text.c_str());
}

void RmlUi_SystemInterface_SDL3::GetClipboardText(Rml::String& text) {
    char* sdl_text = SDL_GetClipboardText();
    if (sdl_text) {
        text = sdl_text;
        SDL_free(sdl_text);
    }
}

void RmlUi_SystemInterface_SDL3::ActivateKeyboard(Rml::Vector2f caret_position, float line_height) {
    SDL_Rect rect;
    rect.x = static_cast<int>(caret_position.x);
    rect.y = static_cast<int>(caret_position.y);
    rect.w = 1;
    rect.h = static_cast<int>(line_height);
    SDL_SetTextInputArea(nullptr, &rect, 0);
    SDL_StartTextInput(nullptr);
}

void RmlUi_SystemInterface_SDL3::DeactivateKeyboard() {
    SDL_StopTextInput(nullptr);
}

bool RmlUi_SystemInterface_SDL3::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    const char* type_str = "";
    switch (type) {
        case Rml::Log::LT_ERROR:   type_str = "ERROR";   break;
        case Rml::Log::LT_ASSERT:  type_str = "ASSERT";  break;
        case Rml::Log::LT_WARNING: type_str = "WARNING"; break;
        case Rml::Log::LT_INFO:    type_str = "INFO";    break;
        case Rml::Log::LT_DEBUG:   type_str = "DEBUG";   break;
        default: break;
    }
    fmt::print("[RmlUi {}] {}\n", type_str, message);
    return true;
}

Rml::Input::KeyIdentifier RmlUi_SystemInterface_SDL3::ConvertKey(SDL_Keycode sdl_key) {
    // Convert SDL3 keycodes to RmlUi key identifiers
    switch (sdl_key) {
        case SDLK_A: return Rml::Input::KI_A;
        case SDLK_B: return Rml::Input::KI_B;
        case SDLK_C: return Rml::Input::KI_C;
        case SDLK_D: return Rml::Input::KI_D;
        case SDLK_E: return Rml::Input::KI_E;
        case SDLK_F: return Rml::Input::KI_F;
        case SDLK_G: return Rml::Input::KI_G;
        case SDLK_H: return Rml::Input::KI_H;
        case SDLK_I: return Rml::Input::KI_I;
        case SDLK_J: return Rml::Input::KI_J;
        case SDLK_K: return Rml::Input::KI_K;
        case SDLK_L: return Rml::Input::KI_L;
        case SDLK_M: return Rml::Input::KI_M;
        case SDLK_N: return Rml::Input::KI_N;
        case SDLK_O: return Rml::Input::KI_O;
        case SDLK_P: return Rml::Input::KI_P;
        case SDLK_Q: return Rml::Input::KI_Q;
        case SDLK_R: return Rml::Input::KI_R;
        case SDLK_S: return Rml::Input::KI_S;
        case SDLK_T: return Rml::Input::KI_T;
        case SDLK_U: return Rml::Input::KI_U;
        case SDLK_V: return Rml::Input::KI_V;
        case SDLK_W: return Rml::Input::KI_W;
        case SDLK_X: return Rml::Input::KI_X;
        case SDLK_Y: return Rml::Input::KI_Y;
        case SDLK_Z: return Rml::Input::KI_Z;

        case SDLK_0: return Rml::Input::KI_0;
        case SDLK_1: return Rml::Input::KI_1;
        case SDLK_2: return Rml::Input::KI_2;
        case SDLK_3: return Rml::Input::KI_3;
        case SDLK_4: return Rml::Input::KI_4;
        case SDLK_5: return Rml::Input::KI_5;
        case SDLK_6: return Rml::Input::KI_6;
        case SDLK_7: return Rml::Input::KI_7;
        case SDLK_8: return Rml::Input::KI_8;
        case SDLK_9: return Rml::Input::KI_9;

        case SDLK_KP_0: return Rml::Input::KI_NUMPAD0;
        case SDLK_KP_1: return Rml::Input::KI_NUMPAD1;
        case SDLK_KP_2: return Rml::Input::KI_NUMPAD2;
        case SDLK_KP_3: return Rml::Input::KI_NUMPAD3;
        case SDLK_KP_4: return Rml::Input::KI_NUMPAD4;
        case SDLK_KP_5: return Rml::Input::KI_NUMPAD5;
        case SDLK_KP_6: return Rml::Input::KI_NUMPAD6;
        case SDLK_KP_7: return Rml::Input::KI_NUMPAD7;
        case SDLK_KP_8: return Rml::Input::KI_NUMPAD8;
        case SDLK_KP_9: return Rml::Input::KI_NUMPAD9;

        case SDLK_LEFT:  return Rml::Input::KI_LEFT;
        case SDLK_RIGHT: return Rml::Input::KI_RIGHT;
        case SDLK_UP:    return Rml::Input::KI_UP;
        case SDLK_DOWN:  return Rml::Input::KI_DOWN;

        case SDLK_KP_PLUS:    return Rml::Input::KI_ADD;
        case SDLK_KP_MINUS:   return Rml::Input::KI_SUBTRACT;
        case SDLK_KP_MULTIPLY: return Rml::Input::KI_MULTIPLY;
        case SDLK_KP_DIVIDE:  return Rml::Input::KI_DIVIDE;
        case SDLK_KP_ENTER:   return Rml::Input::KI_NUMPADENTER;
        case SDLK_KP_DECIMAL: return Rml::Input::KI_DECIMAL;

        case SDLK_BACKSPACE: return Rml::Input::KI_BACK;
        case SDLK_TAB:       return Rml::Input::KI_TAB;
        case SDLK_CLEAR:     return Rml::Input::KI_CLEAR;
        case SDLK_RETURN:    return Rml::Input::KI_RETURN;
        case SDLK_PAUSE:     return Rml::Input::KI_PAUSE;
        case SDLK_CAPSLOCK:  return Rml::Input::KI_CAPITAL;
        case SDLK_ESCAPE:    return Rml::Input::KI_ESCAPE;
        case SDLK_SPACE:     return Rml::Input::KI_SPACE;

        case SDLK_PAGEUP:   return Rml::Input::KI_PRIOR;
        case SDLK_PAGEDOWN: return Rml::Input::KI_NEXT;
        case SDLK_END:      return Rml::Input::KI_END;
        case SDLK_HOME:     return Rml::Input::KI_HOME;
        case SDLK_INSERT:   return Rml::Input::KI_INSERT;
        case SDLK_DELETE:   return Rml::Input::KI_DELETE;

        case SDLK_LSHIFT: return Rml::Input::KI_LSHIFT;
        case SDLK_RSHIFT: return Rml::Input::KI_RSHIFT;
        case SDLK_LCTRL:  return Rml::Input::KI_LCONTROL;
        case SDLK_RCTRL:  return Rml::Input::KI_RCONTROL;
        case SDLK_LALT:   return Rml::Input::KI_LMENU;
        case SDLK_RALT:   return Rml::Input::KI_RMENU;
        case SDLK_LGUI:   return Rml::Input::KI_LMETA;
        case SDLK_RGUI:   return Rml::Input::KI_RMETA;

        case SDLK_F1:  return Rml::Input::KI_F1;
        case SDLK_F2:  return Rml::Input::KI_F2;
        case SDLK_F3:  return Rml::Input::KI_F3;
        case SDLK_F4:  return Rml::Input::KI_F4;
        case SDLK_F5:  return Rml::Input::KI_F5;
        case SDLK_F6:  return Rml::Input::KI_F6;
        case SDLK_F7:  return Rml::Input::KI_F7;
        case SDLK_F8:  return Rml::Input::KI_F8;
        case SDLK_F9:  return Rml::Input::KI_F9;
        case SDLK_F10: return Rml::Input::KI_F10;
        case SDLK_F11: return Rml::Input::KI_F11;
        case SDLK_F12: return Rml::Input::KI_F12;
        case SDLK_F13: return Rml::Input::KI_F13;
        case SDLK_F14: return Rml::Input::KI_F14;
        case SDLK_F15: return Rml::Input::KI_F15;

        case SDLK_NUMLOCKCLEAR: return Rml::Input::KI_NUMLOCK;
        case SDLK_SCROLLLOCK:   return Rml::Input::KI_SCROLL;

        case SDLK_SEMICOLON:    return Rml::Input::KI_OEM_1;
        case SDLK_EQUALS:       return Rml::Input::KI_OEM_PLUS;
        case SDLK_COMMA:        return Rml::Input::KI_OEM_COMMA;
        case SDLK_MINUS:        return Rml::Input::KI_OEM_MINUS;
        case SDLK_PERIOD:       return Rml::Input::KI_OEM_PERIOD;
        case SDLK_SLASH:        return Rml::Input::KI_OEM_2;
        case SDLK_GRAVE:        return Rml::Input::KI_OEM_3;
        case SDLK_LEFTBRACKET:  return Rml::Input::KI_OEM_4;
        case SDLK_BACKSLASH:    return Rml::Input::KI_OEM_5;
        case SDLK_RIGHTBRACKET: return Rml::Input::KI_OEM_6;
        case SDLK_APOSTROPHE:   return Rml::Input::KI_OEM_7;

        default: return Rml::Input::KI_UNKNOWN;
    }
}

int RmlUi_SystemInterface_SDL3::ConvertKeyModifiers(SDL_Keymod sdl_mods) {
    int rml_mods = 0;
    if (sdl_mods & SDL_KMOD_CTRL)
        rml_mods |= Rml::Input::KM_CTRL;
    if (sdl_mods & SDL_KMOD_SHIFT)
        rml_mods |= Rml::Input::KM_SHIFT;
    if (sdl_mods & SDL_KMOD_ALT)
        rml_mods |= Rml::Input::KM_ALT;
    if (sdl_mods & SDL_KMOD_GUI)
        rml_mods |= Rml::Input::KM_META;
    if (sdl_mods & SDL_KMOD_NUM)
        rml_mods |= Rml::Input::KM_NUMLOCK;
    if (sdl_mods & SDL_KMOD_CAPS)
        rml_mods |= Rml::Input::KM_CAPSLOCK;
    return rml_mods;
}

bool RmlUi_SystemInterface_SDL3::ProcessEvent(Rml::Context* context, const SDL_Event& event) {
    if (!context)
        return false;

    switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            int key_state = ConvertKeyModifiers(SDL_GetModState());
            return context->ProcessMouseMove(
                static_cast<int>(event.motion.x),
                static_cast<int>(event.motion.y),
                key_state
            );
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            int button = 0;
            switch (event.button.button) {
                case SDL_BUTTON_LEFT:   button = 0; break;
                case SDL_BUTTON_RIGHT:  button = 1; break;
                case SDL_BUTTON_MIDDLE: button = 2; break;
                default: return false;
            }
            int key_state = ConvertKeyModifiers(SDL_GetModState());
            return context->ProcessMouseButtonDown(button, key_state);
        }

        case SDL_EVENT_MOUSE_BUTTON_UP: {
            int button = 0;
            switch (event.button.button) {
                case SDL_BUTTON_LEFT:   button = 0; break;
                case SDL_BUTTON_RIGHT:  button = 1; break;
                case SDL_BUTTON_MIDDLE: button = 2; break;
                default: return false;
            }
            int key_state = ConvertKeyModifiers(SDL_GetModState());
            return context->ProcessMouseButtonUp(button, key_state);
        }

        case SDL_EVENT_MOUSE_WHEEL: {
            int key_state = ConvertKeyModifiers(SDL_GetModState());
            return context->ProcessMouseWheel(
                Rml::Vector2f(event.wheel.x, -event.wheel.y),
                key_state
            );
        }

        case SDL_EVENT_KEY_DOWN: {
            Rml::Input::KeyIdentifier key = ConvertKey(event.key.key);
            int key_state = ConvertKeyModifiers(SDL_GetModState());
            if (key != Rml::Input::KI_UNKNOWN) {
                return context->ProcessKeyDown(key, key_state);
            }
            return false;
        }

        case SDL_EVENT_KEY_UP: {
            Rml::Input::KeyIdentifier key = ConvertKey(event.key.key);
            int key_state = ConvertKeyModifiers(SDL_GetModState());
            if (key != Rml::Input::KI_UNKNOWN) {
                return context->ProcessKeyUp(key, key_state);
            }
            return false;
        }

        case SDL_EVENT_TEXT_INPUT: {
            return context->ProcessTextInput(Rml::String(event.text.text));
        }

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            context->SetDimensions(Rml::Vector2i(event.window.data1, event.window.data2));
            return false;
        }

        default:
            return false;
    }
}

} // namespace Vapor
