#pragma once

#include "action_components.hpp"
#include "action_systems.hpp"
#include "timeline_asset.hpp"
#include <entt/entt.hpp>
#include <unordered_map>

// ============================================================
// Usage Examples
// ============================================================

namespace TimelineExample {

// ------------------------------------------------------------
// Example 1: Hardcoded timeline (code-driven)
// ------------------------------------------------------------

inline void playHardcodedTimeline(entt::registry& reg,
                                   entt::entity player,
                                   entt::entity npc,
                                   entt::entity director) {
    // Create timeline directly in code
    reg.emplace<ActionTimeline>(director, ActionTimeline{}
        .track(player, {
            Action::moveTo({0, 0, -5}).dur(2.0f).ease(Easing::OutCubic),
            Action::playAnimation("wave"),
            Action::wait(0.5f),
            Action::moveTo({3, 0, 0}).dur(1.5f).ease(Easing::InOutQuad),
        })
        .track(npc, {
            Action::wait(1.0f),
            Action::moveTo({0, 0, -3}).dur(1.5f).ease(Easing::OutQuad),
            Action::scaleTo(1.2f).dur(0.3f).ease(Easing::OutBack),
            Action::scaleTo(1.0f).dur(0.2f),
        })
        .onComplete(1001)
    );
}

// ------------------------------------------------------------
// Example 2: Data-driven timeline (from JSON asset)
// ------------------------------------------------------------

inline void playTimelineFromAsset(entt::registry& reg,
                                   const std::string& assetPath,
                                   entt::entity director) {
    // Load asset
    TimelineAsset asset = TimelineLoader::fromFile(assetPath);

    // Entity registry (could be scene-based, or from NameComponent, etc.)
    // This maps symbolic names to actual entities
    std::unordered_map<std::string, entt::entity> entityMap;

    // Populate from scene (example: iterate entities with NameComponent)
    // auto view = reg.view<NameComponent>();
    // for (auto e : view) {
    //     entityMap[view.get<NameComponent>(e).name] = e;
    // }

    // Or manually for this example:
    // entityMap["player"] = playerEntity;
    // entityMap["npc_guide"] = npcEntity;
    // entityMap["camera"] = cameraEntity;

    // Instantiate with resolver
    ActionTimeline timeline = asset.instantiate([&](const std::string& name) {
        auto it = entityMap.find(name);
        return it != entityMap.end() ? it->second : entt::null;
    });

    // Attach to director entity
    reg.emplace<ActionTimeline>(director, std::move(timeline));
}

// ------------------------------------------------------------
// Example 3: Scene with named entities
// ------------------------------------------------------------

struct NameComponent {
    std::string name;
};

class TimelineManager {
public:
    TimelineManager(entt::registry& reg) : m_reg(reg) {}

    // Build entity map from NameComponents
    void refreshEntityMap() {
        m_entityMap.clear();
        auto view = m_reg.view<NameComponent>();
        for (auto entity : view) {
            m_entityMap[view.get<NameComponent>(entity).name] = entity;
        }
    }

    // Play a timeline asset
    entt::entity play(const TimelineAsset& asset) {
        ActionTimeline timeline = asset.instantiate([this](const std::string& name) {
            auto it = m_entityMap.find(name);
            return it != m_entityMap.end() ? it->second : entt::null;
        });

        // Create a timeline entity
        auto entity = m_reg.create();
        m_reg.emplace<ActionTimeline>(entity, std::move(timeline));
        return entity;
    }

    // Play from file path
    entt::entity playFromFile(const std::string& path) {
        return play(TimelineLoader::fromFile(path));
    }

    // Stop a playing timeline
    void stop(entt::entity timelineEntity) {
        if (m_reg.valid(timelineEntity)) {
            m_reg.remove<ActionTimeline>(timelineEntity);
        }
    }

private:
    entt::registry& m_reg;
    std::unordered_map<std::string, entt::entity> m_entityMap;
};

// ------------------------------------------------------------
// Example 4: Responding to timeline completion
// ------------------------------------------------------------

inline void handleTimelineCompletion(entt::registry& reg) {
    auto view = reg.view<ActionCompleteEvent>();

    for (auto entity : view) {
        auto& event = view.get<ActionCompleteEvent>(entity);

        switch (event.tag) {
        case 1001: // intro_cutscene completed
            // Transition to gameplay
            // sceneManager.loadScene("level_01");
            break;

        case 1002: // death_animation completed
            // Show game over screen
            break;

        default:
            break;
        }
    }

    // Events are cleaned up by ActionEventSystem::cleanup()
}

// ------------------------------------------------------------
// Example 5: Full game loop integration
// ------------------------------------------------------------

inline void gameLoopExample(entt::registry& reg, float dt) {
    // 1. Update action systems (order matters)
    ActionSystem::update(reg, dt);           // Single actions
    ActionSequenceSystem::update(reg, dt);   // Sequences on entities
    ActionTimelineSystem::update(reg, dt);   // Multi-track timelines

    // 2. Handle completion events
    handleTimelineCompletion(reg);

    // 3. Cleanup events (at end of frame)
    ActionEventSystem::cleanup(reg);
}

} // namespace TimelineExample
