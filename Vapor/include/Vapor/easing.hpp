#pragma once

namespace Vapor {

    // ─────────────────────────────────────────────────────────────────────────
    // Easing functions
    //
    // Shared by the whole animation stack (timeline tracks and anything else
    // that interpolates over a normalized [0,1] parameter). The enum VALUES are
    // the shared vocabulary with the Atmospheric engine (and, through it, the
    // Cocos Studio CSB format) — do not renumber. An enum, unlike the
    // std::function easings in action_manager.hpp, is PoD: it can live in
    // blueprint-authored components and serialize through the scene cook.
    // ─────────────────────────────────────────────────────────────────────────

    enum class EasingType {
        Linear = 0,
        SineIn = 1,
        SineOut = 2,
        SineInOut = 3,
        QuadIn = 4,
        QuadOut = 5,
        QuadInOut = 6,
        CubicIn = 7,
        CubicOut = 8,
        CubicInOut = 9,
        QuartIn = 10,
        QuartOut = 11,
        QuartInOut = 12,
        QuintIn = 13,
        QuintOut = 14,
        QuintInOut = 15,
        ExpoIn = 16,
        ExpoOut = 17,
        ExpoInOut = 18,
        CircIn = 19,
        CircOut = 20,
        CircInOut = 21,
        BackIn = 22,
        BackOut = 23,
        BackInOut = 24,
        ElasticIn = 25,
        ElasticOut = 26,
        ElasticInOut = 27,
        BounceIn = 28,
        BounceOut = 29,
        BounceInOut = 30,
    };

    // Remap t (expected in [0, 1]) through the named easing curve. Values
    // outside [0, 1] are passed through the same formulas (callers clamp
    // beforehand where that matters).
    float applyEasing(float t, EasingType type);

}// namespace Vapor
