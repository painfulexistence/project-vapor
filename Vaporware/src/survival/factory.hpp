#pragma once

#include "Vapor/components.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/scene.hpp"
#include "components.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>

// ============================================================================
// Whiteout Survival ad-game — entity factory
//
// Builds every piece of world geometry once, up front, so the renderer can
// stage it all in a single pass. Pooled actors (bears, pickups, survivors) are
// created here too; the systems only ever toggle them in and out of play.
// ============================================================================

namespace Survival::Factory {

    // A flat-shaded PBR material driven purely by baseColorFactor — the renderer
    // re-uploads this every frame, so no texture is needed for solid colors.
    inline std::shared_ptr<Vapor::Material> colorMaterial(
        const glm::vec3& color, float roughness = 0.85f, float metallic = 0.0f,
        const glm::vec3& emissive = glm::vec3(0.0f)) {
        return std::make_shared<Vapor::Material>(Vapor::Material{
            .baseColorFactor = glm::vec4(color, 1.0f),
            .metallicFactor  = metallic,
            .roughnessFactor = roughness,
            .emissiveFactor  = emissive,
        });
    }

    // Shared materials, built once. Reusing a material across meshes is fine:
    // the renderer keys material data by the material pointer.
    struct Palette {
        std::shared_ptr<Vapor::Material> snow   = colorMaterial({ 0.86f, 0.90f, 0.96f }, 0.95f);
        std::shared_ptr<Vapor::Material> player = colorMaterial({ 0.90f, 0.55f, 0.20f }, 0.6f);
        std::shared_ptr<Vapor::Material> bear   = colorMaterial({ 0.22f, 0.18f, 0.16f }, 0.8f);
        std::shared_ptr<Vapor::Material> meat   = colorMaterial({ 0.80f, 0.25f, 0.28f }, 0.5f);
        std::shared_ptr<Vapor::Material> wood   = colorMaterial({ 0.45f, 0.30f, 0.16f }, 0.9f);
        std::shared_ptr<Vapor::Material> stone  = colorMaterial({ 0.55f, 0.57f, 0.60f }, 0.9f);
        std::shared_ptr<Vapor::Material> person = colorMaterial({ 0.30f, 0.55f, 0.85f }, 0.6f);
        std::shared_ptr<Vapor::Material> trunk  = colorMaterial({ 0.35f, 0.24f, 0.14f }, 0.9f);
        std::shared_ptr<Vapor::Material> pine   = colorMaterial({ 0.16f, 0.40f, 0.26f }, 0.9f);
        std::shared_ptr<Vapor::Material> fire   = colorMaterial({ 0.95f, 0.55f, 0.20f }, 0.4f, 0.0f, { 1.4f, 0.7f, 0.2f });
    };

    inline std::shared_ptr<Vapor::Material> pickupMaterial(Palette& pal, ResourceType type) {
        switch (type) {
        case ResourceType::Meat:  return pal.meat;
        case ResourceType::Wood:  return pal.wood;
        case ResourceType::Stone: return pal.stone;
        }
        return pal.meat;
    }

    inline std::shared_ptr<Vapor::Mesh> pickupMesh(ResourceType type, std::shared_ptr<Vapor::Material> mat) {
        switch (type) {
        case ResourceType::Meat:  return MeshBuilder::buildCube(0.45f, mat);
        case ResourceType::Wood:  return MeshBuilder::buildCylinder(0.7f, 0.13f, 10, mat);
        case ResourceType::Stone: return MeshBuilder::buildCube(0.4f, mat);
        }
        return MeshBuilder::buildCube(0.4f, mat);
    }

    // Attach a mesh to an entity and register its geometry with the scene so it
    // gets staged. The transform comes from the entity's TransformComponent.
    inline void attachMesh(entt::registry& reg, entt::entity e, std::shared_ptr<Scene> scene,
                           std::shared_ptr<Vapor::Mesh> mesh) {
        scene->addMesh(mesh);
        auto& mr = reg.get_or_emplace<Vapor::MeshRendererComponent>(e);
        mr.meshes.push_back(mesh);
    }

    // ------------------------------------------------------------------------
    // Ground, decor, lighting
    // ------------------------------------------------------------------------
    inline void buildGround(entt::registry& reg, std::shared_ptr<Scene> scene, Physics3D& physics, Palette& pal) {
        auto e = reg.create();
        auto& t = reg.emplace<Vapor::TransformComponent>(e);
        t.position = glm::vec3(0.0f, -0.5f, 0.0f);
        t.scale    = glm::vec3(120.0f, 1.0f, 120.0f);
        t.isDirty  = true;

        auto& col = reg.emplace<Vapor::BoxColliderComponent>(e);
        col.halfSize = glm::vec3(60.0f, 0.5f, 60.0f);
        auto& rb = reg.emplace<Vapor::RigidbodyComponent>(e);
        rb.motionType = BodyMotionType::Static;
        rb.body = physics.createBoxBody(col.halfSize, t.position, t.rotation, rb.motionType);
        physics.addBody(rb.body, false);

        // Unit cube scaled to a slab — geometry is unit-sized, the transform stretches it.
        attachMesh(reg, e, scene, MeshBuilder::buildCube(1.0f, pal.snow));
    }

    inline void buildSky(entt::registry& reg) {
        // Cold, low directional light — the player's torch does the rest.
        auto e = reg.create();
        auto& dl = reg.emplace<DirectionalLightComponent>(e);
        dl.direction = glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f));
        dl.color     = glm::vec3(0.55f, 0.62f, 0.78f);
        dl.intensity = 1.6f;
    }

    inline void buildCampfire(entt::registry& reg, std::shared_ptr<Scene> scene, Palette& pal, const glm::vec3& pos) {
        auto logs = reg.create();
        auto& t = reg.emplace<Vapor::TransformComponent>(logs);
        t.position = pos;
        t.isDirty  = true;
        attachMesh(reg, logs, scene, MeshBuilder::buildCylinder(0.4f, 0.6f, 10, pal.fire));

        auto light = reg.create();
        auto& lt = reg.emplace<Vapor::TransformComponent>(light);
        lt.position = pos + glm::vec3(0.0f, 0.8f, 0.0f);
        lt.isDirty  = true;
        auto& pl = reg.emplace<PointLightComponent>(light);
        pl.color     = glm::vec3(1.0f, 0.65f, 0.30f);
        pl.intensity = 8.0f;
        pl.radius    = 0.8f;
    }

    inline void buildTree(entt::registry& reg, std::shared_ptr<Scene> scene, Palette& pal, const glm::vec3& pos) {
        auto trunk = reg.create();
        auto& tt = reg.emplace<Vapor::TransformComponent>(trunk);
        tt.position = pos + glm::vec3(0.0f, 1.0f, 0.0f);
        tt.isDirty  = true;
        attachMesh(reg, trunk, scene, MeshBuilder::buildCylinder(2.0f, 0.2f, 8, pal.trunk));

        auto foliage = reg.create();
        auto& ft = reg.emplace<Vapor::TransformComponent>(foliage);
        ft.position = pos + glm::vec3(0.0f, 2.8f, 0.0f);
        ft.isDirty  = true;
        attachMesh(reg, foliage, scene, MeshBuilder::buildCylinder(2.4f, 1.0f, 8, pal.pine));
    }

    // ------------------------------------------------------------------------
    // Player (capsule + character controller + torch + all the stat blocks)
    // ------------------------------------------------------------------------
    inline entt::entity buildPlayer(entt::registry& reg, std::shared_ptr<Scene> scene, Palette& pal,
                                    const glm::vec3& pos) {
        auto e = reg.create();
        reg.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{ "Player" });
        reg.emplace<PlayerTag>(e);

        auto& t = reg.emplace<Vapor::TransformComponent>(e);
        t.position = pos;
        t.isDirty  = true;

        auto& body = reg.emplace<Vapor::CharacterBodyComponent>(e);
        body.settings.height = 1.8f;
        body.settings.radius = 0.4f;

        attachMesh(reg, e, scene, MeshBuilder::buildCapsule(1.8f, 0.4f, 14, 7, pal.player));

        reg.emplace<PlayerControlComponent>(e);
        reg.emplace<FacingComponent>(e);
        reg.emplace<MoveStatsComponent>(e);
        reg.emplace<CombatStatsComponent>(e);
        reg.emplace<HealthComponent>(e);
        reg.emplace<InventoryComponent>(e);
        reg.emplace<ExperienceComponent>(e);
        reg.emplace<EquipmentComponent>(e);

        // Carried torch: a flickering point light riding the player's transform.
        reg.emplace<TorchComponent>(e);
        auto& pl = reg.emplace<PointLightComponent>(e);
        pl.color     = glm::vec3(1.0f, 0.7f, 0.4f);
        pl.intensity = 6.0f;
        pl.radius    = 0.7f;

        return e;
    }

    // ------------------------------------------------------------------------
    // Pools — created hidden + InactiveTag; spawners bring them into play.
    // ------------------------------------------------------------------------
    inline void parkInitially(entt::registry& reg, entt::entity e) {
        reg.emplace<InactiveTag>(e);
        if (auto* mr = reg.try_get<Vapor::MeshRendererComponent>(e)) mr->visible = false;
        if (auto* t = reg.try_get<Vapor::TransformComponent>(e)) {
            t->position = glm::vec3(0.0f, -1000.0f, 0.0f);
            t->isDirty  = true;
        }
    }

    inline entt::entity buildBear(entt::registry& reg, std::shared_ptr<Scene> scene, Palette& pal) {
        auto e = reg.create();
        reg.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{ "Bear" });
        reg.emplace<EnemyTag>(e);
        reg.emplace<PooledComponent>(e, PooledComponent{ PoolKind::Bear });
        reg.emplace<Vapor::TransformComponent>(e).isDirty = true;
        reg.emplace<AIComponent>(e);
        auto& h = reg.emplace<HealthComponent>(e);
        h.hp = h.maxHp = 60.0f;
        reg.emplace<LootTableComponent>(e, LootTableComponent{ ResourceType::Meat, 1, 3, 35.0f });
        attachMesh(reg, e, scene, MeshBuilder::buildCapsule(1.4f, 0.6f, 14, 7, pal.bear));
        return e;
    }

    inline entt::entity buildPickup(entt::registry& reg, std::shared_ptr<Scene> scene, Palette& pal,
                                    ResourceType type) {
        auto e = reg.create();
        reg.emplace<PooledComponent>(e, PooledComponent{ PoolKind::Pickup, type });
        reg.emplace<Vapor::TransformComponent>(e).isDirty = true;
        auto& pick = reg.emplace<PickupComponent>(e);
        pick.type = type;
        reg.emplace<BobComponent>(e);
        attachMesh(reg, e, scene, pickupMesh(type, pickupMaterial(pal, type)));
        return e;
    }

    inline entt::entity buildSurvivor(entt::registry& reg, std::shared_ptr<Scene> scene, Palette& pal) {
        auto e = reg.create();
        reg.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{ "Survivor" });
        reg.emplace<PooledComponent>(e, PooledComponent{ PoolKind::Survivor });
        reg.emplace<Vapor::TransformComponent>(e).isDirty = true;
        reg.emplace<SurvivorComponent>(e);
        attachMesh(reg, e, scene, MeshBuilder::buildCapsule(1.6f, 0.3f, 12, 6, pal.person));
        return e;
    }

}// namespace Survival::Factory
