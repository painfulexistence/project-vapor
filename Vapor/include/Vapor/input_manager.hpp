#pragma once

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <cstdint>

namespace Vapor {

enum class InputAction {
    // Movement
    MoveForward,
    MoveBackward,
    StrafeLeft,
    StrafeRight,
    MoveUp,
    MoveDown,

    // Camera rotation
    LookUp,
    LookDown,
    LookLeft,
    LookRight,
    RollLeft,
    RollRight,

    // General actions
    Jump,
    Crouch,
    Sprint,
    Interact,
    Cancel,

    // Hotkeys
    Hotkey1,
    Hotkey2,
    Hotkey3,
    Hotkey4,
    Hotkey5,
    Hotkey6,
    Hotkey7,
    Hotkey8,
    Hotkey9,
    Hotkey10,

    // Count for iteration
    UNKNOWN
};

class InputState {
public:
    InputState() = default;

    /**
     * Check if action is currently held down.
     */
    bool isHeld(InputAction action) const {
        return heldActions.find(action) != heldActions.end();
    }

    /**
     * Check if action was just pressed this frame.
     */
    bool isPressed(InputAction action) const {
        return pressedActions.find(action) != pressedActions.end();
    }

    /**
     * Check if action was just released this frame.
     */
    bool isReleased(InputAction action) const {
        return releasedActions.find(action) != releasedActions.end();
    }

    glm::vec2 getMovementVector(
        InputAction left, InputAction right,
        InputAction backward, InputAction forward
    ) const {
        float x = (isHeld(right) ? 1.0f : 0.0f) - (isHeld(left) ? 1.0f : 0.0f);
        float y = (isHeld(forward) ? 1.0f : 0.0f) - (isHeld(backward) ? 1.0f : 0.0f);
        return glm::vec2(x, y);
    }

    const std::unordered_set<InputAction>& getHeldActions() const {
        return heldActions;
    }

    const std::unordered_set<InputAction>& getPressedActions() const {
        return pressedActions;
    }

    const std::unordered_set<InputAction>& getReleasedActions() const {
        return releasedActions;
    }

private:
    std::unordered_set<InputAction> heldActions;
    std::unordered_set<InputAction> pressedActions;
    std::unordered_set<InputAction> releasedActions;

    friend class InputManager;
};


struct InputEvent {
    InputAction action;
    uint64_t timestamp;  // Milliseconds
};


class InputManager {
public:
    InputManager();
    ~InputManager() = default;

    void processEvent(const SDL_Event& event);

    void update(float deltaTime);

    const InputState& getInputState() const { return currentState; }

    void mapKey(SDL_Scancode key, InputAction action);

    void unmapKey(SDL_Scancode key);

    void updateMappings(const std::unordered_map<SDL_Scancode, InputAction>& mappings);

    void clearMappings();

    const InputAction getActionForKey(SDL_Scancode key) const;

    const std::deque<InputEvent>& getInputBuffer() const { return inputHistory; }

    glm::vec2 getMousePosition() const { return currMousePosition; }

    glm::vec2 getMouseDelta() const { return mouseDelta; }

private:
    std::unordered_map<SDL_Scancode, InputAction> keyToAction;

    InputState currentState;

    std::deque<InputEvent> inputHistory;
    static constexpr size_t MAX_INPUT_HISTORY_SIZE = 32;
    static constexpr uint64_t INPUT_EVENT_LIFETIME_MS = 1000;

    // Mouse state
    glm::vec2 currMousePosition{0.0f};
    glm::vec2 mouseDelta{0.0f};
    glm::vec2 prevMousePosition{0.0f};

    // Time tracking (in milliseconds)
    uint64_t currentTime = 0;

    bool wasActionPressedRecently(InputAction action, float timeWindow) const {
        float now = (float)SDL_GetTicks() / 1000.0f;
        for (const auto& event : inputHistory) {
            if (event.action == action) {
                if (now - event.timestamp <= timeWindow) {
                    return true;
                }
            }
        }
        return false;
    }
};

} // namespace Vapor
