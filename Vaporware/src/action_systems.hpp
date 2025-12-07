#pragma once

#include "Vapor/components.hpp"
#include "action_components.hpp"
#include <entt/entt.hpp>
#include <vector>

// ============================================================
// Action System - Executes single ActionComponent on entities
// ============================================================

class ActionSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<ActionComponent, Vapor::TransformComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& ac = view.get<ActionComponent>(entity);
            auto& transform = view.get<Vapor::TransformComponent>(entity);

            // Start action
            if (!ac.started) {
                ac.started = true;
                captureStartValues(reg, entity, ac, transform);
            }

            // Update elapsed time
            ac.elapsed += dt;

            // Apply action
            applyAction(reg, entity, ac, transform);

            // Check completion
            if (ac.isComplete()) {
                if (ac.action.completionTag != 0) {
                    emitCompleteEvent(reg, ac.action.completionTag);
                }
                completed.push_back(entity);
            }
        }

        // Remove completed actions
        for (auto entity : completed) {
            reg.remove<ActionComponent>(entity);
        }
    }

private:
    static void captureStartValues(entt::registry& reg, entt::entity entity,
                                   ActionComponent& ac, Vapor::TransformComponent& transform) {
        switch (ac.action.type) {
        case ActionType::MoveTo:
        case ActionType::MoveBy:
            ac.startVec3 = transform.position;
            break;
        case ActionType::ScaleTo:
            ac.startVec3 = transform.scale;
            break;
        case ActionType::RotateTo:
            ac.startQuat = transform.rotation;
            break;
        case ActionType::FadeTo:
        case ActionType::ColorTo:
            // TODO: get from material/sprite component
            ac.startVec4 = glm::vec4(1.0f);
            break;
        default:
            break;
        }
    }

    static void applyAction(entt::registry& reg, entt::entity entity,
                            ActionComponent& ac, Vapor::TransformComponent& transform) {
        float t = ac.getProgress();

        switch (ac.action.type) {
        case ActionType::MoveTo:
            transform.position = glm::mix(ac.startVec3, ac.action.vec3Value, t);
            transform.isDirty = true;
            break;

        case ActionType::MoveBy:
            transform.position = ac.startVec3 + ac.action.vec3Value * t;
            transform.isDirty = true;
            break;

        case ActionType::ScaleTo:
            transform.scale = glm::mix(ac.startVec3, ac.action.vec3Value, t);
            transform.isDirty = true;
            break;

        case ActionType::RotateTo:
            transform.rotation = glm::slerp(ac.startQuat, ac.action.quatValue, t);
            transform.isDirty = true;
            break;

        case ActionType::FadeTo:
        case ActionType::ColorTo:
            // TODO: apply to material/sprite component
            break;

        case ActionType::SetActive:
            // TODO: enable/disable rendering
            break;

        case ActionType::PlayAnimation:
            // TODO: trigger animation system
            break;

        case ActionType::Wait:
            // Do nothing, just wait
            break;
        }
    }

    static void emitCompleteEvent(entt::registry& reg, uint32_t tag) {
        auto entity = reg.create();
        reg.emplace<ActionCompleteEvent>(entity, tag);
    }
};

// ============================================================
// ActionSequence System - Executes sequential actions
// ============================================================

class ActionSequenceSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<ActionSequence, Vapor::TransformComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& seq = view.get<ActionSequence>(entity);
            auto& transform = view.get<Vapor::TransformComponent>(entity);

            if (seq.isComplete()) {
                completed.push_back(entity);
                continue;
            }

            auto* action = seq.current();
            if (!action) {
                completed.push_back(entity);
                continue;
            }

            // Start current action
            if (!seq.started) {
                seq.started = true;
                captureStartValues(reg, entity, seq, transform);
            }

            // Update elapsed time
            seq.elapsed += dt;

            // Apply current action
            applyAction(reg, entity, seq, transform);

            // Check if current action is complete
            if (seq.elapsed >= action->duration) {
                // Emit per-action completion event
                if (action->completionTag != 0) {
                    emitCompleteEvent(reg, action->completionTag);
                }
                seq.advance();

                // Check if sequence is complete
                if (seq.isComplete()) {
                    if (seq.completionTag != 0) {
                        emitCompleteEvent(reg, seq.completionTag);
                    }
                    completed.push_back(entity);
                }
            }
        }

        // Remove completed sequences
        for (auto entity : completed) {
            reg.remove<ActionSequence>(entity);
        }
    }

private:
    static void captureStartValues(entt::registry& reg, entt::entity entity,
                                   ActionSequence& seq, Vapor::TransformComponent& transform) {
        auto* action = seq.current();
        if (!action) return;

        switch (action->type) {
        case ActionType::MoveTo:
        case ActionType::MoveBy:
            seq.startVec3 = transform.position;
            break;
        case ActionType::ScaleTo:
            seq.startVec3 = transform.scale;
            break;
        case ActionType::RotateTo:
            seq.startQuat = transform.rotation;
            break;
        case ActionType::FadeTo:
        case ActionType::ColorTo:
            seq.startVec4 = glm::vec4(1.0f);
            break;
        default:
            break;
        }
    }

    static void applyAction(entt::registry& reg, entt::entity entity,
                            ActionSequence& seq, Vapor::TransformComponent& transform) {
        auto* action = seq.current();
        if (!action) return;

        float t = seq.getProgress();

        switch (action->type) {
        case ActionType::MoveTo:
            transform.position = glm::mix(seq.startVec3, action->vec3Value, t);
            transform.isDirty = true;
            break;

        case ActionType::MoveBy:
            transform.position = seq.startVec3 + action->vec3Value * t;
            transform.isDirty = true;
            break;

        case ActionType::ScaleTo:
            transform.scale = glm::mix(seq.startVec3, action->vec3Value, t);
            transform.isDirty = true;
            break;

        case ActionType::RotateTo:
            transform.rotation = glm::slerp(seq.startQuat, action->quatValue, t);
            transform.isDirty = true;
            break;

        case ActionType::FadeTo:
        case ActionType::ColorTo:
            // TODO: apply to material/sprite
            break;

        case ActionType::SetActive:
            // TODO: enable/disable rendering
            break;

        case ActionType::PlayAnimation:
            // TODO: trigger animation
            break;

        case ActionType::Wait:
            break;
        }
    }

    static void emitCompleteEvent(entt::registry& reg, uint32_t tag) {
        auto entity = reg.create();
        reg.emplace<ActionCompleteEvent>(entity, tag);
    }
};

// ============================================================
// ActionTimeline System - Executes parallel tracks
// ============================================================

class ActionTimelineSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<ActionTimeline>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& timeline = view.get<ActionTimeline>(entity);

            // Update each track
            for (auto& track : timeline.tracks) {
                if (track.sequence.isComplete()) continue;
                if (!reg.valid(track.target)) continue;

                auto* transform = reg.try_get<Vapor::TransformComponent>(track.target);
                if (!transform) continue;

                updateTrack(reg, track, *transform, dt);
            }

            // Check if all tracks complete
            if (timeline.isComplete()) {
                if (timeline.completionTag != 0) {
                    emitCompleteEvent(reg, timeline.completionTag);
                }
                completed.push_back(entity);
            }
        }

        // Remove completed timelines
        for (auto entity : completed) {
            reg.remove<ActionTimeline>(entity);
        }
    }

private:
    static void updateTrack(entt::registry& reg, ActionTimeline::Track& track,
                            Vapor::TransformComponent& transform, float dt) {
        auto& seq = track.sequence;

        if (seq.isComplete()) return;

        auto* action = seq.current();
        if (!action) return;

        // Start current action
        if (!seq.started) {
            seq.started = true;
            captureStartValues(seq, transform, *action);
        }

        // Update elapsed
        seq.elapsed += dt;

        // Apply
        applyAction(seq, transform, *action);

        // Check completion
        if (seq.elapsed >= action->duration) {
            if (action->completionTag != 0) {
                emitCompleteEvent(reg, action->completionTag);
            }
            seq.advance();
        }
    }

    static void captureStartValues(ActionSequence& seq, Vapor::TransformComponent& transform,
                                   const Action& action) {
        switch (action.type) {
        case ActionType::MoveTo:
        case ActionType::MoveBy:
            seq.startVec3 = transform.position;
            break;
        case ActionType::ScaleTo:
            seq.startVec3 = transform.scale;
            break;
        case ActionType::RotateTo:
            seq.startQuat = transform.rotation;
            break;
        case ActionType::FadeTo:
        case ActionType::ColorTo:
            seq.startVec4 = glm::vec4(1.0f);
            break;
        default:
            break;
        }
    }

    static void applyAction(ActionSequence& seq, Vapor::TransformComponent& transform,
                            const Action& action) {
        float t = seq.getProgress();

        switch (action.type) {
        case ActionType::MoveTo:
            transform.position = glm::mix(seq.startVec3, action.vec3Value, t);
            transform.isDirty = true;
            break;

        case ActionType::MoveBy:
            transform.position = seq.startVec3 + action.vec3Value * t;
            transform.isDirty = true;
            break;

        case ActionType::ScaleTo:
            transform.scale = glm::mix(seq.startVec3, action.vec3Value, t);
            transform.isDirty = true;
            break;

        case ActionType::RotateTo:
            transform.rotation = glm::slerp(seq.startQuat, action.quatValue, t);
            transform.isDirty = true;
            break;

        case ActionType::FadeTo:
        case ActionType::ColorTo:
        case ActionType::SetActive:
        case ActionType::PlayAnimation:
        case ActionType::Wait:
            break;
        }
    }

    static void emitCompleteEvent(entt::registry& reg, uint32_t tag) {
        auto entity = reg.create();
        reg.emplace<ActionCompleteEvent>(entity, tag);
    }
};

// ============================================================
// ActionEventSystem - Cleans up completion events
// ============================================================

class ActionEventSystem {
public:
    static void cleanup(entt::registry& reg) {
        auto view = reg.view<ActionCompleteEvent>();
        for (auto entity : view) {
            reg.destroy(entity);
        }
    }
};
