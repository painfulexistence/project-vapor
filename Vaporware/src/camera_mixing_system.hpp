#pragma once

#include "Vapor/camera.hpp"
#include "Vapor/components.hpp"
#include "camera_trauma_components.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

// ============================================================
// Camera Breath Effect - Subtle idle camera motion
// ============================================================

struct CameraBreathState {
    bool enabled = true;

    // Breath parameters
    float breathRate = 0.3f;           // Breaths per second
    float positionAmplitude = 0.02f;   // Subtle vertical bob
    float rotationAmplitude = 0.005f;  // Subtle pitch/roll

    // Internal state
    float phase = 0.0f;

    // Computed offsets (read by CameraMixingSystem)
    glm::vec3 positionOffset{0.0f};
    float pitchOffset = 0.0f;
    float rollOffset = 0.0f;
};

namespace BreathPresets {
    inline CameraBreathState calm() {
        return CameraBreathState{
            .enabled = true,
            .breathRate = 0.25f,
            .positionAmplitude = 0.015f,
            .rotationAmplitude = 0.003f
        };
    }

    inline CameraBreathState tense() {
        return CameraBreathState{
            .enabled = true,
            .breathRate = 0.5f,
            .positionAmplitude = 0.03f,
            .rotationAmplitude = 0.008f
        };
    }

    inline CameraBreathState exhausted() {
        return CameraBreathState{
            .enabled = true,
            .breathRate = 0.8f,
            .positionAmplitude = 0.05f,
            .rotationAmplitude = 0.015f
        };
    }
}

// ============================================================
// Camera Breath System - Computes breath offset (no side effects)
// ============================================================

class CameraBreathSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        auto view = reg.view<CameraBreathState>();
        for (auto entity : view) {
            auto& state = view.get<CameraBreathState>(entity);

            if (!state.enabled) {
                state.positionOffset = glm::vec3(0.0f);
                state.pitchOffset = 0.0f;
                state.rollOffset = 0.0f;
                continue;
            }

            // Advance phase
            state.phase += deltaTime * state.breathRate * 2.0f * 3.14159f;
            if (state.phase > 2.0f * 3.14159f) {
                state.phase -= 2.0f * 3.14159f;
            }

            // Compute smooth breathing motion
            // Primary breath cycle (slow sine)
            float breathCycle = std::sin(state.phase);
            // Secondary micro-movement (faster, smaller)
            float microCycle = std::sin(state.phase * 3.7f) * 0.3f;

            // Position offset (mainly vertical, slight forward/back)
            state.positionOffset = glm::vec3(
                std::sin(state.phase * 0.7f) * state.positionAmplitude * 0.3f,  // X: slight sway
                breathCycle * state.positionAmplitude,                           // Y: main breath
                microCycle * state.positionAmplitude * 0.2f                      // Z: micro movement
            );

            // Rotation offset (subtle pitch and roll)
            state.pitchOffset = breathCycle * state.rotationAmplitude;
            state.rollOffset = std::sin(state.phase * 0.5f) * state.rotationAmplitude * 0.5f;
        }
    }
};

// ============================================================
// Camera Mixing System - Combines base + all additive effects
// ============================================================

class CameraMixingSystem {
public:
    // Call this after all camera systems have updated
    // Returns the final Camera ready for rendering
    static Camera resolve(entt::registry& reg) {
        Camera finalCamera;

        // Find active virtual camera
        entt::entity activeCamEntity = entt::null;
        auto vcView = reg.view<Vapor::VirtualCameraComponent>();
        for (auto entity : vcView) {
            if (vcView.get<Vapor::VirtualCameraComponent>(entity).isActive) {
                activeCamEntity = entity;
                break;
            }
        }

        if (activeCamEntity == entt::null) {
            return finalCamera;  // No active camera
        }

        auto& baseCam = reg.get<Vapor::VirtualCameraComponent>(activeCamEntity);

        // Start with base values
        glm::vec3 finalPosition = baseCam.position;
        glm::quat finalRotation = baseCam.rotation;
        float finalFov = baseCam.fov;

        // Accumulate additive effects
        glm::vec3 positionOffset{0.0f};
        float pitchOffset = 0.0f;
        float yawOffset = 0.0f;
        float rollOffset = 0.0f;
        float fovOffset = 0.0f;

        // Add trauma offset
        if (auto* trauma = reg.try_get<CameraTraumaState>(activeCamEntity)) {
            positionOffset += trauma->positionOffset;
            rollOffset += trauma->rollOffset;
        }

        // Add breath offset
        if (auto* breath = reg.try_get<CameraBreathState>(activeCamEntity)) {
            positionOffset += breath->positionOffset;
            pitchOffset += breath->pitchOffset;
            rollOffset += breath->rollOffset;
        }

        // Apply position offset
        // Transform offset from camera-local space to world space
        glm::vec3 right = finalRotation * glm::vec3(1, 0, 0);
        glm::vec3 up = finalRotation * glm::vec3(0, 1, 0);
        glm::vec3 forward = finalRotation * glm::vec3(0, 0, -1);

        finalPosition += right * positionOffset.x;
        finalPosition += up * positionOffset.y;
        finalPosition += forward * positionOffset.z;

        // Apply rotation offsets
        glm::quat pitchQuat = glm::angleAxis(pitchOffset, glm::vec3(1, 0, 0));
        glm::quat yawQuat = glm::angleAxis(yawOffset, glm::vec3(0, 1, 0));
        glm::quat rollQuat = glm::angleAxis(rollOffset, glm::vec3(0, 0, 1));
        finalRotation = finalRotation * yawQuat * pitchQuat * rollQuat;

        // Apply FOV offset
        finalFov += fovOffset;

        // Build final camera
        finalCamera.setEye(finalPosition);
        finalCamera.setViewMatrix(baseCam.viewMatrix);
        finalCamera.setProjectionMatrix(baseCam.projectionMatrix);

        // Recompute view matrix with mixed values
        glm::vec3 mixedForward = finalRotation * glm::vec3(0, 0, -1);
        glm::vec3 mixedUp = finalRotation * glm::vec3(0, 1, 0);
        glm::mat4 mixedView = glm::lookAt(finalPosition, finalPosition + mixedForward, mixedUp);
        finalCamera.setViewMatrix(mixedView);

        return finalCamera;
    }

    // Convenience: check if there's an active camera
    static bool hasActiveCamera(entt::registry& reg) {
        auto view = reg.view<Vapor::VirtualCameraComponent>();
        for (auto entity : view) {
            if (view.get<Vapor::VirtualCameraComponent>(entity).isActive) {
                return true;
            }
        }
        return false;
    }

    // Convenience: get active camera entity
    static entt::entity getActiveCameraEntity(entt::registry& reg) {
        auto view = reg.view<Vapor::VirtualCameraComponent>();
        for (auto entity : view) {
            if (view.get<Vapor::VirtualCameraComponent>(entity).isActive) {
                return entity;
            }
        }
        return entt::null;
    }
};
