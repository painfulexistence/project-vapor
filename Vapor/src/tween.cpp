#include "Vapor/tween.hpp"
#include "Vapor/components.hpp"
#include "Vapor/timeline_system.hpp"
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

namespace Vapor {

// Read the entity's current value for a property, so a segment's start key and
// the "by" helpers resolve against live state at build time. Mirrors
// TimelineSystem's apply targets (TransformComponent for TRS, sprite tint for
// color/alpha).
glm::vec4 Tween::currentValue(ActionProperty prop, int) const {
    switch (prop) {
    case ActionProperty::Position:
        if (auto* tc = _reg->try_get<TransformComponent>(_e)) {
            const glm::vec3 p = tc->position;
            return glm::vec4(p.x, p.y, p.z, 0.0f);
        }
        return glm::vec4(0.0f);
    case ActionProperty::Rotation:
        if (auto* tc = _reg->try_get<TransformComponent>(_e)) {
            const glm::vec3 e = glm::eulerAngles(tc->rotation);
            return glm::vec4(e.x, e.y, e.z, 0.0f);
        }
        return glm::vec4(0.0f);
    case ActionProperty::Scale:
        if (auto* tc = _reg->try_get<TransformComponent>(_e)) {
            const glm::vec3 s = tc->scale;
            return glm::vec4(s.x, s.y, s.z, 0.0f);
        }
        return glm::vec4(1.0f);
    case ActionProperty::Color:
        if (auto* s = _reg->try_get<Sprite2DComponent>(_e)) return s->tint;
        if (auto* s = _reg->try_get<Sprite3DComponent>(_e)) return s->tint;
        return glm::vec4(1.0f);
    case ActionProperty::Alpha: {
        float a = 1.0f;
        if (auto* s = _reg->try_get<Sprite2DComponent>(_e))
            a = s->tint.a;
        else if (auto* s = _reg->try_get<Sprite3DComponent>(_e))
            a = s->tint.a;
        return glm::vec4(a, 0.0f, 0.0f, 0.0f);
    }
    case ActionProperty::RotationQuat:
    case ActionProperty::Custom:
    default:
        return glm::vec4(0.0f);
    }
}

ActionTrack& Tween::trackFor(ActionProperty prop, int customId) {
    for (auto& t : _timeline.tracks)
        if (t.property == prop && t.customId == customId) return t;
    _timeline.tracks.push_back(ActionTrack{ prop, customId, {} });
    return _timeline.tracks.back();
}

void Tween::appendSegment(ActionProperty prop, int customId, const glm::vec4& target, float dur, EasingType e) {
    ActionTrack& track = trackFor(prop, customId);
    const float start = _cursor;
    const float end = _cursor + dur;
    // Seed a start key at the build-time value the first time this track is
    // touched, so playback begins from wherever the entity currently is.
    if (track.keys.empty()) track.keys.push_back(ActionKey{ start, currentValue(prop, customId), EasingType::Linear });
    track.keys.push_back(ActionKey{ end, target, e });
    _lastStart = start;
    _lastEnd = end;
    _cursor = end;// sequential by default
}

Tween& Tween::moveTo(float dur, const glm::vec3& pos, EasingType e) {
    appendSegment(ActionProperty::Position, 0, glm::vec4(pos.x, pos.y, pos.z, 0.0f), dur, e);
    return *this;
}
Tween& Tween::moveBy(float dur, const glm::vec3& delta, EasingType e) {
    const glm::vec4 cur = currentValue(ActionProperty::Position, 0);
    return moveTo(dur, glm::vec3(cur.x, cur.y, cur.z) + delta, e);
}
Tween& Tween::rotateTo(float dur, const glm::vec3& rot, EasingType e) {
    appendSegment(ActionProperty::Rotation, 0, glm::vec4(rot.x, rot.y, rot.z, 0.0f), dur, e);
    return *this;
}
Tween& Tween::scaleTo(float dur, const glm::vec3& scale, EasingType e) {
    appendSegment(ActionProperty::Scale, 0, glm::vec4(scale.x, scale.y, scale.z, 0.0f), dur, e);
    return *this;
}
Tween& Tween::colorTo(float dur, const glm::vec4& color, EasingType e) {
    appendSegment(ActionProperty::Color, 0, color, dur, e);
    return *this;
}
Tween& Tween::fadeTo(float dur, float alpha, EasingType e) {
    appendSegment(ActionProperty::Alpha, 0, glm::vec4(alpha, 0.0f, 0.0f, 0.0f), dur, e);
    return *this;
}
Tween& Tween::customTo(float dur, int customId, float value, EasingType e) {
    appendSegment(ActionProperty::Custom, customId, glm::vec4(value, 0.0f, 0.0f, 0.0f), dur, e);
    return *this;
}

Tween& Tween::then() {
    _cursor = _lastEnd;
    return *this;
}
Tween& Tween::with() {
    _cursor = _lastStart;
    return *this;
}
Tween& Tween::delay(float seconds) {
    _cursor += seconds;
    return *this;
}
Tween& Tween::event(const std::string& name) {
    _timeline.events.push_back(TimelineEvent{ _cursor, 0, name });
    return *this;
}
Tween& Tween::name(const std::string& n) {
    _timeline.name = n;
    return *this;
}

ActionTimeline Tween::build() {
    std::sort(_timeline.events.begin(), _timeline.events.end(), [](const TimelineEvent& a, const TimelineEvent& b) {
        return a.time < b.time;
    });
    _timeline.recompute();
    return _timeline;
}

int Tween::play(std::function<void()> onFinished) {
    if (!_reg || !_reg->valid(_e)) return 0;
    return TimelineSystem::addOverlay(*_reg, _e, build(), std::move(onFinished));
}

}// namespace Vapor
