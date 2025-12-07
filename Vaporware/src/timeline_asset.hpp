#pragma once

#include "action_components.hpp"
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

// ============================================================
// Timeline Asset - Data-driven timeline definitions
//
// Asset uses symbolic names ("player", "npc_01") instead of entity handles.
// At runtime, instantiate with an entity resolver to bind to actual entities.
// ============================================================

// ============================================================
// Serializable Action Definition
// ============================================================

struct ActionDef {
    std::string type;      // "moveTo", "scaleTo", "wait", etc.
    float duration = 0.0f;
    std::string easing;    // "Linear", "OutBack", etc.

    // Values (JSON stores as arrays/numbers)
    std::vector<float> vec3;   // [x, y, z]
    std::vector<float> vec4;   // [r, g, b, a]
    std::vector<float> quat;   // [w, x, y, z]
    float floatValue = 0.0f;
    bool boolValue = true;
    std::string stringValue;

    uint32_t completionTag = 0;
};

// ============================================================
// Serializable Sequence Definition
// ============================================================

struct SequenceDef {
    std::vector<ActionDef> actions;
    uint32_t completionTag = 0;
};

// ============================================================
// Serializable Track Definition
// ============================================================

struct TrackDef {
    std::string target;  // Symbolic name: "player", "enemy_01", etc.
    SequenceDef sequence;
};

// ============================================================
// Timeline Asset
// ============================================================

struct TimelineAsset {
    std::string name;
    std::vector<TrackDef> tracks;
    uint32_t completionTag = 0;

    // Entity resolver: maps symbolic names to runtime entities
    using EntityResolver = std::function<entt::entity(const std::string&)>;

    // Instantiate into a runtime ActionTimeline
    ActionTimeline instantiate(EntityResolver resolver) const {
        ActionTimeline timeline;
        timeline.completionTag = completionTag;

        for (const auto& trackDef : tracks) {
            entt::entity target = resolver(trackDef.target);
            if (target == entt::null) continue;

            ActionSequence sequence;
            sequence.completionTag = trackDef.sequence.completionTag;

            for (const auto& actionDef : trackDef.sequence.actions) {
                Action action = convertAction(actionDef);
                sequence.actions.push_back(std::move(action));
            }

            timeline.tracks.push_back({ target, std::move(sequence) });
        }

        return timeline;
    }

private:
    static Action convertAction(const ActionDef& def) {
        Action action;
        action.duration = def.duration;
        action.easing = resolveEasing(def.easing);
        action.completionTag = def.completionTag;

        // Parse type and set values
        if (def.type == "moveTo") {
            action.type = ActionType::MoveTo;
            if (def.vec3.size() >= 3) {
                action.vec3Value = { def.vec3[0], def.vec3[1], def.vec3[2] };
            }
        }
        else if (def.type == "moveBy") {
            action.type = ActionType::MoveBy;
            if (def.vec3.size() >= 3) {
                action.vec3Value = { def.vec3[0], def.vec3[1], def.vec3[2] };
            }
        }
        else if (def.type == "scaleTo") {
            action.type = ActionType::ScaleTo;
            if (def.vec3.size() >= 3) {
                action.vec3Value = { def.vec3[0], def.vec3[1], def.vec3[2] };
            } else if (def.vec3.size() == 1) {
                action.vec3Value = glm::vec3(def.vec3[0]);
            }
        }
        else if (def.type == "rotateTo") {
            action.type = ActionType::RotateTo;
            if (def.quat.size() >= 4) {
                action.quatValue = { def.quat[0], def.quat[1], def.quat[2], def.quat[3] };
            }
        }
        else if (def.type == "fadeTo") {
            action.type = ActionType::FadeTo;
            action.vec4Value = glm::vec4(1.0f, 1.0f, 1.0f, def.floatValue);
        }
        else if (def.type == "colorTo") {
            action.type = ActionType::ColorTo;
            if (def.vec4.size() >= 4) {
                action.vec4Value = { def.vec4[0], def.vec4[1], def.vec4[2], def.vec4[3] };
            }
        }
        else if (def.type == "wait") {
            action.type = ActionType::Wait;
        }
        else if (def.type == "setActive") {
            action.type = ActionType::SetActive;
            action.boolValue = def.boolValue;
        }
        else if (def.type == "playAnimation") {
            action.type = ActionType::PlayAnimation;
            action.stringValue = def.stringValue;
        }

        return action;
    }

    static EasingFunction resolveEasing(const std::string& name) {
        static const std::unordered_map<std::string, EasingFunction> easings = {
            { "Linear", Easing::Linear },
            { "InQuad", Easing::InQuad },
            { "OutQuad", Easing::OutQuad },
            { "InOutQuad", Easing::InOutQuad },
            { "InCubic", Easing::InCubic },
            { "OutCubic", Easing::OutCubic },
            { "InOutCubic", Easing::InOutCubic },
            { "InBack", Easing::InBack },
            { "OutBack", Easing::OutBack },
            { "InOutBack", Easing::InOutBack },
            { "OutElastic", Easing::OutElastic },
            { "OutBounce", Easing::OutBounce },
        };

        auto it = easings.find(name);
        return it != easings.end() ? it->second : Easing::Linear;
    }
};

// ============================================================
// JSON Serialization (nlohmann/json)
// ============================================================

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ActionDef,
    type, duration, easing, vec3, vec4, quat, floatValue, boolValue, stringValue, completionTag)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SequenceDef,
    actions, completionTag)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TrackDef,
    target, sequence)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TimelineAsset,
    name, tracks, completionTag)

// ============================================================
// Asset Loading Helper
// ============================================================

namespace TimelineLoader {

inline TimelineAsset fromJson(const std::string& jsonStr) {
    auto j = nlohmann::json::parse(jsonStr);
    return j.get<TimelineAsset>();
}

inline TimelineAsset fromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    nlohmann::json j;
    file >> j;
    return j.get<TimelineAsset>();
}

inline std::string toJson(const TimelineAsset& asset, int indent = 2) {
    nlohmann::json j = asset;
    return j.dump(indent);
}

} // namespace TimelineLoader
