#pragma once

#include "Vapor/components.hpp"
#include "action_components.hpp"
#include "timeline_components.hpp"
#include <entt/entt.hpp>

// ============================================================
// Timeline System - Executes multi-track timelines
// ============================================================

class TimelineSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<TimelineComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& timeline = view.get<TimelineComponent>(entity);

            if (!timeline.playing) continue;

            timeline.elapsed += dt;

            // Process each track
            for (size_t t = 0; t < timeline.tracks.size(); t++) {
                auto& track = timeline.tracks[t];

                for (size_t e = 0; e < track.entries.size(); e++) {
                    auto& entry = track.entries[e];

                    // Check if this entry should trigger
                    if (!timeline.triggered[t][e] && timeline.elapsed >= entry.startTime) {
                        timeline.triggered[t][e] = true;
                        triggerAction(reg, track.target, entry.action);
                    }
                }
            }

            // Check completion
            if (timeline.elapsed >= timeline.duration) {
                if (timeline.loop) {
                    timeline.elapsed = 0.0f;
                    timeline.resetTriggers();
                } else {
                    timeline.playing = false;
                    if (timeline.completionTag != 0) {
                        emitCompleteEvent(reg, timeline.completionTag);
                    }
                    completed.push_back(entity);
                }
            }
        }

        // Remove completed timelines
        for (auto entity : completed) {
            reg.remove<TimelineComponent>(entity);
        }
    }

private:
    static void triggerAction(entt::registry& reg, entt::entity target, const Action& action) {
        if (target == entt::null || !reg.valid(target)) return;

        // Check if entity already has an ActionComponent
        if (reg.any_of<ActionComponent>(target)) {
            // Queue it instead
            auto& queue = reg.get_or_emplace<ActionQueueComponent>(target);
            queue.actions.push_back(action);
        } else {
            reg.emplace<ActionComponent>(target, action);
        }
    }

    static void emitCompleteEvent(entt::registry& reg, uint32_t tag) {
        auto eventEntity = reg.create();
        reg.emplace<ActionCompleteEvent>(eventEntity, tag);
    }
};

// ============================================================
// Cinematic System - Executes cutscenes with multiple shots
// ============================================================

class CinematicSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<CinematicComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& cinematic = view.get<CinematicComponent>(entity);

            if (!cinematic.playing) continue;

            auto* shot = cinematic.getCurrentShot();
            if (!shot) {
                completed.push_back(entity);
                continue;
            }

            // First frame of shot: setup camera
            if (cinematic.shotElapsed == 0.0f) {
                setupShot(reg, *shot);
            }

            cinematic.shotElapsed += dt;

            // Process tracks for current shot
            for (size_t t = 0; t < shot->tracks.size(); t++) {
                auto& track = shot->tracks[t];

                for (size_t e = 0; e < track.entries.size(); e++) {
                    auto& entry = track.entries[e];

                    if (!cinematic.triggered[t][e] && cinematic.shotElapsed >= entry.startTime) {
                        cinematic.triggered[t][e] = true;
                        triggerAction(reg, track.target, entry.action);
                    }
                }
            }

            // Check shot completion
            if (cinematic.shotElapsed >= shot->duration) {
                cinematic.advanceShot();

                if (cinematic.isComplete()) {
                    cinematic.playing = false;
                    if (cinematic.completionTag != 0) {
                        emitCompleteEvent(reg, cinematic.completionTag);
                    }
                    completed.push_back(entity);
                }
            }
        }

        for (auto entity : completed) {
            reg.remove<CinematicComponent>(entity);
        }
    }

    // Skip current cinematic (if skippable)
    static void skip(entt::registry& reg, entt::entity entity) {
        if (auto* cinematic = reg.try_get<CinematicComponent>(entity)) {
            cinematic->skip();
        }
    }

private:
    static void setupShot(entt::registry& reg, CinematicShot& shot) {
        // Setup camera for this shot
        if (shot.camera != entt::null && reg.valid(shot.camera)) {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(shot.camera)) {
                transform->position = shot.cameraPosition;
                transform->rotation = shot.cameraRotation;
                transform->isDirty = true;
            }
            if (auto* cam = reg.try_get<Vapor::CameraComponent>(shot.camera)) {
                cam->fov = shot.cameraFOV;
            }
        }

        // Could also trigger dialogue UI here
        // if (!shot.dialogueText.empty()) {
        //     DialogueSystem::show(reg, shot.dialogueSpeaker, shot.dialogueText);
        // }
    }

    static void triggerAction(entt::registry& reg, entt::entity target, const Action& action) {
        if (target == entt::null || !reg.valid(target)) return;

        if (reg.any_of<ActionComponent>(target)) {
            auto& queue = reg.get_or_emplace<ActionQueueComponent>(target);
            queue.actions.push_back(action);
        } else {
            reg.emplace<ActionComponent>(target, action);
        }
    }

    static void emitCompleteEvent(entt::registry& reg, uint32_t tag) {
        auto eventEntity = reg.create();
        reg.emplace<ActionCompleteEvent>(eventEntity, tag);
    }
};

// ============================================================
// Cinematic Request Component - Request to play a cinematic
// ============================================================

struct PlayCinematicRequest {
    CinematicComponent cinematic;
};

// ============================================================
// Cinematic Request System - Handles cinematic play requests
// ============================================================

class CinematicRequestSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<PlayCinematicRequest>();

        for (auto entity : view) {
            auto& request = view.get<PlayCinematicRequest>(entity);

            // Move cinematic to entity and start playing
            reg.emplace_or_replace<CinematicComponent>(entity, std::move(request.cinematic));
            reg.remove<PlayCinematicRequest>(entity);
        }
    }
};
