#pragma once
#include "animation_clip.hpp"
#include <entt/entt.hpp>
#include <functional>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Tween — fluent builder for one-shot property animations
//
// This is what "the ActionManager line became the timeline line" looks like at
// the call site. Where imperative code used to build a shared_ptr<Action> graph
// (UpdateAction lerping a transform, TimelineAction sequencing it), a Tween
// builds a data-driven ActionTimeline and plays it as an overlay through
// TimelineSystem — so the result seeks, reverses, ping-pongs and serializes
// like any other clip, none of which the Action graph could do.
//
//   Vapor::Tween(reg, entity)
//       .moveTo(0.3f, {x, y, 0}, Vapor::EasingType::QuadOut)
//       .then().scaleTo(0.15f, {1.2f, 1.2f, 1}, Vapor::EasingType::BackOut)
//       .with().fadeTo(0.15f, 0.0f)
//       .event("hitFlashDone")           // → the entity's FSMEventQueue
//       .play();                         // → TimelineSystem::addOverlay
//
// Relative ("by") helpers and each segment's start key resolve against the
// entity's CURRENT component state at build time, so no delta stepping happens
// at runtime. Segments are sequential by default; .with() parallels the next
// segment with the previous one, .then() is the explicit sequential marker.
// ─────────────────────────────────────────────────────────────────────────────

namespace Vapor {

    class Tween {
    public:
        Tween(entt::registry& reg, entt::entity e) : _reg(&reg), _e(e) {
        }

        Tween& moveTo(float dur, const glm::vec3& pos, EasingType e = EasingType::Linear);
        Tween& moveBy(float dur, const glm::vec3& delta, EasingType e = EasingType::Linear);
        Tween& rotateTo(float dur, const glm::vec3& rot, EasingType e = EasingType::Linear);// euler radians
        Tween& scaleTo(float dur, const glm::vec3& scale, EasingType e = EasingType::Linear);
        Tween& colorTo(float dur, const glm::vec4& color, EasingType e = EasingType::Linear);
        Tween& fadeTo(float dur, float alpha, EasingType e = EasingType::Linear);
        Tween& customTo(float dur, int customId, float value, EasingType e = EasingType::Linear);

        Tween& then();               // next segment starts at the previous segment's end (default)
        Tween& with();               // next segment parallels the previous segment
        Tween& delay(float seconds); // insert a gap before the next segment
        Tween& event(const std::string& name);// fire a named FSM event at the current cursor
        Tween& name(const std::string& n);

        ActionTimeline build();// finalize the timeline asset

        // Register the timeline as a fire-and-forget overlay on the entity
        // (adds a TimelineOverlayComponent if absent). Returns the overlay id,
        // or 0 if the entity is invalid.
        int play(std::function<void()> onFinished = {});

    private:
        ActionTrack& trackFor(ActionProperty prop, int customId = 0);
        void appendSegment(ActionProperty prop, int customId, const glm::vec4& target, float dur, EasingType e);
        glm::vec4 currentValue(ActionProperty prop, int customId) const;

        entt::registry* _reg = nullptr;
        entt::entity _e = entt::null;
        ActionTimeline _timeline;
        float _cursor = 0.0f;   // start time for the next appended segment
        float _lastStart = 0.0f;
        float _lastEnd = 0.0f;
    };

}// namespace Vapor
