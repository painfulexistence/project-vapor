#pragma once

#include "Vapor/components.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/scene.hpp"
#include "components.hpp"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

// ============================================================================
// Whiteout Survival ad-game — ECS systems
//
// Each system is a stateless static `update`, matching the engine demo's
// convention. Nothing is created or destroyed at runtime: bears, pickups, and
// survivors live in pools (see Factory) and are toggled in and out of play.
// Every gameplay view therefore excludes InactiveTag.
//
// main.cpp runs these in a fixed order each frame:
//   input → movement → camera → torch → AI → player attack → damage →
//   death-loot → pickup → rescue → progression → upgrades → bob → hit-flash →
//   spawners → game-over → light-gather → recycle
// ============================================================================

namespace Survival {

    inline entt::entity findPlayer(entt::registry& reg) {
        auto view = reg.view<PlayerTag>();
        for (auto e : view) return e;
        return entt::null;
    }

    // ------------------------------------------------------------------------
    // Pool / activation helpers
    // ------------------------------------------------------------------------
    namespace Pool {
        inline entt::entity acquire(entt::registry& reg, PoolKind kind) {
            auto view = reg.view<PooledComponent, InactiveTag>();
            for (auto e : view)
                if (view.get<PooledComponent>(e).kind == kind) return e;
            return entt::null;
        }

        inline entt::entity acquirePickup(entt::registry& reg, ResourceType type) {
            auto view = reg.view<PooledComponent, InactiveTag>();
            for (auto e : view) {
                auto& p = view.get<PooledComponent>(e);
                if (p.kind == PoolKind::Pickup && p.resourceType == type) return e;
            }
            return entt::null;
        }

        inline int countActive(entt::registry& reg, PoolKind kind) {
            int n = 0;
            auto view = reg.view<PooledComponent>(entt::exclude<InactiveTag>);
            for (auto e : view)
                if (view.get<PooledComponent>(e).kind == kind) ++n;
            return n;
        }

        inline void setVisible(entt::registry& reg, entt::entity e, bool v) {
            if (auto* mr = reg.try_get<Vapor::MeshRendererComponent>(e)) mr->visible = v;
        }

        inline void park(entt::registry& reg, entt::entity e) {
            reg.emplace_or_replace<InactiveTag>(e);
            setVisible(reg, e, false);
            reg.remove<DeadTag>(e);
            reg.remove<LootedTag>(e);
            reg.remove<HitFlashComponent>(e);
            reg.remove<PendingDamageComponent>(e);
            if (auto* t = reg.try_get<Vapor::TransformComponent>(e)) {
                t->position = glm::vec3(0.0f, -1000.0f, 0.0f);
                t->scale    = glm::vec3(1.0f);
                t->isDirty  = true;
            }
        }
    }// namespace Pool

    inline void activateBear(entt::registry& reg, entt::entity e, const glm::vec3& pos) {
        reg.remove<InactiveTag>(e);
        reg.remove<DeadTag>(e);
        reg.remove<LootedTag>(e);
        reg.remove<HitFlashComponent>(e);
        Pool::setVisible(reg, e, true);
        if (auto* t = reg.try_get<Vapor::TransformComponent>(e)) {
            t->position = pos;
            t->scale    = glm::vec3(1.0f);
            t->isDirty  = true;
        }
        if (auto* h = reg.try_get<HealthComponent>(e)) h->hp = h->maxHp;
        if (auto* ai = reg.try_get<AIComponent>(e)) {
            ai->state = AIState::Wander;
            ai->cooldownTimer = 0.0f;
            ai->wanderTimer = 0.0f;
        }
    }

    inline void activatePickup(entt::registry& reg, entt::entity e, const glm::vec3& pos, int amount) {
        reg.remove<InactiveTag>(e);
        Pool::setVisible(reg, e, true);
        if (auto* t = reg.try_get<Vapor::TransformComponent>(e)) {
            t->position = pos;
            t->isDirty  = true;
        }
        if (auto* pick = reg.try_get<PickupComponent>(e)) pick->amount = amount;
        if (auto* bob = reg.try_get<BobComponent>(e)) {
            bob->baseY = pos.y;
            bob->timer = 0.0f;
        }
    }

    inline void activateSurvivor(entt::registry& reg, entt::entity e, const glm::vec3& pos) {
        reg.remove<InactiveTag>(e);
        Pool::setVisible(reg, e, true);
        if (auto* t = reg.try_get<Vapor::TransformComponent>(e)) {
            t->position = pos;
            t->scale    = glm::vec3(1.0f);
            t->isDirty  = true;
        }
    }

    // A random point on a ring around a center — used to spawn actors just
    // outside the action without dropping them on the player's head.
    inline glm::vec3 ringPoint(Vapor::RNG& rng, const glm::vec3& center, float minR, float maxR) {
        float angle = rng.RandomFloatInRange(0.0f, 6.2831853f);
        float r     = rng.RandomFloatInRange(minR, maxR);
        return center + glm::vec3(std::cos(angle) * r, 0.0f, std::sin(angle) * r);
    }

    // ------------------------------------------------------------------------
    // Camera basis — map WASD onto the iso screen axes.
    // ------------------------------------------------------------------------
    struct CameraBasis {
        glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 right   = glm::vec3(1.0f, 0.0f, 0.0f);
    };

    inline CameraBasis cameraGroundBasis(entt::registry& reg) {
        CameraBasis basis;
        auto view = reg.view<IsometricCameraComponent>();
        for (auto e : view) {
            auto& iso = view.get<IsometricCameraComponent>(e);
            glm::vec3 fwd = -glm::vec3(iso.offsetDir.x, 0.0f, iso.offsetDir.z);
            if (glm::length(fwd) > 1e-4f) basis.forward = glm::normalize(fwd);
            basis.right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), -basis.forward));
            break;
        }
        return basis;
    }

    // ------------------------------------------------------------------------
    // 1. Input
    // ------------------------------------------------------------------------
    class PlayerInputSystem {
    public:
        static void update(entt::registry& reg, const Vapor::InputState& input) {
            auto view = reg.view<PlayerControlComponent>();
            for (auto e : view) {
                auto& ctrl = view.get<PlayerControlComponent>(e);
                ctrl.moveInput = input.getVector(
                    Vapor::InputAction::StrafeLeft,
                    Vapor::InputAction::StrafeRight,
                    Vapor::InputAction::MoveBackward,
                    Vapor::InputAction::MoveForward
                );
                ctrl.attack = input.isHeld(Vapor::InputAction::Jump)
                              || input.isHeld(Vapor::InputAction::Interact);
                ctrl.upgradeWeapon = input.isPressed(Vapor::InputAction::Hotkey1);
                ctrl.upgradeArmor  = input.isPressed(Vapor::InputAction::Hotkey2);
            }
        }
    };

    // ------------------------------------------------------------------------
    // 2. Movement — camera-relative velocity into the character controller.
    // ------------------------------------------------------------------------
    class PlayerMovementSystem {
    public:
        static void update(entt::registry& reg) {
            CameraBasis basis = cameraGroundBasis(reg);
            auto view = reg.view<PlayerControlComponent, MoveStatsComponent,
                                 Vapor::CharacterBodyComponent, FacingComponent>();
            for (auto e : view) {
                auto& ctrl  = view.get<PlayerControlComponent>(e);
                auto& stats = view.get<MoveStatsComponent>(e);
                auto& body  = view.get<Vapor::CharacterBodyComponent>(e);
                auto& face  = view.get<FacingComponent>(e);

                glm::vec3 move = basis.right * ctrl.moveInput.x + basis.forward * ctrl.moveInput.y;
                if (glm::length(move) > 1e-3f) {
                    move = glm::normalize(move);
                    face.dir = move;
                    body.desiredVelocity = move * stats.moveSpeed;
                } else {
                    body.desiredVelocity = glm::vec3(0.0f);
                }
            }
        }
    };

    // ------------------------------------------------------------------------
    // 3. Camera — orthographic isometric follow.
    // ------------------------------------------------------------------------
    class IsometricCameraSystem {
    public:
        static void update(entt::registry& reg, float dt, float aspect) {
            auto view = reg.view<IsometricCameraComponent, Vapor::VirtualCameraComponent>();
            for (auto e : view) {
                auto& iso = view.get<IsometricCameraComponent>(e);
                auto& cam = view.get<Vapor::VirtualCameraComponent>(e);

                glm::vec3 targetPos = iso.focus;
                if (reg.valid(iso.target)) {
                    if (auto* t = reg.try_get<Vapor::TransformComponent>(iso.target)) targetPos = t->position;
                }
                float a = 1.0f - std::pow(iso.smooth, dt * 60.0f);
                iso.focus = glm::mix(iso.focus, targetPos, glm::clamp(a, 0.0f, 1.0f));

                glm::vec3 eye = iso.focus + glm::normalize(iso.offsetDir) * iso.distance;
                cam.position     = eye;
                cam.viewMatrix   = glm::lookAt(eye, iso.focus, glm::vec3(0, 1, 0));
                float h = iso.orthoSize;
                float w = h * aspect;
                cam.projectionMatrix = glm::orthoZO(-w, w, -h, h, 0.1f, iso.distance * 3.0f);
                cam.aspect = aspect;
            }
        }
    };

    // ------------------------------------------------------------------------
    // 4. Torch flicker
    // ------------------------------------------------------------------------
    class TorchFlickerSystem {
    public:
        static void update(entt::registry& reg, float dt) {
            auto view = reg.view<TorchComponent, PointLightComponent>();
            for (auto e : view) {
                auto& torch = view.get<TorchComponent>(e);
                auto& light = view.get<PointLightComponent>(e);
                torch.timer += dt * torch.flickerSpeed;
                float flicker = std::sin(torch.timer) * 0.6f + std::sin(torch.timer * 2.7f) * 0.4f;
                light.intensity = torch.baseIntensity + flicker * torch.flickerAmount;
            }
        }
    };

    // ------------------------------------------------------------------------
    // 5. Enemy AI
    // ------------------------------------------------------------------------
    class EnemyAISystem {
    public:
        static void update(entt::registry& reg, float dt, Vapor::RNG& rng) {
            entt::entity player = findPlayer(reg);
            glm::vec3 playerPos(0.0f);
            bool hasPlayer = false;
            if (player != entt::null) {
                if (auto* t = reg.try_get<Vapor::TransformComponent>(player)) {
                    playerPos = t->position;
                    hasPlayer = true;
                }
            }

            auto view = reg.view<EnemyTag, AIComponent, Vapor::TransformComponent>(entt::exclude<InactiveTag>);
            for (auto e : view) {
                auto& ai = view.get<AIComponent>(e);
                auto& tf = view.get<Vapor::TransformComponent>(e);
                if (ai.cooldownTimer > 0.0f) ai.cooldownTimer -= dt;

                float distToPlayer = hasPlayer ? glm::length(playerPos - tf.position) : 1e9f;

                if (hasPlayer && distToPlayer <= ai.attackRange) {
                    ai.state = AIState::Attack;
                } else if (hasPlayer && distToPlayer <= ai.detectRange) {
                    ai.state = AIState::Chase;
                } else if (ai.state == AIState::Chase || ai.state == AIState::Attack) {
                    ai.state = AIState::Wander;
                }

                switch (ai.state) {
                case AIState::Idle:
                case AIState::Wander:
                    ai.wanderTimer -= dt;
                    if (ai.wanderTimer <= 0.0f) {
                        ai.wanderTimer = rng.RandomFloatInRange(1.5f, 4.0f);
                        ai.wanderTarget = tf.position + glm::vec3(
                            rng.RandomFloatInRange(-6.0f, 6.0f), 0.0f,
                            rng.RandomFloatInRange(-6.0f, 6.0f));
                    }
                    moveToward(tf, ai.wanderTarget, ai.speed * 0.5f, dt);
                    break;
                case AIState::Chase:
                    moveToward(tf, playerPos, ai.speed, dt);
                    break;
                case AIState::Attack:
                    faceToward(tf, playerPos);
                    if (ai.cooldownTimer <= 0.0f && player != entt::null) {
                        ai.cooldownTimer = ai.attackCooldown;
                        reg.get_or_emplace<PendingDamageComponent>(player).amount += ai.attackDamage;
                    }
                    break;
                }
            }
        }

    private:
        static void faceToward(Vapor::TransformComponent& tf, const glm::vec3& target) {
            glm::vec3 dir = target - tf.position;
            dir.y = 0.0f;
            if (glm::length(dir) > 1e-4f) {
                tf.rotation = glm::angleAxis(std::atan2(dir.x, dir.z), glm::vec3(0, 1, 0));
                tf.isDirty = true;
            }
        }

        static void moveToward(Vapor::TransformComponent& tf, const glm::vec3& target, float speed, float dt) {
            glm::vec3 dir = target - tf.position;
            dir.y = 0.0f;
            float dist = glm::length(dir);
            if (dist > 0.05f) {
                dir /= dist;
                tf.position += dir * std::min(speed * dt, dist);
                tf.rotation = glm::angleAxis(std::atan2(dir.x, dir.z), glm::vec3(0, 1, 0));
                tf.isDirty = true;
            }
        }
    };

    // ------------------------------------------------------------------------
    // 6. Player attack — swing on cooldown, hit everything in the arc.
    // ------------------------------------------------------------------------
    class PlayerAttackSystem {
    public:
        static void update(entt::registry& reg, float dt) {
            auto players = reg.view<PlayerControlComponent, CombatStatsComponent,
                                    FacingComponent, Vapor::TransformComponent>();
            for (auto p : players) {
                auto& ctrl  = players.get<PlayerControlComponent>(p);
                auto& stats = players.get<CombatStatsComponent>(p);
                auto& face  = players.get<FacingComponent>(p);
                auto& ptf   = players.get<Vapor::TransformComponent>(p);

                if (stats.cooldownTimer > 0.0f) stats.cooldownTimer -= dt;
                if (!ctrl.attack || stats.cooldownTimer > 0.0f) continue;
                stats.cooldownTimer = stats.attackCooldown;

                float cosTol = std::cos(glm::radians(stats.attackArcDeg * 0.5f));
                glm::vec3 facing = glm::normalize(glm::vec3(face.dir.x, 0.0f, face.dir.z));

                auto enemies = reg.view<EnemyTag, Vapor::TransformComponent, HealthComponent>(
                    entt::exclude<InactiveTag>);
                for (auto en : enemies) {
                    auto& etf = enemies.get<Vapor::TransformComponent>(en);
                    glm::vec3 to = etf.position - ptf.position;
                    to.y = 0.0f;
                    float dist = glm::length(to);
                    if (dist > stats.attackRange || dist < 1e-4f) continue;
                    if (glm::dot(glm::normalize(to), facing) < cosTol) continue;
                    reg.get_or_emplace<PendingDamageComponent>(en).amount += stats.attackDamage;
                }
            }
        }
    };

    // ------------------------------------------------------------------------
    // 7. Damage — apply accumulated damage, spawn hit-flash, flag deaths.
    // ------------------------------------------------------------------------
    class DamageSystem {
    public:
        static void update(entt::registry& reg, float dt) {
            reg.view<HealthComponent>().each([dt](auto& h) {
                if (h.invulnTimer > 0.0f) h.invulnTimer -= dt;
            });

            std::vector<entt::entity> consumed;
            auto view = reg.view<PendingDamageComponent, HealthComponent>();
            for (auto e : view) {
                auto& dmg    = view.get<PendingDamageComponent>(e);
                auto& health = view.get<HealthComponent>(e);
                consumed.push_back(e);
                if (dmg.amount <= 0.0f) continue;

                bool isPlayer = reg.all_of<PlayerTag>(e);
                if (isPlayer && health.invulnTimer > 0.0f) continue;

                health.hp -= dmg.amount;
                if (isPlayer) health.invulnTimer = 0.4f;
                reg.emplace_or_replace<HitFlashComponent>(e);

                if (health.hp <= 0.0f) {
                    health.hp = 0.0f;
                    // The player is never destroyed — death is handled by
                    // GameOverSystem so the corpse, camera target, and HUD stay
                    // valid. Only pooled actors get flagged for recycling.
                    if (!isPlayer) reg.emplace_or_replace<DeadTag>(e);
                }
            }
            for (auto e : consumed) reg.remove<PendingDamageComponent>(e);
        }
    };

    // ------------------------------------------------------------------------
    // 8. Death loot — corpses pay XP and spawn pickups from the pool, once.
    // ------------------------------------------------------------------------
    class DeathLootSystem {
    public:
        static void update(entt::registry& reg, Vapor::RNG& rng) {
            entt::entity player = findPlayer(reg);

            // Collect corpses first — granting loot adds LootedTag (an excluded
            // component) and activates pooled pickups, so we must not mutate
            // while the view is still being iterated.
            std::vector<entt::entity> corpses;
            auto view = reg.view<DeadTag, LootTableComponent, Vapor::TransformComponent>(
                entt::exclude<LootedTag>);
            for (auto e : view) corpses.push_back(e);

            for (auto e : corpses) {
                auto& loot = reg.get<LootTableComponent>(e);
                auto& tf   = reg.get<Vapor::TransformComponent>(e);
                reg.emplace<LootedTag>(e);

                int count = rng.RandomIntInRange(loot.dropMin, loot.dropMax);
                for (int i = 0; i < count; ++i) {
                    entt::entity drop = Pool::acquirePickup(reg, loot.drop);
                    if (drop == entt::null) break;// pool exhausted, skip
                    glm::vec3 pos = tf.position + glm::vec3(
                        rng.RandomFloatInRange(-1.2f, 1.2f), 0.4f,
                        rng.RandomFloatInRange(-1.2f, 1.2f));
                    activatePickup(reg, drop, pos, 1);
                }

                if (player != entt::null) {
                    if (auto* xp = reg.try_get<ExperienceComponent>(player)) xp->xp += loot.xpReward;
                }
            }
        }
    };

    // ------------------------------------------------------------------------
    // 9. Pickups — proximity collection.
    // ------------------------------------------------------------------------
    class PickupSystem {
    public:
        static void update(entt::registry& reg) {
            entt::entity player = findPlayer(reg);
            if (player == entt::null) return;
            auto* ptf = reg.try_get<Vapor::TransformComponent>(player);
            auto* inv = reg.try_get<InventoryComponent>(player);
            auto* xp  = reg.try_get<ExperienceComponent>(player);
            if (!ptf || !inv) return;

            // Collect first: flagging DeadTag (excluded by this view) mid-iteration
            // would invalidate it.
            std::vector<entt::entity> collected;
            auto view = reg.view<PickupComponent, Vapor::TransformComponent>(entt::exclude<InactiveTag, DeadTag>);
            for (auto e : view) {
                auto& pick = view.get<PickupComponent>(e);
                auto& tf   = view.get<Vapor::TransformComponent>(e);
                glm::vec3 d = tf.position - ptf->position;
                d.y = 0.0f;
                if (glm::length(d) > pick.pickupRange) continue;

                switch (pick.type) {
                case ResourceType::Meat:  inv->meat  += pick.amount; break;
                case ResourceType::Wood:  inv->wood  += pick.amount; break;
                case ResourceType::Stone: inv->stone += pick.amount; break;
                }
                if (xp) xp->xp += pick.xpReward;
                collected.push_back(e);
            }
            for (auto e : collected) reg.emplace_or_replace<DeadTag>(e);
        }
    };

    // ------------------------------------------------------------------------
    // 10. Survivor rescue
    // ------------------------------------------------------------------------
    class SurvivorRescueSystem {
    public:
        static void update(entt::registry& reg) {
            entt::entity player = findPlayer(reg);
            if (player == entt::null) return;
            auto* ptf = reg.try_get<Vapor::TransformComponent>(player);
            auto* inv = reg.try_get<InventoryComponent>(player);
            auto* xp  = reg.try_get<ExperienceComponent>(player);
            if (!ptf || !inv) return;

            std::vector<entt::entity> rescued;
            auto view = reg.view<SurvivorComponent, Vapor::TransformComponent>(entt::exclude<InactiveTag, DeadTag>);
            for (auto e : view) {
                auto& s  = view.get<SurvivorComponent>(e);
                auto& tf = view.get<Vapor::TransformComponent>(e);
                glm::vec3 d = tf.position - ptf->position;
                d.y = 0.0f;
                if (glm::length(d) > s.rescueRange) continue;
                inv->survivors += 1;
                if (xp) xp->xp += s.xpReward;
                rescued.push_back(e);
            }
            for (auto e : rescued) reg.emplace_or_replace<DeadTag>(e);
        }
    };

    // ------------------------------------------------------------------------
    // 11. Progression — level up on banked XP.
    // ------------------------------------------------------------------------
    class ProgressionSystem {
    public:
        static void update(entt::registry& reg) {
            auto view = reg.view<PlayerTag, ExperienceComponent, CombatStatsComponent, HealthComponent>();
            for (auto e : view) {
                auto& xp     = view.get<ExperienceComponent>(e);
                auto& combat = view.get<CombatStatsComponent>(e);
                auto& health = view.get<HealthComponent>(e);
                while (xp.xp >= xp.xpToNext) {
                    xp.xp -= xp.xpToNext;
                    xp.level += 1;
                    xp.xpToNext *= 1.4f;
                    combat.attackDamage += 6.0f;
                    health.maxHp += 15.0f;
                    health.hp = health.maxHp;
                }
            }
        }
    };

    // ------------------------------------------------------------------------
    // 12. Equipment upgrades — spend resources for permanent buffs.
    // ------------------------------------------------------------------------
    class EquipmentUpgradeSystem {
    public:
        static void update(entt::registry& reg) {
            auto view = reg.view<PlayerControlComponent, EquipmentComponent, InventoryComponent,
                                 CombatStatsComponent, HealthComponent>();
            for (auto e : view) {
                auto& ctrl   = view.get<PlayerControlComponent>(e);
                auto& equip  = view.get<EquipmentComponent>(e);
                auto& inv    = view.get<InventoryComponent>(e);
                auto& combat = view.get<CombatStatsComponent>(e);
                auto& health = view.get<HealthComponent>(e);

                if (ctrl.upgradeWeapon) {
                    int costWood  = equip.weaponLevel * 3;
                    int costStone = equip.weaponLevel * 2;
                    if (inv.wood >= costWood && inv.stone >= costStone) {
                        inv.wood  -= costWood;
                        inv.stone -= costStone;
                        equip.weaponLevel += 1;
                        combat.attackDamage += 10.0f;
                        combat.attackCooldown = std::max(0.15f, combat.attackCooldown - 0.03f);
                    }
                }
                if (ctrl.upgradeArmor) {
                    int costMeat  = equip.armorLevel * 2;
                    int costStone = equip.armorLevel * 3;
                    if (inv.meat >= costMeat && inv.stone >= costStone) {
                        inv.meat  -= costMeat;
                        inv.stone -= costStone;
                        equip.armorLevel += 1;
                        float bonus = 20.0f;
                        health.maxHp += bonus;
                        health.hp = std::min(health.maxHp, health.hp + bonus);
                    }
                }
            }
        }
    };

    // ------------------------------------------------------------------------
    // 13. Cosmetic bob + spin for world pickups.
    // ------------------------------------------------------------------------
    class BobSystem {
    public:
        static void update(entt::registry& reg, float dt) {
            auto view = reg.view<BobComponent, Vapor::TransformComponent>(entt::exclude<InactiveTag>);
            for (auto e : view) {
                auto& bob = view.get<BobComponent>(e);
                auto& tf  = view.get<Vapor::TransformComponent>(e);
                bob.timer += dt;
                tf.position.y = bob.baseY + std::sin(bob.timer * bob.bobSpeed) * bob.amplitude;
                tf.rotation   = glm::angleAxis(bob.timer * bob.spinSpeed, glm::vec3(0, 1, 0));
                tf.isDirty    = true;
            }
        }
    };

    // ------------------------------------------------------------------------
    // 14. Hit flash — decay the scale-punch.
    // ------------------------------------------------------------------------
    class HitFlashSystem {
    public:
        static void update(entt::registry& reg, float dt) {
            std::vector<entt::entity> done;
            auto view = reg.view<HitFlashComponent, Vapor::TransformComponent>();
            for (auto e : view) {
                auto& flash = view.get<HitFlashComponent>(e);
                auto& tf    = view.get<Vapor::TransformComponent>(e);
                flash.timer += dt;
                float t = flash.timer / flash.duration;
                if (t >= 1.0f) {
                    done.push_back(e);
                    continue;
                }
                tf.scale   = glm::vec3(1.0f + std::sin(t * 3.14159265f) * flash.strength);
                tf.isDirty = true;
            }
            for (auto e : done) {
                if (auto* tf = reg.try_get<Vapor::TransformComponent>(e)) {
                    tf->scale  = glm::vec3(1.0f);
                    tf->isDirty = true;
                }
                reg.remove<HitFlashComponent>(e);
            }
        }
    };

    // ------------------------------------------------------------------------
    // 15. Spawners — keep the world stocked from the pools.
    // ------------------------------------------------------------------------
    class SpawnerSystem {
    public:
        static void update(entt::registry& reg, float dt, Vapor::RNG& rng) {
            entt::entity player = findPlayer(reg);
            glm::vec3 center(0.0f);
            if (player != entt::null) {
                if (auto* t = reg.try_get<Vapor::TransformComponent>(player)) center = t->position;
            }

            auto states = reg.view<GameStateComponent>();
            for (auto se : states) {
                auto& gs = states.get<GameStateComponent>(se);
                if (gs.gameOver) continue;

                gs.bearSpawnTimer -= dt;
                if (gs.bearSpawnTimer <= 0.0f && Pool::countActive(reg, PoolKind::Bear) < gs.maxActiveBears) {
                    gs.bearSpawnTimer = rng.RandomFloatInRange(2.0f, 4.0f);
                    entt::entity bear = Pool::acquire(reg, PoolKind::Bear);
                    if (bear != entt::null) {
                        glm::vec3 pos = ringPoint(rng, center, 14.0f, 22.0f);
                        pos.y = 0.7f;
                        activateBear(reg, bear, pos);
                    }
                }

                gs.survivorSpawnTimer -= dt;
                if (gs.survivorSpawnTimer <= 0.0f
                    && Pool::countActive(reg, PoolKind::Survivor) < gs.maxActiveSurvivors) {
                    gs.survivorSpawnTimer = rng.RandomFloatInRange(8.0f, 14.0f);
                    entt::entity s = Pool::acquire(reg, PoolKind::Survivor);
                    if (s != entt::null) {
                        glm::vec3 pos = ringPoint(rng, center, 10.0f, 20.0f);
                        pos.y = 0.8f;
                        activateSurvivor(reg, s, pos);
                    }
                }
            }
        }
    };

    // ------------------------------------------------------------------------
    // 16. Game-over check.
    // ------------------------------------------------------------------------
    class GameOverSystem {
    public:
        static void update(entt::registry& reg, float dt) {
            auto states = reg.view<GameStateComponent>();
            for (auto se : states) {
                auto& gs = states.get<GameStateComponent>(se);
                if (gs.gameOver) continue;
                gs.survivalTime += dt;

                entt::entity player = findPlayer(reg);
                if (player == entt::null) {
                    gs.gameOver = true;
                    continue;
                }
                if (auto* h = reg.try_get<HealthComponent>(player)) {
                    if (h->hp <= 0.0f) gs.gameOver = true;
                }
            }
        }
    };

    // ------------------------------------------------------------------------
    // 17. Light gather — publish ECS lights into the Scene.
    // ------------------------------------------------------------------------
    class LightGatherSystem {
    public:
        static void update(entt::registry& reg, Scene* scene) {
            scene->pointLights.clear();
            auto pointView = reg.view<PointLightComponent, Vapor::TransformComponent>(entt::exclude<InactiveTag>);
            for (auto e : pointView) {
                auto& light = pointView.get<PointLightComponent>(e);
                auto& tf    = pointView.get<Vapor::TransformComponent>(e);
                scene->pointLights.push_back({
                    .position  = tf.position,
                    .color     = light.color,
                    .intensity = light.intensity,
                    .radius    = light.radius,
                });
            }

            scene->directionalLights.clear();
            auto dirView = reg.view<DirectionalLightComponent>();
            for (auto e : dirView) {
                auto& light = dirView.get<DirectionalLightComponent>(e);
                scene->directionalLights.push_back({
                    .direction = light.direction,
                    .color     = light.color,
                    .intensity = light.intensity,
                });
            }
        }
    };

    // ------------------------------------------------------------------------
    // 18. Recycle — park pooled entities, destroy everything else flagged dead.
    // ------------------------------------------------------------------------
    class RecycleSystem {
    public:
        static void update(entt::registry& reg, Physics3D* physics) {
            // Snapshot the dead set first: parking removes DeadTag (the very pool
            // being iterated) and destroying entities both invalidate the view.
            std::vector<entt::entity> dead;
            for (auto e : reg.view<DeadTag>()) dead.push_back(e);

            for (auto e : dead) {
                if (reg.all_of<PooledComponent>(e)) {
                    Pool::park(reg, e);
                } else {
                    if (auto* rb = reg.try_get<Vapor::RigidbodyComponent>(e)) {
                        if (rb->body.valid()) physics->destroyBody(rb->body);
                    }
                    reg.destroy(e);
                }
            }
        }
    };

}// namespace Survival
