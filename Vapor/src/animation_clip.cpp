#include "Vapor/animation_clip.hpp"
#include <algorithm>

namespace Vapor {

// ── FlipbookClip ─────────────────────────────────────────────────────────────

void FlipbookClip::recompute() {
    float d = 0.0f;
    for (const auto& f : frames)
        d += f.duration;
    duration = d;
}

uint16_t FlipbookClip::sample(float t, int* outFrameSlot) const {
    if (frames.empty()) {
        if (outFrameSlot) *outFrameSlot = -1;
        return 0;
    }
    if (t <= 0.0f) {
        if (outFrameSlot) *outFrameSlot = 0;
        return frames.front().frameIndex;
    }
    // Walk the cumulative durations (frame lists are short; linear is fine and
    // keeps the clip data-only — the O(log n) prefix-sum cache lives in the
    // component if a clip ever needs it).
    float acc = 0.0f;
    for (size_t i = 0; i < frames.size(); ++i) {
        acc += frames[i].duration;
        if (t < acc) {
            if (outFrameSlot) *outFrameSlot = static_cast<int>(i);
            return frames[i].frameIndex;
        }
    }
    if (outFrameSlot) *outFrameSlot = static_cast<int>(frames.size() - 1);
    return frames.back().frameIndex;
}

FlipbookClip FlipbookClip::fromIndices(std::string name, std::vector<uint16_t> frameIndices, float frameDuration) {
    FlipbookClip clip;
    clip.name = std::move(name);
    clip.frames.reserve(frameIndices.size());
    for (uint16_t idx : frameIndices)
        clip.frames.push_back(FlipbookFrame{ idx, frameDuration, -1 });
    clip.recompute();
    return clip;
}

FlipbookClip FlipbookClip::fromRange(std::string name, uint16_t firstIndex, uint16_t count, float frameDuration) {
    FlipbookClip clip;
    clip.name = std::move(name);
    clip.frames.reserve(count);
    for (uint16_t i = 0; i < count; ++i)
        clip.frames.push_back(FlipbookFrame{ static_cast<uint16_t>(firstIndex + i), frameDuration, -1 });
    clip.recompute();
    return clip;
}

// ── ActionTrack ──────────────────────────────────────────────────────────────

glm::vec4 ActionTrack::sample(float t) const {
    if (keys.empty()) return glm::vec4(0.0f);
    if (keys.size() == 1 || t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;

    // First key strictly after t (keys are sorted by time).
    auto hi =
        std::upper_bound(keys.begin(), keys.end(), t, [](float time, const ActionKey& k) { return time < k.time; });
    const ActionKey& b = *hi;
    const ActionKey& a = *(hi - 1);

    const float span = b.time - a.time;
    float u = span > 0.0f ? (t - a.time) / span : 1.0f;
    u = applyEasing(u, b.easing);// segment easing is authored on the destination key

    // Rotation quaternions must be interpolated spherically, not component-wise
    // (glTF LINEAR rotation == slerp). value is packed (x,y,z,w).
    if (property == ActionProperty::RotationQuat) {
        glm::quat qa(a.value.w, a.value.x, a.value.y, a.value.z);
        glm::quat qb(b.value.w, b.value.x, b.value.y, b.value.z);
        glm::quat q = glm::slerp(qa, qb, u);
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return a.value + (b.value - a.value) * u;
}

// ── ActionTimeline ───────────────────────────────────────────────────────────

void ActionTimeline::recompute() {
    float d = 0.0f;
    for (const auto& tr : tracks) {
        if (!tr.keys.empty()) d = std::max(d, tr.keys.back().time);
    }
    for (const auto& e : events)
        d = std::max(d, e.time);
    duration = d;
}

// ── SkeletonClip ─────────────────────────────────────────────────────────────

glm::vec3 sampleVec3Track(const std::vector<Vec3Key>& keys, float t, const glm::vec3& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    auto hi = std::upper_bound(keys.begin(), keys.end(), t, [](float time, const Vec3Key& k) { return time < k.time; });
    const Vec3Key& b = *hi;
    const Vec3Key& a = *(hi - 1);
    const float span = b.time - a.time;
    const float u = span > 0.0f ? (t - a.time) / span : 0.0f;
    return a.value + (b.value - a.value) * u;
}

glm::quat sampleQuatTrack(const std::vector<QuatKey>& keys, float t, const glm::quat& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    auto hi = std::upper_bound(keys.begin(), keys.end(), t, [](float time, const QuatKey& k) { return time < k.time; });
    const QuatKey& b = *hi;
    const QuatKey& a = *(hi - 1);
    const float span = b.time - a.time;
    const float u = span > 0.0f ? (t - a.time) / span : 0.0f;
    return glm::slerp(a.value, b.value, u);
}

void SkeletonClip::recompute() {
    float d = 0.0f;
    for (const auto& c : channels) {
        if (!c.translation.empty()) d = std::max(d, c.translation.back().time);
        if (!c.rotation.empty()) d = std::max(d, c.rotation.back().time);
        if (!c.scale.empty()) d = std::max(d, c.scale.back().time);
    }
    duration = d;
}

}// namespace Vapor
