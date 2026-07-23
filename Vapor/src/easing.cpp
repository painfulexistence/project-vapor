#include "Vapor/easing.hpp"
#include <cmath>

// Easing curve implementations, shared by the whole animation stack. Ported
// from Atmospheric's easing.cpp so the two engines sample identical curves for
// identical enum values (the CSB-compatible numbering).

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Vapor {

namespace {

    float easeInSine(float t) {
        return 1.0f - std::cos((t * M_PI) / 2.0f);
    }
    float easeOutSine(float t) {
        return std::sin((t * M_PI) / 2.0f);
    }
    float easeInOutSine(float t) {
        return -(std::cos(M_PI * t) - 1.0f) / 2.0f;
    }

    float easeInQuad(float t) {
        return t * t;
    }
    float easeOutQuad(float t) {
        return 1.0f - (1.0f - t) * (1.0f - t);
    }
    float easeInOutQuad(float t) {
        return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
    }

    float easeInCubic(float t) {
        return t * t * t;
    }
    float easeOutCubic(float t) {
        return 1.0f - std::pow(1.0f - t, 3.0f);
    }
    float easeInOutCubic(float t) {
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
    }

    float easeInQuart(float t) {
        return t * t * t * t;
    }
    float easeOutQuart(float t) {
        return 1.0f - std::pow(1.0f - t, 4.0f);
    }
    float easeInOutQuart(float t) {
        return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) / 2.0f;
    }

    float easeInQuint(float t) {
        return t * t * t * t * t;
    }
    float easeOutQuint(float t) {
        return 1.0f - std::pow(1.0f - t, 5.0f);
    }
    float easeInOutQuint(float t) {
        return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 5.0f) / 2.0f;
    }

    float easeInExpo(float t) {
        return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
    }
    float easeOutExpo(float t) {
        return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
    }
    float easeInOutExpo(float t) {
        if (t == 0.0f) return 0.0f;
        if (t == 1.0f) return 1.0f;
        return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
                        : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) / 2.0f;
    }

    float easeInCirc(float t) {
        return 1.0f - std::sqrt(1.0f - t * t);
    }
    float easeOutCirc(float t) {
        return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f));
    }
    float easeInOutCirc(float t) {
        return t < 0.5f ? (1.0f - std::sqrt(1.0f - 4.0f * t * t)) / 2.0f
                        : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
    }

    float easeInBack(float t) {
        const float c1 = 1.70158f;
        return (c1 + 1.0f) * t * t * t - c1 * t * t;
    }
    float easeOutBack(float t) {
        const float c1 = 1.70158f;
        return 1.0f + (c1 + 1.0f) * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
    }
    float easeInOutBack(float t) {
        const float c2 = 1.70158f * 1.525f;
        return t < 0.5f ? (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f
                        : (std::pow(2.0f * t - 2.0f, 2.0f) * ((c2 + 1.0f) * (t * 2.0f - 2.0f) + c2) + 2.0f) / 2.0f;
    }

    // NOTE: the upstream one-liner form `n1 * (t -= x) * t` reads and writes t
    // unsequenced (UB); the statements below are the same math, well-defined.
    float easeOutBounce(float t) {
        const float n1 = 7.5625f, d1 = 2.75f;
        if (t < 1.0f / d1) return n1 * t * t;
        if (t < 2.0f / d1) {
            t -= 1.5f / d1;
            return n1 * t * t + 0.75f;
        }
        if (t < 2.5f / d1) {
            t -= 2.25f / d1;
            return n1 * t * t + 0.9375f;
        }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
    float easeInBounce(float t) {
        return 1.0f - easeOutBounce(1.0f - t);
    }
    float easeInOutBounce(float t) {
        return t < 0.5f ? (1.0f - easeOutBounce(1.0f - 2.0f * t)) / 2.0f
                        : (1.0f + easeOutBounce(2.0f * t - 1.0f)) / 2.0f;
    }

    float easeInElastic(float t) {
        if (t == 0.0f || t == 1.0f) return t;
        const float c4 = (2.0f * M_PI) / 3.0f;
        return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
    }
    float easeOutElastic(float t) {
        if (t == 0.0f || t == 1.0f) return t;
        const float c4 = (2.0f * M_PI) / 3.0f;
        return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
    }
    float easeInOutElastic(float t) {
        if (t == 0.0f || t == 1.0f) return t;
        const float c5 = (2.0f * M_PI) / 4.5f;
        return t < 0.5f ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f
                        : (std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f + 1.0f;
    }

}// namespace

float applyEasing(float t, EasingType type) {
    switch (type) {
    case EasingType::Linear:
        return t;
    case EasingType::SineIn:
        return easeInSine(t);
    case EasingType::SineOut:
        return easeOutSine(t);
    case EasingType::SineInOut:
        return easeInOutSine(t);
    case EasingType::QuadIn:
        return easeInQuad(t);
    case EasingType::QuadOut:
        return easeOutQuad(t);
    case EasingType::QuadInOut:
        return easeInOutQuad(t);
    case EasingType::CubicIn:
        return easeInCubic(t);
    case EasingType::CubicOut:
        return easeOutCubic(t);
    case EasingType::CubicInOut:
        return easeInOutCubic(t);
    case EasingType::QuartIn:
        return easeInQuart(t);
    case EasingType::QuartOut:
        return easeOutQuart(t);
    case EasingType::QuartInOut:
        return easeInOutQuart(t);
    case EasingType::QuintIn:
        return easeInQuint(t);
    case EasingType::QuintOut:
        return easeOutQuint(t);
    case EasingType::QuintInOut:
        return easeInOutQuint(t);
    case EasingType::ExpoIn:
        return easeInExpo(t);
    case EasingType::ExpoOut:
        return easeOutExpo(t);
    case EasingType::ExpoInOut:
        return easeInOutExpo(t);
    case EasingType::CircIn:
        return easeInCirc(t);
    case EasingType::CircOut:
        return easeOutCirc(t);
    case EasingType::CircInOut:
        return easeInOutCirc(t);
    case EasingType::BackIn:
        return easeInBack(t);
    case EasingType::BackOut:
        return easeOutBack(t);
    case EasingType::BackInOut:
        return easeInOutBack(t);
    case EasingType::ElasticIn:
        return easeInElastic(t);
    case EasingType::ElasticOut:
        return easeOutElastic(t);
    case EasingType::ElasticInOut:
        return easeInOutElastic(t);
    case EasingType::BounceIn:
        return easeInBounce(t);
    case EasingType::BounceOut:
        return easeOutBounce(t);
    case EasingType::BounceInOut:
        return easeInOutBounce(t);
    default:
        return t;
    }
}

}// namespace Vapor
