#pragma once
#include "easing.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Animation clips (shared, data-only assets)
//
// The passive data the timeline/skeletal players sample. Clips carry no
// playback state (no playhead, no speed) — one clip instance can back any
// number of simultaneous players; playback state lives in per-entity
// components (TimelinePlaybackComponent) and the clip is a pure function of
// time. Clips live in AnimationClipLibrary and are referred to by lightweight
// handles so they can be shared and hot-swapped.
//
// The data model is shared vocabulary with the Atmospheric engine
// (animation_clip.hpp there): same track/key/wrap semantics, same CSB-
// compatible easing values. Handle spelling follows THIS repo's house style
// (Uint32 rid, UINT32_MAX sentinel — PhysicsHandle/GPUHandle).
// ─────────────────────────────────────────────────────────────────────────────

namespace Vapor {

    // How a player treats the playhead at the clip's ends.
    enum class WrapMode {
        Once,     // play to the end, stop
        Loop,     // wrap to the start each cycle
        PingPong, // reverse direction at each end
        ClampHold,// like Once, but caller intent is "rest on the final frame"
    };

    // ── Handles ──────────────────────────────────────────────────────────────
    // Distinct tag types so a skeleton-clip handle can't be passed where a
    // timeline handle is expected.
    template<typename Tag> struct ClipHandle {
        Uint32 rid = UINT32_MAX;

        bool valid() const {
            return rid != UINT32_MAX;
        }
        bool operator==(const ClipHandle&) const = default;
    };

    struct TimelineTag {};
    struct FlipbookTag {};
    struct SkeletonClipTag {};
    struct SkeletonTag {};
    using TimelineHandle = ClipHandle<TimelineTag>;
    using FlipbookClipHandle = ClipHandle<FlipbookTag>;
    using SkeletonClipHandle = ClipHandle<SkeletonClipTag>;
    using SkeletonHandle = ClipHandle<SkeletonTag>;

    // ── Flipbook (frame-based sprite animation) ──────────────────────────────
    // Shared, data-only frame strips sampled by FlipbookComponent. Vapor sprites
    // address an atlas by frame INDEX (unlike Atmospheric, whose flipbook stores
    // UV rects), so a frame here is an atlas index + how long it shows. Same
    // semantics as Atmospheric's FlipbookClip otherwise — per-frame duration and
    // an optional frame event — and the same from-grid builders, adapted to
    // index addressing.
    struct FlipbookFrame {
        uint16_t frameIndex = 0;// index into the sprite's AtlasHandle
        float duration = 0.1f;  // seconds this frame is shown
        int eventId = -1;       // >= 0 → reserved frame-event id (no engine consumer yet)
    };

    struct FlipbookClip {
        std::string name;
        std::vector<FlipbookFrame> frames;
        float duration = 0.0f;// sum of frame durations; cached by recompute()

        void recompute();

        // Sample the clip at t (seconds): returns the frame index active at that
        // time. Clamps to the first/last frame outside [0, duration]. Returns 0
        // for an empty clip. `outFrameSlot` receives the frame's position in the
        // strip (for change detection / event firing), or -1 if empty.
        uint16_t sample(float t, int* outFrameSlot = nullptr) const;

        // Build a strip from explicit atlas indices, each shown for the same
        // duration. The addressing the old FlipbookComponent::frameIndices used.
        static FlipbookClip fromIndices(std::string name, std::vector<uint16_t> frameIndices, float frameDuration);
        // Build a strip from `count` consecutive atlas indices starting at
        // `firstIndex` (the common "frames 4..11 are the walk cycle" case).
        static FlipbookClip fromRange(std::string name, uint16_t firstIndex, uint16_t count, float frameDuration);
    };

    // ── Action timeline (property tracks as keyframes) ───────────────────────
    enum class ActionProperty {
        Position,    // vec3 local position
        Rotation,    // vec3 euler radians
        Scale,       // vec3
        Color,       // vec4 tint (sprite)
        Alpha,       // float in .x → color.a only
        Custom,      // float in .x, routed by customId (no engine consumer yet)
        RotationQuat,// quaternion (x,y,z,w) — slerp'd; glTF/USD node rotation
    };

    struct ActionKey {
        float time = 0.0f;                     // seconds from timeline start
        glm::vec4 value{ 0.0f };               // interpreted per ActionProperty (scalars in .x)
        EasingType easing = EasingType::Linear;// eases the segment ending at this key
    };

    struct ActionTrack {
        ActionProperty property = ActionProperty::Position;
        int customId = 0;// routes ActionProperty::Custom tracks
        std::vector<ActionKey> keys;// kept sorted by time

        // Sample the track at t (seconds). Binary-searches the surrounding keys
        // and returns the eased lerp (slerp for RotationQuat); clamps to the
        // first/last key outside the range. Zero vector for an empty track.
        glm::vec4 sample(float t) const;
    };

    // A named marker on the timeline. When playback crosses it, TimelineSystem
    // pushes `name` into the entity's FSMEventQueue (if any) — the seam where
    // data-driven animation feeds the FSM orchestration layer.
    struct TimelineEvent {
        float time = 0.0f;
        int eventId = 0;
        std::string name;
    };

    struct ActionTimeline {
        std::string name;
        float duration = 0.0f;// max key/event time; cached by recompute()
        std::vector<ActionTrack> tracks;
        std::vector<TimelineEvent> events;// sorted by time

        // Recompute `duration` from the tracks/events (call after building).
        void recompute();
    };

    // ── Skeleton clip (skeletal animation) ───────────────────────────────────
    // Per-joint local TRS keyframe channels, indexing into a Skeleton by joint
    // index. Imported from glTF skins / USD UsdSkel and stored in the library;
    // data-complete today, consumer (a skinning path) is future work.
    // Translation/scale interpolate linearly, rotation slerps (glTF LINEAR).
    // Empty channels fall back to the joint's bind pose.
    struct Vec3Key {
        float time = 0.0f;
        glm::vec3 value{ 0.0f };
    };
    struct QuatKey {
        float time = 0.0f;
        glm::quat value{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    struct JointChannel {
        int joint = 0;// index into Skeleton::joints
        std::vector<Vec3Key> translation;
        std::vector<QuatKey> rotation;
        std::vector<Vec3Key> scale;
    };

    struct SkeletonClip {
        std::string name;
        float duration = 0.0f;// max key time across all channels; cached by recompute()
        std::vector<JointChannel> channels;

        void recompute();
    };

    // Keyframe samplers (binary search + interpolation; clamp outside the
    // range). Empty tracks return the provided fallback (the joint's bind value).
    glm::vec3 sampleVec3Track(const std::vector<Vec3Key>& keys, float t, const glm::vec3& fallback);
    glm::quat sampleQuatTrack(const std::vector<QuatKey>& keys, float t, const glm::quat& fallback);

}// namespace Vapor
