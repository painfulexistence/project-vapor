#include "camera_manager.hpp"

#include <glm/glm.hpp>
#include <fmt/core.h>
#include <stdexcept>

namespace Vapor {

// ============================================================================
// FlyCam Implementation
// ============================================================================

FlyCam::FlyCam(
    glm::vec3 eye,
    glm::vec3 center,
    glm::vec3 up,
    float fov,
    float aspect,
    float near,
    float far,
    float moveSpeed,
    float rotateSpeed
) : _camera(eye, center, up, fov, aspect, near, far),
    _initialEye(eye),
    _initialCenter(center),
    _initialUp(up),
    _moveSpeed(moveSpeed),
    _rotateSpeed(rotateSpeed)
{
}

void FlyCam::update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keyboardState) {
    // Movement controls (WASDRF)
    float moveDistance = _moveSpeed * deltaTime;

    // W/S - Dolly (forward/backward)
    if (keyboardState.count(SDL_SCANCODE_W) && keyboardState.at(SDL_SCANCODE_W)) {
        _camera.dolly(moveDistance);
    }
    if (keyboardState.count(SDL_SCANCODE_S) && keyboardState.at(SDL_SCANCODE_S)) {
        _camera.dolly(-moveDistance);
    }

    // A/D - Truck (strafe left/right)
    if (keyboardState.count(SDL_SCANCODE_A) && keyboardState.at(SDL_SCANCODE_A)) {
        _camera.truck(-moveDistance);
    }
    if (keyboardState.count(SDL_SCANCODE_D) && keyboardState.at(SDL_SCANCODE_D)) {
        _camera.truck(moveDistance);
    }

    // R/F - Pedestal (up/down)
    if (keyboardState.count(SDL_SCANCODE_R) && keyboardState.at(SDL_SCANCODE_R)) {
        _camera.pedestal(moveDistance);
    }
    if (keyboardState.count(SDL_SCANCODE_F) && keyboardState.at(SDL_SCANCODE_F)) {
        _camera.pedestal(-moveDistance);
    }

    // Rotation controls (IJKL UO)
    float rotateAngle = _rotateSpeed * deltaTime;

    // I/K - Tilt (look up/down)
    if (keyboardState.count(SDL_SCANCODE_I) && keyboardState.at(SDL_SCANCODE_I)) {
        _camera.tilt(rotateAngle);
    }
    if (keyboardState.count(SDL_SCANCODE_K) && keyboardState.at(SDL_SCANCODE_K)) {
        _camera.tilt(-rotateAngle);
    }

    // J/L - Pan (turn left/right)
    if (keyboardState.count(SDL_SCANCODE_J) && keyboardState.at(SDL_SCANCODE_J)) {
        _camera.pan(rotateAngle);
    }
    if (keyboardState.count(SDL_SCANCODE_L) && keyboardState.at(SDL_SCANCODE_L)) {
        _camera.pan(-rotateAngle);
    }

    // U/O - Roll
    if (keyboardState.count(SDL_SCANCODE_U) && keyboardState.at(SDL_SCANCODE_U)) {
        _camera.roll(rotateAngle);
    }
    if (keyboardState.count(SDL_SCANCODE_O) && keyboardState.at(SDL_SCANCODE_O)) {
        _camera.roll(-rotateAngle);
    }
}

void FlyCam::reset() {
    _camera = Camera(_initialEye, _initialCenter, _initialUp,
                     _camera.near(), _camera.far());
}

// ============================================================================
// FollowCam Implementation
// ============================================================================

FollowCam::FollowCam(
    std::shared_ptr<Node> target,
    glm::vec3 offset,
    float fov,
    float aspect,
    float near,
    float far,
    float smoothFactor,
    float deadzone
) : _camera(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f),
            fov, aspect, near, far),
    _target(target),
    _offset(offset),
    _currentPosition(glm::vec3(0.0f)),
    _initialOffset(offset),
    _smoothFactor(smoothFactor),
    _deadzone(deadzone),
    _fov(fov),
    _aspect(aspect),
    _near(near),
    _far(far)
{
    if (target) {
        _currentPosition = target->getWorldPosition() + _offset;
        _camera.setEye(_currentPosition);
    }
}

void FollowCam::update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keyboardState) {
    // Unused parameter for interface compatibility
    (void)keyboardState;
    (void)deltaTime;

    if (!_target) {
        return;
    }

    // Get target position in world space
    glm::vec3 targetPosition = _target->getWorldPosition();
    glm::vec3 desiredPosition = targetPosition + _offset;

    // Calculate distance to desired position
    float distance = glm::length(desiredPosition - _currentPosition);

    // Only update if outside deadzone
    if (distance > _deadzone) {
        // Smooth interpolation to desired position
        _currentPosition += (desiredPosition - _currentPosition) * _smoothFactor;
    }

    // Camera looks at target position (slightly above for better view)
    glm::vec3 lookAtPoint = targetPosition + glm::vec3(0.0f, 0.5f, 0.0f);

    // Recreate camera with new position and look-at point
    _camera = Camera(
        _currentPosition,
        lookAtPoint,
        glm::vec3(0.0f, 1.0f, 0.0f),
        _fov,
        _aspect,
        _near,
        _far
    );
}

void FollowCam::reset() {
    _offset = _initialOffset;
    if (_target) {
        _currentPosition = _target->getWorldPosition() + _offset;
        _camera.setEye(_currentPosition);
    }
}

// ============================================================================
// CameraManager Implementation
// ============================================================================

void CameraManager::addCamera(const std::string& name, std::unique_ptr<VirtualCamera> camera) {
    if (_cameras.find(name) != _cameras.end()) {
        throw std::runtime_error(fmt::format("Camera '{}' already exists", name));
    }

    _cameras[name] = std::move(camera);

    // If this is the first camera, make it current
    if (_currentCameraName.empty()) {
        _currentCameraName = name;
    }

    fmt::print("[CameraManager] Added camera: '{}'\n", name);
}

void CameraManager::switchCamera(const std::string& name) {
    if (_cameras.find(name) == _cameras.end()) {
        throw std::runtime_error(fmt::format("Camera '{}' not found", name));
    }

    _currentCameraName = name;
    fmt::print("[CameraManager] Switched to camera: '{}'\n", name);
}

VirtualCamera* CameraManager::getCurrentCamera() {
    if (_currentCameraName.empty() || _cameras.empty()) {
        return nullptr;
    }

    auto it = _cameras.find(_currentCameraName);
    if (it == _cameras.end()) {
        return nullptr;
    }

    return it->second.get();
}

VirtualCamera* CameraManager::getCamera(const std::string& name) {
    auto it = _cameras.find(name);
    if (it == _cameras.end()) {
        return nullptr;
    }
    return it->second.get();
}

void CameraManager::update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keyboardState) {
    VirtualCamera* current = getCurrentCamera();
    if (current) {
        current->update(deltaTime, keyboardState);
    }
}

bool CameraManager::hasCamera(const std::string& name) const {
    return _cameras.find(name) != _cameras.end();
}

bool CameraManager::removeCamera(const std::string& name) {
    auto it = _cameras.find(name);
    if (it == _cameras.end()) {
        return false;
    }

    // If we're removing the current camera, switch to another one
    if (_currentCameraName == name) {
        _cameras.erase(it);
        if (!_cameras.empty()) {
            _currentCameraName = _cameras.begin()->first;
        } else {
            _currentCameraName.clear();
        }
    } else {
        _cameras.erase(it);
    }

    fmt::print("[CameraManager] Removed camera: '{}'\n", name);
    return true;
}

void CameraManager::resetCurrentCamera() {
    VirtualCamera* current = getCurrentCamera();
    if (current) {
        current->reset();
        fmt::print("[CameraManager] Reset current camera\n");
    }
}

} // namespace Vapor
