#pragma once

#include "Vapor/components.hpp"
#include <entt/entt.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>

// ============================================================================
// Whiteout Survival ad-game — ECS components
//
// The core loop: carry a torch out into the snow, kill bears, pick up meat and
// resources, rescue stranded survivors, and spend what you gather upgrading your
// gear. Everything here is plain data; behavior lives in survival/systems.hpp.
// ============================================================================

namespace Survival {

    // ------------------------------------------------------------------------
    // Lighting (same shape as the engine demo's light components so the
    // LightGatherSystem can feed Scene::pointLights / directionalLights).
    // ------------------------------------------------------------------------
    struct PointLightComponent {
        glm::vec3 color     = glm::vec3(1.0f);
        float     intensity = 1.0f;
        float     radius    = 0.5f;
    };

    struct DirectionalLightComponent {
        glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 color     = glm::vec3(1.0f);
        float     intensity = 1.0f;
    };

    // ------------------------------------------------------------------------
    // Resources the player gathers.
    // ------------------------------------------------------------------------
    enum class ResourceType { Meat, Wood, Stone };

    // ------------------------------------------------------------------------
    // Player
    // ------------------------------------------------------------------------
    struct PlayerTag {};

    // Raw per-frame intent, written by PlayerInputSystem, consumed by movement /
    // attack / upgrade systems. Kept separate from stats so input has one owner.
    struct PlayerControlComponent {
        glm::vec2 moveInput    = glm::vec2(0.0f);// (strafe, forward) in [-1, 1]
        bool      attack       = false;
        bool      upgradeWeapon = false;
        bool      upgradeArmor  = false;
    };

    // Last non-zero movement direction on the ground plane. Drives the attack
    // arc and which way the character model faces.
    struct FacingComponent {
        glm::vec3 dir = glm::vec3(0.0f, 0.0f, 1.0f);
    };

    struct HealthComponent {
        float hp          = 100.0f;
        float maxHp       = 100.0f;
        float invulnTimer = 0.0f;// brief i-frames after taking a hit
    };

    struct CombatStatsComponent {
        float attackDamage  = 25.0f;
        float attackRange   = 2.5f;
        float attackArcDeg  = 120.0f;// half-angle tolerance handled in system
        float attackCooldown = 0.5f;
        float cooldownTimer  = 0.0f;
    };

    struct MoveStatsComponent {
        float moveSpeed = 6.0f;
    };

    struct InventoryComponent {
        int meat      = 0;
        int wood      = 0;
        int stone     = 0;
        int survivors = 0;
    };

    struct ExperienceComponent {
        int   level    = 1;
        float xp       = 0.0f;
        float xpToNext = 100.0f;
    };

    struct EquipmentComponent {
        int weaponLevel = 1;
        int armorLevel  = 1;
    };

    // The torch the player carries. Drives a flickering point light; the light
    // itself lives on the same entity (PointLightComponent), so it follows the
    // player for free through its TransformComponent.
    struct TorchComponent {
        float baseIntensity = 6.0f;
        float flickerAmount = 1.5f;
        float flickerSpeed  = 12.0f;
        float timer         = 0.0f;
    };

    // ------------------------------------------------------------------------
    // Enemies (bears) — kinematic AI, moved directly via TransformComponent.
    // ------------------------------------------------------------------------
    struct EnemyTag {};

    enum class AIState { Idle, Wander, Chase, Attack };

    struct AIComponent {
        AIState   state          = AIState::Idle;
        float     detectRange    = 12.0f;
        float     attackRange    = 2.0f;
        float     speed          = 3.0f;
        float     attackDamage   = 12.0f;
        float     attackCooldown = 1.2f;
        float     cooldownTimer  = 0.0f;
        float     wanderTimer    = 0.0f;
        glm::vec3 wanderTarget   = glm::vec3(0.0f);
    };

    // What an entity yields when it dies: a loot drop and some XP.
    struct LootTableComponent {
        ResourceType drop      = ResourceType::Meat;
        int          dropMin   = 1;
        int          dropMax   = 3;
        float        xpReward  = 35.0f;
    };

    // ------------------------------------------------------------------------
    // World pickups (meat / wood / stone lying on the ground).
    // ------------------------------------------------------------------------
    struct PickupComponent {
        ResourceType type        = ResourceType::Meat;
        int          amount      = 1;
        float        pickupRange = 1.6f;
        float        xpReward    = 5.0f;
    };

    // Idle bob + spin for anything lying in the world, purely cosmetic.
    struct BobComponent {
        float baseY     = 0.0f;
        float amplitude = 0.25f;
        float spinSpeed = 1.5f;
        float bobSpeed  = 2.0f;
        float timer     = 0.0f;
    };

    // ------------------------------------------------------------------------
    // Survivors waiting to be rescued.
    // ------------------------------------------------------------------------
    struct SurvivorComponent {
        float rescueRange = 2.0f;
        float xpReward    = 50.0f;
    };

    // ------------------------------------------------------------------------
    // Combat plumbing
    // ------------------------------------------------------------------------
    // Damage accumulated this frame; applied (and cleared) by DamageSystem.
    struct PendingDamageComponent {
        float amount = 0.0f;
    };

    // Short scale-punch so a struck entity reads as "hit".
    struct HitFlashComponent {
        float timer    = 0.0f;
        float duration = 0.15f;
        float strength = 0.35f;
    };

    // ------------------------------------------------------------------------
    // Camera
    // ------------------------------------------------------------------------
    // Orthographic isometric follow camera. offsetDir is the (normalized) world
    // direction from the target to the eye; orthoSize is half the vertical
    // world extent shown.
    struct IsometricCameraComponent {
        entt::entity target    = entt::null;
        glm::vec3    offsetDir = glm::normalize(glm::vec3(1.0f, 1.2f, 1.0f));
        float        distance  = 30.0f;
        float        orthoSize = 12.0f;
        float        smooth    = 0.12f;// fraction-per-frame easing toward target
        glm::vec3    focus     = glm::vec3(0.0f);
    };

    // ------------------------------------------------------------------------
    // Object pooling
    //
    // The engine's renderer stages all mesh geometry once at startup; creating
    // meshes at runtime would mean re-uploading GPU buffers and reshuffling
    // material IDs. So nothing is created or destroyed mid-game. Instead, every
    // bear, pickup, and survivor is pre-built and staged up front, then toggled
    // between "in play" and "parked" (InactiveTag, hidden, moved off-map).
    // ------------------------------------------------------------------------
    enum class PoolKind { Bear, Pickup, Survivor };

    struct PooledComponent {
        PoolKind     kind = PoolKind::Bear;
        ResourceType resourceType = ResourceType::Meat;// only meaningful for Pickup
    };

    // Present == parked and out of play. Every gameplay view excludes it.
    struct InactiveTag {};

    // ------------------------------------------------------------------------
    // Singletons / tags
    // ------------------------------------------------------------------------
    struct GameStateComponent {
        float survivalTime    = 0.0f;
        bool  gameOver        = false;
        float bearSpawnTimer  = 0.0f;
        float survivorSpawnTimer = 0.0f;
        int   maxActiveBears  = 6;
        int   maxActiveSurvivors = 2;
    };

    // Marks an entity to leave play this frame: pooled entities get parked,
    // anything else is destroyed (RecycleSystem).
    struct DeadTag {};

    // Loot/XP already granted for this life — cleared when the bear is recycled.
    struct LootedTag {};

}// namespace Survival
