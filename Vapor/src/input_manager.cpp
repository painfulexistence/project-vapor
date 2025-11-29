#include "Vapor/input_manager.hpp"
#include <algorithm>

namespace Vapor {

InputManager::InputManager()
{
    loadDefaultMappings();
}

void InputManager::processEvent(const SDL_Event& event)
{
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN: {
            auto it = m_keyToAction.find(event.key.scancode);
            if (it != m_keyToAction.end()) {
                InputAction action = it->second;

                // Only add to pressed if not already held
                if (m_currentState.m_heldActions.find(action) == m_currentState.m_heldActions.end()) {
                    m_currentState.m_heldActions.insert(action);
                    m_currentState.m_pressedActions.insert(action);
                    addToBuffer(action);
                }
            }
            break;
        }

        case SDL_EVENT_KEY_UP: {
            auto it = m_keyToAction.find(event.key.scancode);
            if (it != m_keyToAction.end()) {
                InputAction action = it->second;

                // Only add to released if it was held
                if (m_currentState.m_heldActions.find(action) != m_currentState.m_heldActions.end()) {
                    m_currentState.m_heldActions.erase(action);
                    m_currentState.m_releasedActions.insert(action);
                }
            }
            break;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            m_mousePosition = glm::vec2(event.motion.x, event.motion.y);
            break;
        }

        default:
            break;
    }
}

void InputManager::update(float deltaTime)
{
    // Update time (convert deltaTime from seconds to milliseconds)
    m_currentTime += static_cast<uint64_t>(deltaTime * 1000.0f);

    // Clear pressed and released actions (they only last one frame)
    m_currentState.m_pressedActions.clear();
    m_currentState.m_releasedActions.clear();

    // Update mouse delta
    m_mouseDelta = m_mousePosition - m_prevMousePosition;
    m_prevMousePosition = m_mousePosition;

    // Clean old events from buffer
    cleanBuffer();
}

void InputManager::mapKey(SDL_Scancode key, InputAction action)
{
    m_keyToAction[key] = action;
}

void InputManager::unmapKey(SDL_Scancode key)
{
    m_keyToAction.erase(key);
}

const InputAction* InputManager::getActionForKey(SDL_Scancode key) const
{
    auto it = m_keyToAction.find(key);
    if (it != m_keyToAction.end()) {
        return &it->second;
    }
    return nullptr;
}

void InputManager::clearMappings()
{
    m_keyToAction.clear();
}

void InputManager::loadDefaultMappings()
{
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

    // Camera switching (1/2)
    mapKey(SDL_SCANCODE_1, InputAction::SwitchToFlyCam);
    mapKey(SDL_SCANCODE_2, InputAction::SwitchToFollowCam);

    // General actions
    mapKey(SDL_SCANCODE_SPACE, InputAction::Jump);
    mapKey(SDL_SCANCODE_LSHIFT, InputAction::Sprint);
    mapKey(SDL_SCANCODE_LCTRL, InputAction::Crouch);
    mapKey(SDL_SCANCODE_E, InputAction::Interact);
    mapKey(SDL_SCANCODE_ESCAPE, InputAction::Cancel);
}

void InputManager::addToBuffer(InputAction action)
{
    m_inputBuffer.push_back({action, m_currentTime});

    // Enforce max buffer size
    while (m_inputBuffer.size() > m_maxBufferSize) {
        m_inputBuffer.pop_front();
    }
}

void InputManager::cleanBuffer()
{
    // Remove events older than INPUT_EVENT_LIFETIME_MS
    while (!m_inputBuffer.empty()) {
        const auto& oldestEvent = m_inputBuffer.front();
        if (m_currentTime - oldestEvent.timestamp > INPUT_EVENT_LIFETIME_MS) {
            m_inputBuffer.pop_front();
        } else {
            break;  // Events are ordered by time, so we can stop here
        }
    }
}

} // namespace Vapor
