#pragma once

#include "Vapor/physics_3d.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/scene.hpp"
#include "components.hpp"
#include "factory.hpp"
#include "systems.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>

// ============================================================================
// Whiteout Survival ad-game — world assembly
//
// Builds the whole level once: ground, decor, lights, the player, the camera,
// and the full pools of bears / pickups / survivors. A handful are activated
// immediately so there's action on frame one; the spawners take it from there.
// ============================================================================

namespace Survival {

    struct GameWorld {
        entt::entity player = entt::null;
        entt::entity camera = entt::null;
        entt::entity state  = entt::null;
    };

    inline GameWorld buildWorld(
        entt::registry& reg, std::shared_ptr<Scene> scene, Physics3D& physics,
        int windowWidth, int windowHeight, Vapor::RNG& rng) {

        Factory::Palette pal;
        GameWorld world;

        Factory::buildGround(reg, scene, physics, pal);
        Factory::buildSky(reg);
        Factory::buildCampfire(reg, scene, pal, glm::vec3(6.0f, 0.0f, 6.0f));
        Factory::buildCampfire(reg, scene, pal, glm::vec3(-8.0f, 0.0f, -5.0f));

        for (int i = 0; i < 22; ++i) {
            glm::vec3 pos = ringPoint(rng, glm::vec3(0.0f), 12.0f, 48.0f);
            Factory::buildTree(reg, scene, pal, pos);
        }

        world.player = Factory::buildPlayer(reg, scene, pal, glm::vec3(0.0f, 1.0f, 0.0f));

        // Camera entity: a virtual camera driven by the isometric follow system.
        world.camera = reg.create();
        {
            auto& cam = reg.emplace<Vapor::VirtualCameraComponent>(world.camera);
            cam.isActive = true;
            cam.aspect   = (float)windowWidth / (float)windowHeight;
            auto& iso = reg.emplace<IsometricCameraComponent>(world.camera);
            iso.target    = world.player;
            iso.offsetDir = glm::normalize(glm::vec3(1.0f, 1.25f, 1.0f));
            iso.distance  = 32.0f;
            iso.orthoSize = 13.0f;
            iso.focus     = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        // Game-state singleton.
        world.state = reg.create();
        reg.emplace<GameStateComponent>(world.state);

        // --- Pools ---
        constexpr int kBearPool     = 14;
        constexpr int kPickupPerType = 20;
        constexpr int kSurvivorPool = 5;

        for (int i = 0; i < kBearPool; ++i) {
            auto e = Factory::buildBear(reg, scene, pal);
            Factory::parkInitially(reg, e);
        }
        for (auto type : { ResourceType::Meat, ResourceType::Wood, ResourceType::Stone }) {
            for (int i = 0; i < kPickupPerType; ++i) {
                auto e = Factory::buildPickup(reg, scene, pal, type);
                Factory::parkInitially(reg, e);
            }
        }
        for (int i = 0; i < kSurvivorPool; ++i) {
            auto e = Factory::buildSurvivor(reg, scene, pal);
            Factory::parkInitially(reg, e);
        }

        // --- Initial activations so the scene isn't empty on frame one ---
        for (int i = 0; i < 4; ++i) {
            if (auto e = Pool::acquire(reg, PoolKind::Bear); e != entt::null) {
                glm::vec3 pos = ringPoint(rng, glm::vec3(0.0f), 10.0f, 18.0f);
                pos.y = 0.7f;
                activateBear(reg, e, pos);
            }
        }
        for (int i = 0; i < 5; ++i) {
            if (auto e = Pool::acquirePickup(reg, ResourceType::Wood); e != entt::null) {
                glm::vec3 pos = ringPoint(rng, glm::vec3(0.0f), 4.0f, 16.0f);
                pos.y = 0.4f;
                activatePickup(reg, e, pos, 1);
            }
            if (auto e = Pool::acquirePickup(reg, ResourceType::Stone); e != entt::null) {
                glm::vec3 pos = ringPoint(rng, glm::vec3(0.0f), 4.0f, 16.0f);
                pos.y = 0.4f;
                activatePickup(reg, e, pos, 1);
            }
        }
        if (auto e = Pool::acquire(reg, PoolKind::Survivor); e != entt::null) {
            glm::vec3 pos = ringPoint(rng, glm::vec3(0.0f), 8.0f, 14.0f);
            pos.y = 0.8f;
            activateSurvivor(reg, e, pos);
        }

        return world;
    }

}// namespace Survival
