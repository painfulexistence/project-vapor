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
//   1. Pop trigger events from Physics3D
//   2. Resolve BodyHandle → entity via getBodyUserData
//   3. Emit TriggerEnterEvent / TriggerExitEvent components
//

class TriggerSystem {
public:
    /**
     * Clear previous frame's trigger events.
     * Call at the start of the frame before update().
     */
    static void clearEvents(entt::registry& registry) {
        registry.clear<TriggerEnterEvent>();
        registry.clear<TriggerExitEvent>();
    }

    /**
     * Process physics trigger events and emit ECS event components.
     */
    static void update(entt::registry& registry, Physics3D* physics) {
        if (!physics) return;

        auto events = physics->popTriggerEvents();

        for (const auto& evt : events) {
            // Resolve handles to entities via userData
            auto triggerEntityRaw = physics->getBodyUserData(evt.triggerBody);
            auto otherEntityRaw = physics->getBodyUserData(evt.otherBody);

            // Skip if either entity is invalid (userData not set)
            if (triggerEntityRaw == 0 || otherEntityRaw == 0) continue;

            auto triggerEntity = static_cast<entt::entity>(triggerEntityRaw);
            auto otherEntity = static_cast<entt::entity>(otherEntityRaw);

            // Validate entities still exist
            if (!registry.valid(triggerEntity) || !registry.valid(otherEntity)) continue;

            if (evt.isEnter) {
                // Create event entity to hold the TriggerEnterEvent
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
