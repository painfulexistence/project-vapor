#include "Vapor/input_manager.hpp"

namespace Vapor {

InputManager::InputManager() {
    // Movement (WASD)
    mapKey(SDL_SCANCODE_W, InputAction::MoveForward);
    mapKey(SDL_SCANCODE_S, InputAction::MoveBackward);
    mapKey(SDL_SCANCODE_A, InputAction::StrafeLeft);
    mapKey(SDL_SCANCODE_D, InputAction::StrafeRight);
    mapKey(SDL_SCANCODE_R, InputAction::MoveUp);
    mapKey(SDL_SCANCODE_F, InputAction::MoveDown);

    // Camera rotation (IJKL + UO)
    mapKey(SDL_SCANCODE_I, InputAction::LookUp);
    mapKey(SDL_SCANCODE_K, InputAction::LookDown);
    mapKey(SDL_SCANCODE_J, InputAction::LookLeft);
    mapKey(SDL_SCANCODE_L, InputAction::LookRight);
    mapKey(SDL_SCANCODE_U, InputAction::RollLeft);
    mapKey(SDL_SCANCODE_O, InputAction::RollRight);

    // General actions
    mapKey(SDL_SCANCODE_SPACE, InputAction::Jump);
    mapKey(SDL_SCANCODE_LSHIFT, InputAction::Sprint);
    mapKey(SDL_SCANCODE_LCTRL, InputAction::Crouch);
    mapKey(SDL_SCANCODE_E, InputAction::Interact);
    mapKey(SDL_SCANCODE_ESCAPE, InputAction::Cancel);

    // Hotkeys
    mapKey(SDL_SCANCODE_1, InputAction::Hotkey1);
    mapKey(SDL_SCANCODE_2, InputAction::Hotkey2);
    mapKey(SDL_SCANCODE_3, InputAction::Hotkey3);
    mapKey(SDL_SCANCODE_4, InputAction::Hotkey4);
    mapKey(SDL_SCANCODE_5, InputAction::Hotkey5);
    mapKey(SDL_SCANCODE_6, InputAction::Hotkey6);
    mapKey(SDL_SCANCODE_7, InputAction::Hotkey7);
    mapKey(SDL_SCANCODE_8, InputAction::Hotkey8);
    mapKey(SDL_SCANCODE_9, InputAction::Hotkey9);
    mapKey(SDL_SCANCODE_0, InputAction::Hotkey10);
}

void InputManager::processEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        auto it = keyToAction.find(event.key.scancode);
        if (it != keyToAction.end()) {
            InputAction action = it->second;
            // Only add to pressed if not already held
            if (currentState.heldActions.find(action) == currentState.heldActions.end()) {
                currentState.heldActions.insert(action);
                currentState.pressedActions.insert(action);

                inputHistory.push_back({action, currentTime});
            }
        }
        break;
    }
    case SDL_EVENT_KEY_UP: {
        auto it = keyToAction.find(event.key.scancode);
        if (it != keyToAction.end()) {
            InputAction action = it->second;
            // Only add to released if it was held
            if (currentState.heldActions.find(action) != currentState.heldActions.end()) {
                currentState.heldActions.erase(action);
                currentState.releasedActions.insert(action);

                inputHistory.push_back({action, currentTime});
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        currMousePosition = glm::vec2(event.motion.x, event.motion.y);
        break;
    }
    default:
        break;
    }
    while (inputHistory.size() > MAX_INPUT_HISTORY_SIZE) {
        inputHistory.pop_front();
    }
}

// TODO: get time directly from SDL
void InputManager::update(float deltaTime) {
    // Update time (convert deltaTime from seconds to milliseconds)
    currentTime += static_cast<uint64_t>(deltaTime * 1000.0f);

    // Clear pressed and released actions (they only last one frame)
    currentState.pressedActions.clear();
    currentState.releasedActions.clear();

    // Update mouse delta
    mouseDelta = currMousePosition - prevMousePosition;
    prevMousePosition = currMousePosition;

    while (!inputHistory.empty() && (currentTime - inputHistory.front().timestamp > INPUT_EVENT_LIFETIME_MS)) {
        inputHistory.pop_front();
    }
}

void InputManager::mapKey(SDL_Scancode key, InputAction action)
{
    keyToAction[key] = action;
}

void InputManager::unmapKey(SDL_Scancode key)
{
    keyToAction.erase(key);
}

void InputManager::updateMappings(const std::unordered_map<SDL_Scancode, InputAction>& mappings)
{
    keyToAction = mappings;
}

void InputManager::clearMappings()
{
    keyToAction.clear();
}

const InputAction InputManager::getActionForKey(SDL_Scancode key) const
{
    auto it = keyToAction.find(key);
    if (it != keyToAction.end()) {
        return it->second;
    }
    return InputAction::UNKNOWN;
}

} // namespace Vapor
