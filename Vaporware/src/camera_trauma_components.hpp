#pragma once

#include <glm/glm.hpp>
#include <cmath>

// ============================================================
// Camera Trauma System - Screen shake and impact effects
//
// Unlike FSM (state switching), this is a "state accumulation" system:
// - Multiple sources can ADD trauma
// - Trauma naturally DECAYS over time
// - Shake intensity = trauma^2 (more natural feel)
// ============================================================

// Different types of camera trauma effects
enum class TraumaType : uint8_t {
    Shake,      // Random positional offset
    Kick,       // Directional impulse (e.g., recoil)
    Roll,       // Rotational shake
    All         // Combined effect
};

// ============================================================
// Request Component - Add trauma from any source
// ============================================================

struct CameraTraumaRequest {
    float amount = 0.3f;                    // How much trauma to add (0-1)
    TraumaType type = TraumaType::Shake;
    glm::vec3 direction{0.0f};              // For Kick type: impulse direction

    // Convenience constructors
    static CameraTraumaRequest shake(float amount) {
        return { amount, TraumaType::Shake, glm::vec3(0.0f) };
    }

    static CameraTraumaRequest kick(float amount, const glm::vec3& dir) {
        return { amount, TraumaType::Kick, dir };
    }

    static CameraTraumaRequest roll(float amount) {
        return { amount, TraumaType::Roll, glm::vec3(0.0f) };
    }

    static CameraTraumaRequest all(float amount) {
        return { amount, TraumaType::All, glm::vec3(0.0f) };
    }
};

// ============================================================
// State Component - Tracks current trauma and computed offsets
// ============================================================

struct CameraTraumaState {
    // Trauma values (0-1, naturally decay)
    float shakeTrauma = 0.0f;
    float kickTrauma = 0.0f;
    float rollTrauma = 0.0f;

    // Configuration
    float maxShakeOffset = 0.5f;        // Max positional offset in units
    float maxRollAngle = 0.1f;          // Max roll in radians
    float decayRate = 1.5f;             // Trauma decay per second
    float frequency = 15.0f;            // Noise frequency (higher = faster shake)

    // Kick-specific
    glm::vec3 kickDirection{0.0f};
    float kickDecayRate = 5.0f;         // Kick decays faster

    // Computed results (applied to camera each frame)
    glm::vec3 positionOffset{0.0f};
    glm::vec3 previousOffset{0.0f};     // Previous frame offset (for correction)
    float rollOffset = 0.0f;

    // Internal time accumulator for noise
    float noiseTime = 0.0f;

    // Helper to get total trauma (for UI display, etc.)
    float getTotalTrauma() const {
        return std::max({ shakeTrauma, kickTrauma, rollTrauma });
    }

    bool hasTrauma() const {
        return shakeTrauma > 0.001f || kickTrauma > 0.001f || rollTrauma > 0.001f;
    }
};

// ============================================================
// Simple noise functions for natural-feeling shake
// ============================================================

namespace TraumaNoise {

    // Simple hash-based pseudo-random
    inline float hash(float n) {
        return glm::fract(std::sin(n) * 43758.5453123f);
    }

    // Smooth noise (value noise)
    inline float noise(float x) {
        float i = std::floor(x);
        float f = glm::fract(x);
        // Smoothstep interpolation
        float u = f * f * (3.0f - 2.0f * f);
        return glm::mix(hash(i), hash(i + 1.0f), u) * 2.0f - 1.0f;
    }

    // 2D noise for more variation
    inline float noise2D(float x, float y) {
        return noise(x + y * 57.0f);
    }

    // Perlin-like noise with multiple octaves
    inline float fbm(float x, int octaves = 2) {
        float value = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;

        for (int i = 0; i < octaves; i++) {
            value += amplitude * noise(x * frequency);
            maxValue += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }

        return value / maxValue;
    }

}  // namespace TraumaNoise

// ============================================================
// Presets for common trauma scenarios
// ============================================================

namespace TraumaPresets {

    // Light impact (footstep, small bump)
    inline CameraTraumaRequest lightImpact() {
        return CameraTraumaRequest::shake(0.1f);
    }

    // Medium impact (landing, hit)
    inline CameraTraumaRequest mediumImpact() {
        return CameraTraumaRequest::shake(0.3f);
    }

    // Heavy impact (explosion nearby)
    inline CameraTraumaRequest heavyImpact() {
        return CameraTraumaRequest::all(0.6f);
    }

    // Massive impact (big explosion, boss attack)
    inline CameraTraumaRequest massiveImpact() {
        return CameraTraumaRequest::all(1.0f);
    }

    // Weapon recoil
    inline CameraTraumaRequest recoil(const glm::vec3& direction, float intensity = 0.2f) {
        return CameraTraumaRequest::kick(intensity, direction);
    }

    // Damage taken
    inline CameraTraumaRequest damageTaken(float damagePercent) {
        // Scale trauma based on damage amount
        float amount = glm::clamp(damagePercent * 0.5f, 0.1f, 0.8f);
        return CameraTraumaRequest::shake(amount);
    }

}  // namespace TraumaPresets
