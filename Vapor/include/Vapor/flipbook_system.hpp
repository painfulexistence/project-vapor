#pragma once
#include "animation_library.hpp"
#include "components.hpp"
#include <algorithm>
#include <cmath>
#include <entt/entt.hpp>

// ============================================================
// Flipbook playback — frame-based sprite animation
// ============================================================
//
// The sibling of TimelineSystem for discrete frame animation: clips are shared
// FlipbookClip assets in AnimationClipLibrary, per-entity playback state is the
// PoD FlipbookComponent, and this is the single tick point. Advancing and
// wrapping the playhead is identical to TimelineSystem (same WrapMode set,
// same time-scale groups); the only difference is that sampling maps time to a
// discrete atlas frame index written into Sprite2DComponent::frameIndex, and
// the sprite is only touched when the frame actually changes.

namespace Vapor {

    class FlipbookSystem {
    public:
        static void update(
            entt::registry& reg,
            const AnimationClipLibrary& library,
            float dt,
            const TimelineTimeScales* scales = nullptr
        ) {
            auto view = reg.view<Sprite2DComponent, FlipbookComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& sprite = view.get<Sprite2DComponent>(entity);
                auto& fb = view.get<FlipbookComponent>(entity);
                if (!fb.clip.valid()) continue;
                const FlipbookClip* clip = library.getFlipbook(fb.clip);
                if (!clip || clip->frames.empty()) continue;

                if (fb.playing) advance(fb, *clip, dt, scales);

                int slot = -1;
                const uint16_t frame = clip->sample(fb.time, &slot);
                // Only write when the frame slot changes — flipbooks spend most
                // frames on the same cel, and a redundant write would dirty the
                // sprite every tick.
                if (slot != fb.lastFrameSlot) {
                    sprite.frameIndex = frame;
                    fb.lastFrameSlot = slot;
                }
            }
        }

        // (Re)start playback of a clip handle on this entity; records it in the
        // component's clip list so it stays switchable by name later.
        static void play(entt::registry& reg, entt::entity e, FlipbookClipHandle h) {
            if (!h.valid()) return;
            auto& fb = reg.get_or_emplace<FlipbookComponent>(e);
            if (std::find(fb.clips.begin(), fb.clips.end(), h) == fb.clips.end()) fb.clips.push_back(h);
            fb.clip = h;
            fb.time = 0.0f;
            fb.pingForward = true;
            fb.lastFrameSlot = -1;// force a sprite write on the next update
            fb.playing = true;
        }

        // (Re)start the named clip, resolved among THIS entity's own clips
        // (names collide across entities). False if this entity has no such clip.
        static bool
            play(entt::registry& reg, entt::entity e, const AnimationClipLibrary& library, const std::string& name) {
            auto* fb = reg.try_get<FlipbookComponent>(e);
            if (!fb) return false;
            for (FlipbookClipHandle h : fb->clips) {
                const FlipbookClip* clip = library.getFlipbook(h);
                if (clip && clip->name == name) {
                    play(reg, e, h);
                    return true;
                }
            }
            return false;
        }

        static void stop(entt::registry& reg, entt::entity e) {
            if (auto* fb = reg.try_get<FlipbookComponent>(e)) {
                fb->playing = false;
                fb->time = 0.0f;
                fb->pingForward = true;
                fb->lastFrameSlot = -1;
            }
        }

    private:
        // Playhead advance + wrap, identical in shape to TimelineSystem::update
        // (kept in sync deliberately — one WrapMode contract for the whole
        // animation stack).
        static void
            advance(FlipbookComponent& fb, const FlipbookClip& clip, float dt, const TimelineTimeScales* scales) {
            const float scale = scales ? scales->forGroup(fb.groupId) : 1.0f;
            const float dur = clip.duration;
            if (dur <= 0.0f) return;

            float step = dt * scale * fb.speed;
            if (fb.wrap == WrapMode::PingPong && !fb.pingForward) step = -step;
            fb.time += step;

            switch (fb.wrap) {
            case WrapMode::Once:
            case WrapMode::ClampHold:
                if (fb.time >= dur || fb.time < 0.0f) {
                    fb.time = std::clamp(fb.time, 0.0f, dur);
                    fb.playing = false;
                }
                break;
            case WrapMode::Loop:
                if (fb.time >= dur) {
                    fb.time = std::fmod(fb.time, dur);
                } else if (fb.time < 0.0f) {
                    fb.time = std::fmod(fb.time, dur) + dur;
                    if (fb.time >= dur) fb.time = 0.0f;
                }
                break;
            case WrapMode::PingPong:
                if (fb.time >= dur) {
                    fb.time = std::clamp(dur - (fb.time - dur), 0.0f, dur);
                    fb.pingForward = false;
                } else if (fb.time < 0.0f) {
                    fb.time = std::clamp(-fb.time, 0.0f, dur);
                    fb.pingForward = true;
                }
                break;
            }
        }
    };

}// namespace Vapor
