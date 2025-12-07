#pragma once

#include "camera_trauma_components.hpp"
#include "Vapor/components.hpp"
#include <entt/entt.hpp>
#include <algorithm>

// ============================================================
// Camera Trauma System
//
// A "state accumulation" system (vs FSM's "state switching"):
// 1. Consumes CameraTraumaRequest → accumulates trauma values
// 2. Each frame: compute shake offsets from trauma
// 3. Each frame: decay trauma values
// 4. Applies offsets to VirtualCameraComponent
//
// This system does NOT produce Action components - it directly
// modifies camera state each frame (continuous effect).
// ============================================================

class CameraTraumaSystem {
public:
    // Main update - call each frame
    static void update(entt::registry& reg, float deltaTime) {
        // 1. Process all trauma requests
        processRequests(reg);

        // 2. Update all cameras with trauma state
        updateTrauma(reg, deltaTime);
    }

    // Add trauma to a specific camera entity
    static void addTrauma(entt::registry& reg, entt::entity cameraEntity, const CameraTraumaRequest& request) {
        reg.emplace_or_replace<CameraTraumaRequest>(cameraEntity, request);
    }

    // Add trauma to all active cameras
    static void addTraumaToActiveCamera(entt::registry& reg, const CameraTraumaRequest& request) {
        auto view = reg.view<Vapor::VirtualCameraComponent>();
        for (auto entity : view) {
            auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);
            if (cam.isActive) {
                reg.emplace_or_replace<CameraTraumaRequest>(entity, request);
            }
        }
    }

    // Immediately clear all trauma (e.g., when entering cutscene)
    static void clearAllTrauma(entt::registry& reg) {
        auto view = reg.view<CameraTraumaState, Vapor::VirtualCameraComponent>();
        for (auto entity : view) {
            auto& state = view.get<CameraTraumaState>(entity);
            auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);

            // Restore camera position before clearing
            cam.position -= state.previousOffset;

            state.shakeTrauma = 0.0f;
            state.kickTrauma = 0.0f;
            state.rollTrauma = 0.0f;
            state.positionOffset = glm::vec3(0.0f);
            state.previousOffset = glm::vec3(0.0f);
            state.rollOffset = 0.0f;
        }
    }

private:
    static void processRequests(entt::registry& reg) {
        auto view = reg.view<CameraTraumaRequest>();

        for (auto entity : view) {
            auto& request = view.get<CameraTraumaRequest>(entity);

            // Get or create trauma state
            auto& state = reg.get_or_emplace<CameraTraumaState>(entity);

            // Accumulate trauma based on type
            switch (request.type) {
                case TraumaType::Shake:
                    state.shakeTrauma = std::min(1.0f, state.shakeTrauma + request.amount);
                    break;

                case TraumaType::Kick:
                    state.kickTrauma = std::min(1.0f, state.kickTrauma + request.amount);
                    state.kickDirection = glm::normalize(request.direction + glm::vec3(0.001f));
                    break;

                case TraumaType::Roll:
                    state.rollTrauma = std::min(1.0f, state.rollTrauma + request.amount);
                    break;

                case TraumaType::All:
                    state.shakeTrauma = std::min(1.0f, state.shakeTrauma + request.amount);
                    state.rollTrauma = std::min(1.0f, state.rollTrauma + request.amount * 0.5f);
                    break;
            }
        }

        // Clear all processed requests
        reg.clear<CameraTraumaRequest>();
    }

    static void updateTrauma(entt::registry& reg, float deltaTime) {
        auto view = reg.view<CameraTraumaState, Vapor::VirtualCameraComponent>();

        for (auto entity : view) {
            auto& state = view.get<CameraTraumaState>(entity);
            auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);

            // Update noise time
            state.noiseTime += deltaTime * state.frequency;

            // Calculate shake offset (trauma^2 for more natural feel)
            float shakeIntensity = state.shakeTrauma * state.shakeTrauma;
            state.positionOffset = glm::vec3(
                TraumaNoise::fbm(state.noiseTime) * shakeIntensity * state.maxShakeOffset,
                TraumaNoise::fbm(state.noiseTime + 100.0f) * shakeIntensity * state.maxShakeOffset,
                TraumaNoise::fbm(state.noiseTime + 200.0f) * shakeIntensity * state.maxShakeOffset * 0.5f
            );

            // Add kick offset
            float kickIntensity = state.kickTrauma * state.kickTrauma;
            state.positionOffset += state.kickDirection * kickIntensity * state.maxShakeOffset * 2.0f;

            // Calculate roll offset
            float rollIntensity = state.rollTrauma * state.rollTrauma;
            state.rollOffset = TraumaNoise::fbm(state.noiseTime + 300.0f) * rollIntensity * state.maxRollAngle;

            // Apply to camera position
            // First, undo the previous frame's offset, then apply the new offset
            cam.position -= state.previousOffset;
            cam.position += state.positionOffset;
            state.previousOffset = state.positionOffset;

            // Decay trauma
            state.shakeTrauma = std::max(0.0f, state.shakeTrauma - state.decayRate * deltaTime);
            state.kickTrauma = std::max(0.0f, state.kickTrauma - state.kickDecayRate * deltaTime);
            state.rollTrauma = std::max(0.0f, state.rollTrauma - state.decayRate * deltaTime);

            // Clean up if no more trauma
            if (!state.hasTrauma()) {
                state.positionOffset = glm::vec3(0.0f);
                state.previousOffset = glm::vec3(0.0f);
                state.rollOffset = 0.0f;
            }
        }
    }
};

// ============================================================
// Integration helpers
// ============================================================

namespace CameraTraumaHelpers {

    // Call from damage system
    inline void onDamage(entt::registry& reg, float damageAmount, float maxHealth) {
        float percent = damageAmount / maxHealth;
        CameraTraumaSystem::addTraumaToActiveCamera(reg, TraumaPresets::damageTaken(percent));
    }

    // Call from explosion system
    inline void onExplosion(entt::registry& reg, const glm::vec3& explosionPos,
                            const glm::vec3& cameraPos, float explosionRadius) {
        float distance = glm::length(explosionPos - cameraPos);
        if (distance < explosionRadius) {
            float intensity = 1.0f - (distance / explosionRadius);
            CameraTraumaSystem::addTraumaToActiveCamera(reg,
                CameraTraumaRequest::all(intensity * 0.8f));
        }
    }

    // Call from weapon system
    inline void onWeaponFire(entt::registry& reg, const glm::vec3& recoilDirection, float weaponKick) {
        CameraTraumaSystem::addTraumaToActiveCamera(reg,
            TraumaPresets::recoil(recoilDirection, weaponKick));
    }

    // Call from landing system
    inline void onLand(entt::registry& reg, float fallHeight) {
        if (fallHeight > 2.0f) {
            float intensity = std::min((fallHeight - 2.0f) / 10.0f, 0.5f);
            CameraTraumaSystem::addTraumaToActiveCamera(reg,
                CameraTraumaRequest::shake(intensity));
        }
    }

}  // namespace CameraTraumaHelpers
