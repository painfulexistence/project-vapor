#pragma once
#include "animation_library.hpp"
#include "components.hpp"
#include "fsm.hpp"
#include "hidden.hpp"
#include <algorithm>
#include <cmath>
#include <entt/entt.hpp>
#include <functional>
#include <glm/gtc/quaternion.hpp>
#include <vector>

// ============================================================
// Timeline playback — the data-driven animation path
// ============================================================
//
// The Atmospheric-style split, translated into entt idioms:
//   - clips are shared assets in AnimationClipLibrary (EngineCore-owned),
//   - per-entity playback state is a PoD component below,
//   - TimelineSystem::update is the single tick point (one call per frame,
//     like PageSystem), sampling every playing timeline as a pure function of
//     its playhead — which is what makes seek, reverse, ping-pong and
//     scrubbing all work uniformly.
//
// This is the layer UNDER the FSM orchestration systems, not a replacement
// for them: timeline events feed FSMEventQueue, and FSM actions start/stop
// timelines. ActionManager remains the imperative escape hatch for
// code-driven one-shot sequencing.

namespace Vapor {

    // Playback state for one entity's timeline. Pure data: the clip is named
    // by handle, easing/wrap are enums, and the only non-authorable fields are
    // Hidden<> playhead internals — so the component stays blueprint- and
    // inspector-friendly.
    struct TimelinePlaybackComponent {
        TimelineHandle clip;              // active clip (invalid = nothing to play)
        std::vector<TimelineHandle> clips;// this entity's available clips (switch by assigning `clip`)
        float time = 0.0f;                // seconds into the clip
        float speed = 1.0f;               // negative plays backwards
        WrapMode wrap = WrapMode::Loop;
        bool playing = false;
        Uint32 groupId = 0;               // TimelineTimeScales group

        Hidden<float> prevTime = { 0.0f };  // last evaluated playhead (event crossing)
        Hidden<bool> pingForward = { true };// PingPong direction
    };

    // One fire-and-forget overlay timeline layered on top of the main playback.
    // Owns its ActionTimeline (usually anonymous, built by a Tween) and its own
    // playhead; plays once and retires, firing onFinished.
    struct TimelineOverlay {
        int id = 0;
        ActionTimeline timeline;// owned copy
        float time = 0.0f;
        float prevTime = 0.0f;
        std::function<void()> onFinished;
    };

    // Behavior-tier companion to TimelinePlaybackComponent (deliberately NOT
    // PoD — overlays own timelines and callbacks). This is where imperative,
    // code-driven tweens live: the Tween builder (tween.hpp) constructs an
    // ActionTimeline and drops it here as an overlay, so the ergonomics that
    // used to reach for ActionManager become data-driven clips that seek and
    // reverse like any other. Layered on top of the main timeline: overlay
    // tracks apply after the main clip each frame.
    struct TimelineOverlayComponent {
        std::vector<TimelineOverlay> overlays;
        int nextId = 1;
        Uint32 groupId = 0;// TimelineTimeScales group (same groups as playback)
    };

    class TimelineSystem {
    public:
        // Advance and evaluate every playing timeline. `scales` may be null
        // (all groups run at 1x).
        static void update(
            entt::registry& reg,
            const AnimationClipLibrary& library,
            float dt,
            const TimelineTimeScales* scales = nullptr
        ) {
            auto view = reg.view<TimelinePlaybackComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& play = view.get<TimelinePlaybackComponent>(entity);
                if (!play.playing || !play.clip.valid()) continue;
                const ActionTimeline* tl = library.getTimeline(play.clip);
                if (!tl) continue;

                const float scale = scales ? scales->forGroup(play.groupId) : 1.0f;
                const float scaledDt = dt * scale;
                const float dur = tl->duration;

                // Untimed clip: accumulate and sample, no wrapping.
                if (dur <= 0.0f) {
                    play.time += scaledDt * play.speed;
                    evaluate(reg, entity, play, *tl);
                    continue;
                }

                float step = scaledDt * play.speed;
                if (play.wrap == WrapMode::PingPong && !play.pingForward) step = -step;
                play.time += step;

                switch (play.wrap) {
                case WrapMode::Once:
                case WrapMode::ClampHold:
                    // Both play once and hold the final frame — the end pose is
                    // the last evaluate before we stop ticking.
                    if (play.time >= dur || play.time < 0.0f) {
                        play.time = std::clamp(play.time, 0.0f, dur);
                        play.playing = false;
                    }
                    break;

                case WrapMode::Loop:
                    if (play.time >= dur) {
                        play.time = std::fmod(play.time, dur);
                    } else if (play.time < 0.0f) {
                        play.time = std::fmod(play.time, dur) + dur;
                        if (play.time >= dur) play.time = 0.0f;
                    }
                    break;

                case WrapMode::PingPong:
                    if (play.time >= dur) {
                        play.time = std::clamp(dur - (play.time - dur), 0.0f, dur);// reflect past the end
                        play.pingForward = false;
                    } else if (play.time < 0.0f) {
                        play.time = std::clamp(-play.time, 0.0f, dur);// reflect past the start
                        play.pingForward = true;
                    }
                    break;
                }

                evaluate(reg, entity, play, *tl);
            }

            // Overlays run after all main playback, so an entity's overlay
            // tracks layer on top of whatever its main clip set this frame.
            updateOverlays(reg, dt, scales);
        }

        // Add a fire-and-forget overlay timeline to this entity (creating the
        // overlay component if absent). Returns an id usable with cancelOverlay.
        // The Tween builder is the ergonomic front door; this is the raw one.
        static int addOverlay(
            entt::registry& reg, entt::entity e, ActionTimeline tl, std::function<void()> onFinished = {}
        ) {
            tl.recompute();
            auto& oc = reg.get_or_emplace<TimelineOverlayComponent>(e);
            TimelineOverlay ov;
            ov.id = oc.nextId++;
            ov.timeline = std::move(tl);
            ov.onFinished = std::move(onFinished);
            oc.overlays.push_back(std::move(ov));
            return oc.overlays.back().id;
        }

        static void cancelOverlay(entt::registry& reg, entt::entity e, int id) {
            if (auto* oc = reg.try_get<TimelineOverlayComponent>(e)) {
                oc->overlays.erase(
                    std::remove_if(
                        oc->overlays.begin(), oc->overlays.end(), [id](const TimelineOverlay& o) { return o.id == id; }
                    ),
                    oc->overlays.end()
                );
            }
        }

        static void cancelAllOverlays(entt::registry& reg, entt::entity e) {
            if (auto* oc = reg.try_get<TimelineOverlayComponent>(e)) oc->overlays.clear();
        }

        // (Re)start playback of a handle on this entity. Also records it in the
        // component's clip list so it stays switchable by name later.
        static void play(entt::registry& reg, entt::entity e, TimelineHandle h) {
            if (!h.valid()) return;
            auto& play = reg.get_or_emplace<TimelinePlaybackComponent>(e);
            if (std::find(play.clips.begin(), play.clips.end(), h) == play.clips.end()) play.clips.push_back(h);
            play.clip = h;
            play.time = 0.0f;
            play.prevTime = 0.0f;
            play.pingForward = true;
            play.playing = true;
        }

        // (Re)start the named timeline, resolved among THIS entity's own clips.
        // Timeline names collide across entities (every imported node's clip
        // may share a name), so a global by-name lookup would grab another
        // entity's clip. Returns false if this entity has no clip of that name.
        static bool play(
            entt::registry& reg, entt::entity e, const AnimationClipLibrary& library, const std::string& name
        ) {
            auto* play = reg.try_get<TimelinePlaybackComponent>(e);
            if (!play) return false;
            for (TimelineHandle h : play->clips) {
                const ActionTimeline* tl = library.getTimeline(h);
                if (tl && tl->name == name) {
                    TimelineSystem::play(reg, e, h);
                    return true;
                }
            }
            return false;
        }

        static void stop(entt::registry& reg, entt::entity e) {
            if (auto* play = reg.try_get<TimelinePlaybackComponent>(e)) {
                play->playing = false;
                play->time = 0.0f;
                play->prevTime = 0.0f;
                play->pingForward = true;
            }
        }

        // Set the playhead and re-sample immediately (works while paused).
        static void seek(
            entt::registry& reg, entt::entity e, const AnimationClipLibrary& library, float seconds
        ) {
            auto* play = reg.try_get<TimelinePlaybackComponent>(e);
            if (!play || !play->clip.valid()) return;
            const ActionTimeline* tl = library.getTimeline(play->clip);
            if (!tl) return;
            play->time = tl->duration > 0.0f ? std::clamp(seconds, 0.0f, tl->duration) : seconds;
            evaluate(reg, e, *play, *tl);
        }

    private:
        static void evaluate(
            entt::registry& reg, entt::entity e, TimelinePlaybackComponent& play, const ActionTimeline& tl
        ) {
            applyTracks(reg, e, tl, play.time);
            fireEvents(reg, e, tl, play.prevTime, play.time);
            play.prevTime = play.time;
        }

        static void updateOverlays(entt::registry& reg, float dt, const TimelineTimeScales* scales) {
            auto view = reg.view<TimelineOverlayComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& oc = view.get<TimelineOverlayComponent>(entity);
                if (oc.overlays.empty()) continue;
                const float scaledDt = dt * (scales ? scales->forGroup(oc.groupId) : 1.0f);

                for (auto& ov : oc.overlays) {
                    ov.prevTime = ov.time;
                    ov.time += scaledDt;
                    applyTracks(reg, entity, ov.timeline, ov.time);
                    fireEvents(reg, entity, ov.timeline, ov.prevTime, ov.time);
                }

                // Retire finished overlays (they play once). Collect callbacks
                // and fire them AFTER erasing, so an onFinished that starts a new
                // overlay can't invalidate the vector mid-iteration.
                std::vector<std::function<void()>> finished;
                oc.overlays.erase(
                    std::remove_if(
                        oc.overlays.begin(), oc.overlays.end(),
                        [&](TimelineOverlay& ov) {
                            if (ov.time >= ov.timeline.duration) {
                                if (ov.onFinished) finished.push_back(std::move(ov.onFinished));
                                return true;
                            }
                            return false;
                        }
                    ),
                    oc.overlays.end()
                );
                for (auto& cb : finished)
                    cb();
            }
        }

        static void applyTracks(entt::registry& reg, entt::entity e, const ActionTimeline& tl, float time) {
            auto* tc = reg.try_get<TransformComponent>(e);
            for (const auto& track : tl.tracks) {
                const glm::vec4 v = track.sample(time);
                switch (track.property) {
                case ActionProperty::Position:
                    if (tc) {
                        tc->position = glm::vec3(v.x, v.y, v.z);
                        tc->isDirty = true;
                    }
                    break;
                case ActionProperty::Rotation:
                    if (tc) {
                        tc->rotation = glm::quat(glm::vec3(v.x, v.y, v.z));
                        tc->isDirty = true;
                    }
                    break;
                case ActionProperty::RotationQuat:
                    if (tc) {
                        // Interpolation already happened in quaternion space
                        // (track.sample slerps); v is packed (x,y,z,w).
                        tc->rotation = glm::quat(v.w, v.x, v.y, v.z);
                        tc->isDirty = true;
                    }
                    break;
                case ActionProperty::Scale:
                    if (tc) {
                        tc->scale = glm::vec3(v.x, v.y, v.z);
                        tc->isDirty = true;
                    }
                    break;
                case ActionProperty::Color:
                    setTint(reg, e, v);
                    break;
                case ActionProperty::Alpha: {
                    glm::vec4 c(1.0f);
                    getTint(reg, e, c);
                    c.a = v.x;
                    setTint(reg, e, c);
                    break;
                }
                case ActionProperty::Custom:
                    // No engine-side consumer yet; game systems can sample the
                    // clip themselves for custom tracks.
                    break;
                }
            }
        }

        // Fire events crossed moving from `from` to `to` into the entity's
        // FSMEventQueue (the seam between animation and orchestration).
        static void fireEvents(entt::registry& reg, entt::entity e, const ActionTimeline& tl, float from, float to) {
            if (tl.events.empty()) return;
            auto* queue = reg.try_get<FSMEventQueue>(e);
            if (!queue) return;
            auto fireRange = [&](float lo, float hi) {
                for (const auto& ev : tl.events)
                    if (ev.time > lo && ev.time <= hi && !ev.name.empty()) queue->push(ev.name);
            };
            if (to >= from) {
                fireRange(from, to);
            } else {
                // wrapped: (from, duration] then (start, to]
                fireRange(from, tl.duration);
                fireRange(-1.0f, to);
            }
        }

        // Colour get/set across the drawable component types.
        static bool getTint(entt::registry& reg, entt::entity e, glm::vec4& out) {
            if (auto* s = reg.try_get<Sprite2DComponent>(e)) {
                out = s->tint;
                return true;
            }
            if (auto* s = reg.try_get<Sprite3DComponent>(e)) {
                out = s->tint;
                return true;
            }
            return false;
        }
        static void setTint(entt::registry& reg, entt::entity e, const glm::vec4& c) {
            if (auto* s = reg.try_get<Sprite2DComponent>(e)) {
                s->tint = c;
                return;
            }
            if (auto* s = reg.try_get<Sprite3DComponent>(e)) {
                s->tint = c;
                return;
            }
        }
    };

}// namespace Vapor
