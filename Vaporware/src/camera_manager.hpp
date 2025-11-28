#pragma once

#include "Vapor/camera.hpp"
#include "Vapor/scene.hpp"
#include <SDL3/SDL.h>
#include <string>
#include <memory>
#include <unordered_map>

namespace Vapor {

/**
 * ICamera - Abstract interface for all camera types
 *
 * Provides a common interface for different camera implementations.
 * All cameras must implement update() and provide access to the underlying Camera.
 */
class VirtualCamera {
public:
    virtual ~VirtualCamera() = default;

    /**
     * Update camera state based on input and time
     * @param deltaTime Time elapsed since last frame (seconds)
     * @param keyboardState Current keyboard state map
     */
    virtual void update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keyboardState) = 0;

    /**
     * Get the underlying Camera object for rendering
     * @return Reference to the Camera instance
     */
    virtual Camera& getCamera() = 0;

    /**
     * Reset camera to initial state
     */
    virtual void reset() = 0;
};

/**
 * FlyCam - Free-flying camera with WASDRF controls
 *
 * Supports full 6-DOF movement with keyboard controls:
 * - W/S: Move forward/backward (dolly)
 * - A/D: Strafe left/right (truck)
 * - R/F: Move up/down (pedestal)
 * - I/K: Look up/down (tilt)
 * - J/L: Turn left/right (pan)
 * - U/O: Roll camera (roll)
 *
 * Example:
 *     auto flyCam = std::make_unique<FlyCam>(
 *         glm::vec3(0, 2, 5),  // Initial position
 *         glm::vec3(0, 0, 0),  // Look at point
 *         fov, aspect, near, far
 *     );
 *     flyCam->update(dt, keyboardState);
 */
class FlyCam : public VirtualCamera {
public:
    FlyCam(
        glm::vec3 eye = glm::vec3(0.0f, 0.0f, 3.0f),
        glm::vec3 center = glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f),
        float fov = glm::radians(60.0f),
        float aspect = 16.0f / 9.0f,
        float near = 0.05f,
        float far = 500.0f,
        float moveSpeed = 5.0f,
        float rotateSpeed = 1.5f
    );

    void update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keyboardState) override;
    Camera& getCamera() override { return _camera; }
    void reset() override;

    void setMoveSpeed(float speed) { _moveSpeed = speed; }
    void setRotateSpeed(float speed) { _rotateSpeed = speed; }
    float getMoveSpeed() const { return _moveSpeed; }
    float getRotateSpeed() const { return _rotateSpeed; }

private:
    Camera _camera;
    glm::vec3 _initialEye;
    glm::vec3 _initialCenter;
    glm::vec3 _initialUp;
    float _moveSpeed;
    float _rotateSpeed;
};

/**
 * FollowCam - Smooth camera that follows a target node
 *
 * Features:
 * - Smooth following with configurable lag (smoothFactor)
 * - Offset from target position
 * - Optional look-at target
 * - Deadzone to prevent jittery movement
 *
 * Example:
 *     auto followCam = std::make_unique<FollowCam>(
 *         playerNode,              // Target to follow
 *         glm::vec3(0, 2, 5),     // Offset behind and above
 *         fov, aspect, near, far,
 *         0.1f                    // Smooth factor (lower = smoother)
 *     );
 *     followCam->update(dt, keyboardState);
 */
class FollowCam : public VirtualCamera {
public:
    FollowCam(
        std::shared_ptr<Node> target,
        glm::vec3 offset = glm::vec3(0.0f, 2.0f, 5.0f),
        float fov = glm::radians(60.0f),
        float aspect = 16.0f / 9.0f,
        float near = 0.05f,
        float far = 500.0f,
        float smoothFactor = 0.1f,
        float deadzone = 0.1f
    );

    void update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keyboardState) override;
    Camera& getCamera() override { return _camera; }
    void reset() override;

    void setTarget(std::shared_ptr<Node> target) { _target = target; }
    void setOffset(const glm::vec3& offset) { _offset = offset; }
    void setSmoothFactor(float factor) { _smoothFactor = factor; }
    void setDeadzone(float deadzone) { _deadzone = deadzone; }

    std::shared_ptr<Node> getTarget() const { return _target; }
    glm::vec3 getOffset() const { return _offset; }
    float getSmoothFactor() const { return _smoothFactor; }
    float getDeadzone() const { return _deadzone; }

private:
    Camera _camera;
    std::shared_ptr<Node> _target;
    glm::vec3 _offset;
    glm::vec3 _currentPosition;
    glm::vec3 _initialOffset;
    float _smoothFactor;
    float _deadzone;
    // Store camera parameters for reconstruction
    float _fov;
    float _aspect;
    float _near;
    float _far;
};

/**
 * CameraManager - Manages multiple cameras and switches between them
 *
 * Supports multiple named cameras and easy switching. Automatically updates
 * the current camera each frame.
 *
 * Example:
 *     CameraManager cameraManager;
 *
 *     // Add cameras
 *     cameraManager.addCamera("fly", std::make_unique<FlyCam>(...));
 *     cameraManager.addCamera("follow", std::make_unique<FollowCam>(...));
 *
 *     // Switch cameras
 *     cameraManager.switchCamera("follow");
 *
 *     // Update current camera
 *     cameraManager.update(dt, keyboardState);
 *
 *     // Get camera for rendering
 *     Camera& camera = cameraManager.getCurrentCamera()->getCamera();
 */
class CameraManager {
public:
    CameraManager() = default;

    /**
     * Add a camera to the manager
     * @param name Unique identifier for the camera
     * @param camera Camera instance to add
     * @throws std::runtime_error if camera with same name already exists
     */
    void addCamera(const std::string& name, std::unique_ptr<VirtualCamera> camera);

    /**
     * Switch to a different camera
     * @param name Name of camera to switch to
     * @throws std::runtime_error if camera name not found
     */
    void switchCamera(const std::string& name);

    /**
     * Get the currently active camera
     * @return Pointer to current ICamera, or nullptr if no cameras exist
     */
    VirtualCamera* getCurrentCamera();

    /**
     * Get a specific camera by name
     * @param name Camera name
     * @return Pointer to ICamera, or nullptr if not found
     */
    VirtualCamera* getCamera(const std::string& name);

    /**
     * Get the name of the currently active camera
     * @return Current camera name, or empty string if no cameras exist
     */
    const std::string& getCurrentCameraName() const { return _currentCameraName; }

    /**
     * Update the current camera
     * @param deltaTime Time elapsed since last frame (seconds)
     * @param keyboardState Current keyboard state map
     */
    void update(float deltaTime, const std::unordered_map<SDL_Scancode, bool>& keyboardState);

    /**
     * Check if a camera with the given name exists
     * @param name Camera name to check
     * @return true if camera exists
     */
    bool hasCamera(const std::string& name) const;

    /**
     * Remove a camera from the manager
     * @param name Camera name to remove
     * @return true if camera was removed, false if not found
     */
    bool removeCamera(const std::string& name);

    /**
     * Reset the current camera to its initial state
     */
    void resetCurrentCamera();

private:
    std::unordered_map<std::string, std::unique_ptr<VirtualCamera>> _cameras;
    std::string _currentCameraName;
};

} // namespace Vapor
