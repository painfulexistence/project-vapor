#pragma once

#include "components.hpp"
#include "physics_3d.hpp"
#include <entt/entt.hpp>

namespace Vapor {

// ============================================================
// Trigger System - converts physics trigger events to ECS events
// ============================================================
//
// Prerequisites:
//   When creating bodies/triggers, store the entity ID using:
//     physics->setBodyUserData(body, static_cast<Uint64>(entt::to_integral(entity)));
//
// This system will:
//   1. Clear previous frame's trigger events
//   2. Pop trigger events from Physics3D
//   3. Resolve BodyHandle → entity via getBodyUserData
//   4. Emit TriggerEnterEvent / TriggerExitEvent components
//

class TriggerSystem {
public:
    static void update(entt::registry& registry, Physics3D* physics) {
        // 1. Clear previous frame's events
        registry.clear<TriggerEnterEvent>();
        registry.clear<TriggerExitEvent>();

        if (!physics) return;

        // 2. Pop trigger events from physics
        auto events = physics->popTriggerEvents();

        for (const auto& evt : events) {
            // 3. Resolve handles to entities via userData
            auto triggerEntityRaw = physics->getBodyUserData(evt.triggerBody);
            auto otherEntityRaw = physics->getBodyUserData(evt.otherBody);

            // Skip if either entity is invalid (userData not set)
            if (triggerEntityRaw == 0 || otherEntityRaw == 0) continue;

            auto triggerEntity = static_cast<entt::entity>(triggerEntityRaw);
            auto otherEntity = static_cast<entt::entity>(otherEntityRaw);

            // Validate entities still exist
            if (!registry.valid(triggerEntity) || !registry.valid(otherEntity)) continue;

            // 4. Emit event components
            if (evt.isEnter) {
                auto eventEntity = registry.create();
                registry.emplace<TriggerEnterEvent>(eventEntity,
                    TriggerEnterEvent{ triggerEntity, otherEntity });
            } else {
                auto eventEntity = registry.create();
                registry.emplace<TriggerExitEvent>(eventEntity,
                    TriggerExitEvent{ triggerEntity, otherEntity });
            }
        }
    }
};

} // namespace Vapor
