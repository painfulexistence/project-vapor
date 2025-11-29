#pragma once

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <cstdint>

namespace Vapor {

/**
 * InputAction - Abstract game actions
 *
 * Maps physical input (keys, buttons) to logical game actions.
 * This allows for rebindable controls and platform-independent input handling.
 */
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

    // Camera switching
    SwitchToFlyCam,
    SwitchToFollowCam,

    // General actions
    Jump,
    Crouch,
    Sprint,
    Interact,
    Cancel,

    // Count for iteration
    COUNT
};

/**
 * InputState - Represents the current state of input actions
 *
 * Tracks which actions are held, pressed (this frame), or released (this frame).
 * Similar to Python InputState but for C++.
 *
 * Example:
 *     const InputState& state = inputManager.getInputState();
 *     if (state.isPressed(InputAction::Jump)) {
 *         player.jump();
 *     }
 *     if (state.isHeld(InputAction::MoveForward)) {
 *         player.moveForward(dt);
 *     }
 */
class InputState {
public:
    InputState() = default;

    /**
     * Check if action is currently held down.
     */
    bool isHeld(InputAction action) const {
        return m_heldActions.find(action) != m_heldActions.end();
    }

    /**
     * Check if action was just pressed this frame.
     */
    bool isPressed(InputAction action) const {
        return m_pressedActions.find(action) != m_pressedActions.end();
    }

    /**
     * Check if action was just released this frame.
     */
    bool isReleased(InputAction action) const {
        return m_releasedActions.find(action) != m_releasedActions.end();
    }

    /**
     * Get normalized movement vector from directional actions.
     *
     * Example:
     *     auto movement = state.getMovementVector(
     *         InputAction::StrafeLeft, InputAction::StrafeRight,
     *         InputAction::MoveBackward, InputAction::MoveForward
     *     );
     *     player.move(movement.x, movement.y);
     */
    glm::vec2 getMovementVector(
        InputAction left, InputAction right,
        InputAction backward, InputAction forward
    ) const {
        float x = (isHeld(right) ? 1.0f : 0.0f) - (isHeld(left) ? 1.0f : 0.0f);
        float y = (isHeld(forward) ? 1.0f : 0.0f) - (isHeld(backward) ? 1.0f : 0.0f);
        return glm::vec2(x, y);
    }

    /**
     * Get all currently held actions.
     */
    const std::unordered_set<InputAction>& getHeldActions() const {
        return m_heldActions;
    }

    /**
     * Get all actions pressed this frame.
     */
    const std::unordered_set<InputAction>& getPressedActions() const {
        return m_pressedActions;
    }

    /**
     * Get all actions released this frame.
     */
    const std::unordered_set<InputAction>& getReleasedActions() const {
        return m_releasedActions;
    }

private:
    std::unordered_set<InputAction> m_heldActions;
    std::unordered_set<InputAction> m_pressedActions;
    std::unordered_set<InputAction> m_releasedActions;

    friend class InputManager;
};

/**
 * InputEvent - Represents a single input event in the buffer
 */
struct InputEvent {
    InputAction action;
    uint64_t timestamp;  // Milliseconds
};

/**
 * InputManager - Input State Handling
 *
 * Manages input state capture and action mapping for keyboard/controller input.
 * Uses state-based input (not purely event-driven) for game controls.
 *
 * Features:
 * - Action-based input abstraction
 * - Input state tracking (held/pressed/released)
 * - Input event buffer (for debugging and combo detection)
 * - Rebindable key mappings
 * - Mouse position and delta tracking
 *
 * Usage:
 *     InputManager inputManager;
 *
 *     // Setup default key mappings
 *     inputManager.mapKey(SDL_SCANCODE_W, InputAction::MoveForward);
 *     inputManager.mapKey(SDL_SCANCODE_SPACE, InputAction::Jump);
 *
 *     // In main loop:
 *     while (SDL_PollEvent(&e)) {
 *         inputManager.processEvent(e);
 *     }
 *     inputManager.update(deltaTime);
 *
 *     // Query input state
 *     const InputState& state = inputManager.getInputState();
 *     if (state.isPressed(InputAction::Jump)) {
 *         player.jump();
 *     }
 */
class InputManager {
public:
    InputManager();
    ~InputManager() = default;

    /**
     * Process a single SDL event.
     * Call this for each event in your event polling loop.
     */
    void processEvent(const SDL_Event& event);

    /**
     * Update input state for the current frame.
     * Call this once per frame after processing all events.
     *
     * @param deltaTime Time elapsed since last frame (for time-based input buffer cleanup)
     */
    void update(float deltaTime);

    /**
     * Get the current frame's input state.
     */
    const InputState& getInputState() const { return m_currentState; }

    /**
     * Map a key to an action.
     *
     * Example:
     *     inputManager.mapKey(SDL_SCANCODE_W, InputAction::MoveForward);
     */
    void mapKey(SDL_Scancode key, InputAction action);

    /**
     * Unmap a key (remove its action binding).
     */
    void unmapKey(SDL_Scancode key);

    /**
     * Get the action bound to a key (if any).
     * Returns nullptr if key is not mapped.
     */
    const InputAction* getActionForKey(SDL_Scancode key) const;

    /**
     * Clear all key mappings.
     */
    void clearMappings();

    /**
     * Load default key mappings.
     * Call this to restore default controls.
     */
    void loadDefaultMappings();

    /**
     * Get the input event buffer (recent input events).
     * Useful for debugging or implementing combo systems.
     */
    const std::deque<InputEvent>& getInputBuffer() const { return m_inputBuffer; }

    /**
     * Set maximum size of input buffer.
     */
    void setInputBufferSize(size_t size) { m_maxBufferSize = size; }

    /**
     * Get mouse position in screen coordinates.
     */
    glm::vec2 getMousePosition() const { return m_mousePosition; }

    /**
     * Get mouse delta (movement since last frame).
     */
    glm::vec2 getMouseDelta() const { return m_mouseDelta; }

private:
    // Key to action mapping
    std::unordered_map<SDL_Scancode, InputAction> m_keyToAction;

    // Current input state
    InputState m_currentState;

    // Input event buffer (for recent input history)
    std::deque<InputEvent> m_inputBuffer;
    size_t m_maxBufferSize = 32;
    static constexpr uint64_t INPUT_EVENT_LIFETIME_MS = 1000;

    // Mouse state
    glm::vec2 m_mousePosition{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_prevMousePosition{0.0f};

    // Time tracking (in milliseconds)
    uint64_t m_currentTime = 0;

    // Helper to add event to buffer
    void addToBuffer(InputAction action);

    // Helper to clean old events from buffer
    void cleanBuffer();
};

} // namespace Vapor
