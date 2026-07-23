#pragma once

#include "components.hpp"
#include "input_manager.hpp"
#include "physics_3d.hpp"
#include "render_data.hpp"
#include "renderer.hpp"
#include "render_scene.hpp"
#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace Vapor {

    // ============================================================================
    // 變換系統 - 計算世界變換矩陣
    // ============================================================================
    class TransformSystem {
    public:
        static void update(entt::registry& registry) {
            auto view = registry.view<TransformComponent>();

            // 第一遍：標記所有需要更新的實體
            for (auto entity : view) {
                auto& transform = view.get<TransformComponent>(entity);

                if (transform.isDirty || transform.parent != entt::null) {
                    // 需要重新計算世界變換
                    updateWorldTransform(registry, entity, transform);
                }
            }
        }

    private:
        static void updateWorldTransform(entt::registry& registry, entt::entity e, TransformComponent& transform) {
            glm::mat4 localTransform = glm::translate(glm::mat4(1.0f), transform.position)
                                       * glm::mat4_cast(transform.rotation)
                                       * glm::scale(glm::mat4(1.0f), transform.scale);

            if (transform.parent != entt::null) {
                auto* parentTransform = registry.try_get<TransformComponent>(transform.parent);
                if (parentTransform) {
                    transform.worldTransform = parentTransform->worldTransform * localTransform;
                } else {
                    transform.worldTransform = localTransform;
                }
            } else {
                transform.worldTransform = localTransform;
            }

            transform.isDirty = false;
        }
    };

    // ============================================================================
    // 渲染系統 - 收集渲染實例
    // ============================================================================
    // NOTE: This system is commented out because it uses an outdated Mesh structure.
    // The current Mesh struct doesn't have materialID field, and the rendering
    // is handled directly by the Renderer class, not through this system.
    /*
    class RenderSystem {
    public:
        struct RenderInstance {
            glm::mat4 model;
            glm::vec4 color;
            Uint32 vertexOffset;
            Uint32 indexOffset;
            Uint32 vertexCount;
            Uint32 indexCount;
            Uint32 materialID;
            PrimitiveMode primitiveMode;
            glm::vec3 AABBMin;
            glm::vec3 AABBMax;
        };

        static void collectInstances(
            entt::registry& registry,
            std::vector<RenderInstance>& instances,
            std::unordered_map<std::shared_ptr<Material>, std::vector<std::shared_ptr<Mesh>>>& instanceBatches
        ) {
            auto view = registry.view<MeshRendererComponent>(entt::exclude<InactiveComponent>);

            for (auto entity : view) {
                auto& render = view.get<MeshRendererComponent>(entity);

                if (!render.visible || render.meshes.empty()) {
                    continue;
                }

                auto* transform = registry.try_get<TransformComponent>(entity);
                if (!transform) {
                    continue;
                }

                const glm::mat4& modelMatrix = transform->worldTransform;

                for (const auto& mesh : render.meshes) {
                    instances.push_back(
                        {
                            .model = modelMatrix,
                            .color = glm::vec4(1.0f), // Default white color since MeshRendererComponent doesn't have color field
                            .vertexOffset = mesh->vertexOffset,
                            .indexOffset = mesh->indexOffset,
                            .vertexCount = mesh->vertexCount,
                            .indexCount = mesh->indexCount,
                            .materialID = mesh->materialID,
                            .primitiveMode = mesh->primitiveMode,
                            .AABBMin = mesh->worldAABBMin,
                            .AABBMax = mesh->worldAABBMax,
                        }
                    );

                    if (mesh->material) {
                        instanceBatches[mesh->material].push_back(mesh);
                    }
                }
            }
        }
    };
    */

    // ============================================================================
    // 物理同步系統 - 同步 Transform 和 Physics
    // ============================================================================
    class PhysicsSyncSystem {
    public:
        // Scene → Physics: 同步 Kinematic/Static 物體
        static void syncToPhysics(entt::registry& registry, Physics3D* physics) {
            auto view = registry.view<RigidbodyComponent>();

            for (auto entity : view) {
                auto& phys = view.get<RigidbodyComponent>(entity);

                if (!phys.body.valid() || !phys.syncToPhysics) {
                    continue;
                }

                auto* transform = registry.try_get<TransformComponent>(entity);
                if (!transform) {
                    continue;
                }

                // 檢查運動類型
                auto motionType = physics->getMotionType(phys.body);
                if (motionType == BodyMotionType::Kinematic || motionType == BodyMotionType::Static) {
                    physics->setPosition(phys.body, transform->position);
                    physics->setRotation(phys.body, transform->rotation);
                }
            }
        }

        // Physics → Scene: 同步 Dynamic 物體
        static void syncFromPhysics(entt::registry& registry, Physics3D* physics) {
            auto view = registry.view<RigidbodyComponent>();

            for (auto entity : view) {
                auto& phys = view.get<RigidbodyComponent>(entity);

                if (!phys.body.valid() || !phys.syncFromPhysics) {
                    continue;
                }

                auto* transform = registry.try_get<TransformComponent>(entity);
                if (!transform) {
                    continue;
                }

                // 檢查運動類型
                auto motionType = physics->getMotionType(phys.body);
                if (motionType == BodyMotionType::Dynamic) {
                    transform->position = physics->getPosition(phys.body);
                    transform->rotation = physics->getRotation(phys.body);
                    transform->isDirty = true;// 標記需要重新計算世界變換
                }
            }
        }
    };

    // ============================================================================
    // 抓取系統 - 使用 ECS 設計
    // ============================================================================
    class GrabSystem {
    public:
        static bool
            tryPickup(entt::registry& registry, entt::entity grabber, Physics3D* physics, Camera* camera, float pickupRange = 5.0f) {
            auto* grabberComp = registry.try_get<GrabberComponent>(grabber);
            if (!grabberComp || grabberComp->heldEntity != entt::null) {
                return false;// 已經抓著東西了
            }

            glm::vec3 rayStart = camera->getEye();
            glm::vec3 rayDir = camera->getForward();
            glm::vec3 rayEnd = rayStart + rayDir * pickupRange;

            RaycastHit hit;
            if (!physics->raycast(rayStart, rayEnd, hit)) {
                return false;
            }

            // 從 hit.node 找到對應的 Entity
            // 這需要一個 Node -> Entity 的映射（可以存儲在 Node 中）
            // 暫時假設可以通過某種方式找到 Entity

            // 檢查是否有 GrabbableComponent
            // entt::entity hitEntity = findEntityFromNode(hit.node);
            // if (!registry.all_of<GrabbableComponent>(hitEntity)) {
            //     return false;
            // }

            // 添加 HeldComponent
            // auto& held = registry.emplace<HeldComponent>(hitEntity);
            // held.holder = grabber;
            // held.originalGravityFactor = ...;

            // 更新 PhysicsComponent
            // auto* phys = registry.try_get<PhysicsComponent>(hitEntity);
            // if (phys) {
            //     physics->setMotionType(phys->body, BodyMotionType::Kinematic);
            //     physics->setGravityFactor(phys->body, 0.0f);
            // }

            // grabberComp->heldEntity = hitEntity;

            return true;
        }

        static void update(entt::registry& registry, Physics3D* physics, Camera* camera, float deltaTime) {
            auto view = registry.view<HeldByComponent>(entt::exclude<InactiveComponent>);

            for (auto entity : view) {
                auto& held = view.get<HeldByComponent>(entity);

                if (held.holder == entt::null) {
                    continue;
                }

                // 獲取抓取者的變換
                auto* holderTransform = registry.try_get<TransformComponent>(held.holder);
                if (!holderTransform) {
                    continue;
                }

                // 計算目標位置（相機前方）
                glm::vec3 targetPos = camera->getEye() + camera->getForward() * held.holdDistance;

                // 更新物理體位置
                auto* phys = registry.try_get<RigidbodyComponent>(entity);
                if (phys && phys->body.valid()) {
                    glm::vec3 currentPos = physics->getPosition(phys->body);
                    glm::vec3 velocity = (targetPos - currentPos) / deltaTime;

                    float maxSpeed = 20.0f;
                    if (glm::length(velocity) > maxSpeed) {
                        velocity = glm::normalize(velocity) * maxSpeed;
                    }

                    physics->setLinearVelocity(phys->body, velocity);

                    // 同步到 Transform
                    auto* transform = registry.try_get<TransformComponent>(entity);
                    if (transform) {
                        transform->position = currentPos;
                        transform->isDirty = true;
                    }
                }
            }
        }
    };

    // ============================================================================
    // 光照收集系統 - gathers ECS light components into the scene's render lists
    // ============================================================================
    // The registry is the single source of truth for lights: authoring lives on
    // per-entity light components, and this system rebuilds the scene's transient
    // render lists from them every frame (they are never authored on the Scene).
    // The SunComponent-tagged directional light is gathered into index 0, which
    // the atmosphere/sky and shadow paths treat as the authoritative sun.
    class LightGatherSystem {
    public:
        static void update(entt::registry& reg, RenderScene* scene) {
            if (!scene) return;

            // Point lights: position from the transform.
            scene->pointLights.clear();
            auto pointView = reg.view<PointLightComponent, TransformComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : pointView) {
                const auto& light     = pointView.get<PointLightComponent>(entity);
                const auto& transform = pointView.get<TransformComponent>(entity);
                scene->pointLights.push_back({
                    .position  = transform.position,
                    .color     = light.color,
                    .intensity = light.intensity,
                    .radius    = light.radius,
                });
            }

            // Directional lights: the SunComponent-tagged entity (if any) is
            // emitted first so it lands at directionalLights[0] regardless of
            // registry iteration order.
            scene->directionalLights.clear();
            entt::entity sunEntity = entt::null;
            auto sunView = reg.view<DirectionalLightComponent, SunComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : sunView) { sunEntity = entity; break; }
            auto pushDirectional = [&](const DirectionalLightComponent& light) {
                scene->directionalLights.push_back({
                    .direction = light.direction,
                    .color     = light.color,
                    .intensity = light.intensity,
                });
            };
            if (sunEntity != entt::null) {
                pushDirectional(reg.get<DirectionalLightComponent>(sunEntity));
            }
            auto dirView = reg.view<DirectionalLightComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : dirView) {
                if (entity == sunEntity) continue;
                pushDirectional(dirView.get<DirectionalLightComponent>(entity));
            }

            // Spot lights: position from the transform, beam along its forward
            // axis (rotation * -Z), degree angles converted to cosines for the GPU.
            scene->spotLights.clear();
            auto spotView = reg.view<SpotLightComponent, TransformComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : spotView) {
                const auto& light     = spotView.get<SpotLightComponent>(entity);
                const auto& transform = spotView.get<TransformComponent>(entity);
                SpotLight sl{};
                sl.position  = transform.position;
                sl.direction = glm::normalize(transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f));
                sl.color     = light.color;
                sl.intensity = light.intensity;
                sl.radius    = light.radius;
                sl.cosInner  = std::cos(glm::radians(light.innerAngle));
                sl.cosOuter  = std::cos(glm::radians(light.outerAngle));
                scene->spotLights.push_back(sl);
            }

            // Rect area lights: quad axes from the transform's rotation (right =
            // +X, up = +Y), half-extents from the component size.
            scene->rectLights.clear();
            auto rectView = reg.view<RectLightComponent, TransformComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : rectView) {
                const auto& light     = rectView.get<RectLightComponent>(entity);
                const auto& transform = rectView.get<TransformComponent>(entity);
                RectLight rl{};
                rl.position       = transform.position;
                rl.right          = glm::normalize(transform.rotation * glm::vec3(1.0f, 0.0f, 0.0f));
                rl.up             = glm::normalize(transform.rotation * glm::vec3(0.0f, 1.0f, 0.0f));
                rl.halfWidth      = light.size.x * 0.5f;
                rl.halfHeight     = light.size.y * 0.5f;
                rl.color          = light.color;
                rl.intensity      = light.intensity;
                rl.useVideoTexture = light.useVideoTexture ? 1u : 0u;
                scene->rectLights.push_back(rl);
            }
        }
    };

    // ============================================================================
    // 天空系統 - resolves the SkyComponent into a SkyRenderData for the renderer
    // ============================================================================
    // The registry owns the sky description (SkyComponent). This system pushes it
    // to the renderer only when it changes (the `dirty` flag), so it never fights
    // the renderer's own atmosphere debug UI on unchanged frames. The sun stays
    // light-driven — only the sky type + atmosphere/gradient tunables flow here.
    class SkySystem {
    public:
        static void update(entt::registry& reg, IRenderer* renderer) {
            if (!renderer) return;
            auto view = reg.view<SkyComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& sky = view.get<SkyComponent>(entity);

                // Push the sky description only when it changes.
                if (sky.dirty) {
                    SkyRenderData data;
                    data.type                  = sky.type;
                    data.rayleighCoefficients  = sky.rayleighCoefficients;
                    data.rayleighScaleHeight   = sky.rayleighScaleHeight;
                    data.mieCoefficient        = sky.mieCoefficient;
                    data.mieScaleHeight        = sky.mieScaleHeight;
                    data.miePreferredDirection = sky.miePreferredDirection;
                    data.planetRadius          = sky.planetRadius;
                    data.atmosphereRadius      = sky.atmosphereRadius;
                    data.exposure              = sky.exposure;
                    data.groundColor           = sky.groundColor;
                    data.gradientZenith        = sky.gradientZenith;
                    data.gradientHorizon       = sky.gradientHorizon;
                    data.gradientGround        = sky.gradientGround;
                    data.starDensity           = sky.starDensity;
                    data.starBrightness        = sky.starBrightness;
                    data.moonColor             = sky.moonColor;
                    data.moonSize              = sky.moonSize;
                    data.moonBrightness        = sky.moonBrightness;
                    renderer->setSky(data);
                    sky.dirty = false;
                }

                // Throttled IBL rebake: a moving sun restales the captured
                // environment. Runs every frame; the decision lives here (ECS),
                // so both backends stay aligned via requestIBLUpdate().
                if (sky.iblSunThresholdDeg > 0.0f) {
                    glm::vec3 sunDir(0.0f);
                    auto sunView = reg.view<DirectionalLightComponent, SunComponent>(entt::exclude<InactiveComponent>);
                    for (auto e : sunView) {
                        sunDir = sunView.get<DirectionalLightComponent>(e).direction;
                        break;
                    }
                    if (glm::length(sunDir) > 1e-6f) {
                        sunDir = glm::normalize(sunDir);
                        const glm::vec3 last = sky._lastIblSunDir;
                        const bool firstBake = (glm::length(last) <= 1e-6f);
                        // First bake (last == 0) always counts as "moved".
                        float cosT = firstBake
                                         ? -1.0f
                                         : glm::clamp(glm::dot(sunDir, last), -1.0f, 1.0f);
                        float movedDeg = glm::degrees(std::acos(cosT));
                        if (movedDeg >= sky.iblSunThresholdDeg) {
                            if (firstBake) {
                                // The initial bake must ALWAYS happen, independent
                                // of the auto-rebake toggle: the renderer's startup
                                // bake can run before the async scene + sun are
                                // ready, leaving a stale prefilter. Re-push the sky
                                // to force one ungated bake (setSky sets
                                // iblNeedsUpdate).
                                sky.dirty = true;
                            } else {
                                // Continuous sun-tracking rebakes: gated by the
                                // renderer's auto-rebake toggle (off by default).
                                renderer->requestIBLUpdate();
                            }
                            sky._lastIblSunDir = sunDir;
                        }
                    }
                }

                break;  // singleton: the first sky entity wins
            }
        }
    };

    // ============================================================================
    // 天氣系統 - the weather state machine (clouds / dimming / precipitation / lightning)
    // ============================================================================
    // Blends the singleton WeatherComponent toward its target state and fans the
    // resolved params out to every weather-affected medium:
    //   - volumetric clouds: pushed via IRenderer::setClouds (weather OWNS the
    //     artist cloud tunables while driveClouds is set — the panel copies of
    //     those fields are overwritten each frame)
    //   - sun/moon dimming: resolved into _resolved.sunDim, consumed by
    //     TimeOfDaySystem (clouds occlude the lights, CPU side)
    //   - fog density / wind strength multipliers: consumed by
    //     VolumetricFogSystem / WindSystem at push time (the authored component
    //     values stay untouched — weather multiplies, never overwrites)
    //   - precipitation: PrecipitationComponent-tagged emitter entities are
    //     (de)activated by adding/removing InactiveComponent
    //   - lightning: LightningComponent-tagged point lights get a double-pulse
    //     intensity envelope during Thunderstorm, plus a cloud-interior glow
    // Run BEFORE TimeOfDaySystem / WindSystem / VolumetricFogSystem so they see
    // this frame's resolved weather.
    class WeatherSystem {
    public:
        static void update(entt::registry& reg, IRenderer* renderer, float deltaTime) {
            auto view = reg.view<WeatherComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& w = view.get<WeatherComponent>(entity);
                if (!w.enabled) break;

                // ── Transition blend (snapshot-on-change so chained switches
                // mid-transition continue from the currently blended values) ──
                if (static_cast<uint8_t>(w.state) != static_cast<uint8_t>(w._lastState)) {
                    const float tPrev = smoothstep01(w._blend);
                    const auto prevTarget = static_cast<WeatherState>(static_cast<uint8_t>(w._lastState));
                    w._from = mixWeatherParams(w._from, weatherParamsFor(prevTarget), tPrev);
                    w._blend = 0.0f;
                    w._lastState = static_cast<uint8_t>(w.state);
                }
                if (w._blend < 1.0f) {
                    const float step = (w.transitionSeconds > 0.0f)
                                           ? deltaTime / w.transitionSeconds : 1.0f;
                    w._blend = std::min(1.0f, w._blend + step);
                }
                const WeatherParams p =
                    mixWeatherParams(w._from, weatherParamsFor(w.state), smoothstep01(w._blend));
                w._resolved = p;

                // ── Lightning (returns the cloud-interior glow to add) ──
                const float flashGlow = updateLightning(reg, w, deltaTime);

                // ── Clouds ──
                if (renderer && w.driveClouds) {
                    CloudsRenderData clouds;
                    clouds.enabled          = true;
                    clouds.coverage         = p.cloudCoverage;
                    clouds.density          = p.cloudDensity;
                    clouds.type             = p.cloudType;
                    clouds.layerBottom      = p.cloudLayerBottom;
                    clouds.layerTop         = p.cloudLayerTop;
                    clouds.ambientIntensity = p.cloudAmbient + flashGlow;
                    clouds.cloudDim         = p.cloudDim;
                    clouds.ambientColor     = p.cloudAmbientColor;
                    renderer->setClouds(clouds);
                }

                // ── Environment dimming: the baked IBL would stay sunny under
                // an overcast deck; scale it by the resolved factor (applied at
                // shading time in the PBR pass — no rebake needed) ──
                if (renderer) renderer->setIBLIntensity(p.iblDim);

                // ── Precipitation: gate on the target state once the transition
                // is half way in, so rain starts under an already-heavy sky ──
                const bool committed = w._blend > 0.5f;
                const bool rainOn = committed && (w.state == WeatherState::Rain ||
                                                  w.state == WeatherState::Thunderstorm);
                const bool snowOn = committed && w.state == WeatherState::Snow;
                // Active camera position for camera-following emitters (rain
                // falls where the player is, not where the scene authored it).
                glm::vec3 camPos(0.0f);
                bool camValid = false;
                auto camView = reg.view<VirtualCameraComponent>(entt::exclude<InactiveComponent>);
                for (auto ce : camView) {
                    const auto& vc = camView.get<VirtualCameraComponent>(ce);
                    if (vc.isActive) { camPos = vc.position; camValid = true; break; }
                }
                auto precip = reg.view<PrecipitationComponent>();
                for (auto pe : precip) {
                    const auto& pc = precip.get<PrecipitationComponent>(pe);
                    const auto kind = pc.kind;
                    if (pc.followCamera && camValid) {
                        if (auto* tr = reg.try_get<TransformComponent>(pe)) {
                            tr->position = glm::vec3(camPos.x, camPos.y + pc.followHeight, camPos.z);
                            tr->isDirty = true;
                        }
                    }
                    const bool on = (kind == PrecipitationComponent::Kind::Rain) ? rainOn : snowOn;
                    if (on) {
                        // Wake the authored-inactive emitter and (re)start it.
                        reg.remove<InactiveComponent>(pe);
                        if (auto* em = reg.try_get<ParticleEmitterComponent>(pe)) em->emitting = true;
                    } else if (!reg.all_of<InactiveComponent>(pe)) {
                        // Graceful stop: quit spawning, let airborne particles
                        // finish falling (the entity stays active so they draw).
                        if (auto* em = reg.try_get<ParticleEmitterComponent>(pe)) em->emitting = false;
                    }
                }

                break;  // singleton: the first weather wins
            }
        }

    private:
        static float smoothstep01(float x) {
            x = glm::clamp(x, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }

        // Drives every LightningComponent-tagged point light. Returns the
        // cloud-ambient glow for this frame (lightning illuminating the cloud
        // deck from inside). Strikes use a double-pulse envelope: a bright
        // leader followed by a re-strike ~0.15 s later, both decaying fast.
        static float updateLightning(entt::registry& reg, WeatherComponent& w, float deltaTime) {
            const bool storm = w.state == WeatherState::Thunderstorm && w._blend > 0.5f;
            float intensity = 0.0f;
            float glow = 0.0f;
            if (storm) {
                w._flashAge = static_cast<float>(w._flashAge) + deltaTime;
                w._lightningTimer = static_cast<float>(w._lightningTimer) - deltaTime;
                if (static_cast<float>(w._lightningTimer) <= 0.0f) {
                    uint32_t s = w._rng;
                    if (s == 0u) s = 0x9E3779B9u;   // first-use seed
                    s ^= s << 13; s ^= s >> 17; s ^= s << 5;   // xorshift32
                    w._rng = s;
                    const float u = static_cast<float>(s & 0xFFFFFFu) / 16777216.0f;
                    const float span = std::max(0.0f, w.lightningMaxInterval - w.lightningMinInterval);
                    w._lightningTimer = w.lightningMinInterval + u * span;
                    w._flashAge = 0.0f;   // strike!
                }
                const float a = w._flashAge;
                float env = std::exp(-25.0f * a);
                if (a > 0.15f) env += 0.7f * std::exp(-25.0f * (a - 0.15f));
                if (env > 0.005f) {
                    intensity = w.lightningIntensity * env;
                    // Cloud-interior glow: huge relative to the ~0.001 ambient
                    // scale — a brief flash lighting the deck from inside.
                    glow = 1.5f * env;
                }
            } else {
                w._flashAge = 1e9f;
                w._lightningTimer = 0.0f;  // first strike lands as the storm arrives
            }
            auto lights = reg.view<PointLightComponent, LightningComponent>();
            for (auto e : lights)
                lights.get<PointLightComponent>(e).intensity = intensity;
            return glow;
        }
    };

    // The active weather singleton's resolved params, or nullptr when no
    // enabled WeatherComponent exists. Consumers (TimeOfDay/Wind/Fog systems)
    // treat nullptr as "no weather" (all multipliers 1).
    inline const WeatherParams* activeWeatherParams(entt::registry& reg) {
        auto view = reg.view<WeatherComponent>(entt::exclude<InactiveComponent>);
        for (auto e : view) {
            const auto& w = view.get<WeatherComponent>(e);
            return w.enabled ? &static_cast<const WeatherParams&>(w._resolved) : nullptr;
        }
        return nullptr;
    }

    // ============================================================================
    // 時間系統 - advances the time-of-day clock and drives the sun
    // ============================================================================
    // The single moving sun: TimeOfDaySystem turns the clock into the
    // SunComponent-tagged directional light's direction (a sun arc, latitude-
    // tilted) and its colour/intensity (warm/dim at the horizon, white at noon,
    // dark at night — the atmosphere->light coupling, CPU side). Run this BEFORE
    // LightGatherSystem so the updated sun lands in directionalLights[0].
    class TimeOfDaySystem {
    public:
        static void update(entt::registry& reg, float deltaTime) {
            auto todView = reg.view<TimeOfDayComponent>(entt::exclude<InactiveComponent>);
            entt::entity todEntity = entt::null;
            for (auto e : todView) { todEntity = e; break; }
            if (todEntity == entt::null) return;
            auto& tod = todView.get<TimeOfDayComponent>(todEntity);

            if (!tod.paused && tod.dayLengthSeconds > 0.0f) {
                tod.timeOfDay += (24.0f / tod.dayLengthSeconds) * deltaTime;
                tod.timeOfDay = std::fmod(tod.timeOfDay, 24.0f);
                if (tod.timeOfDay < 0.0f) tod.timeOfDay += 24.0f;
            }

            // Sun position: rises east (+X) at 06:00, peaks overhead at noon,
            // sets west (-X) at 18:00, below the horizon at night. Latitude tilts
            // the arc toward +Z (south).
            const float twoPi = 6.28318530718f;
            float phase   = (tod.timeOfDay / 24.0f) * twoPi;   // 0 at midnight
            float sunUp   = -std::cos(phase);                  // -1 midnight, +1 noon
            float sunEast =  std::sin(phase);                  // +1 at 06:00 (east)
            float latRad  = glm::radians(tod.latitudeDeg);
            glm::vec3 sunPos = glm::normalize(glm::vec3(
                sunEast,
                sunUp * std::cos(latRad),
                sunUp * std::sin(latRad)));

            // Radiometry from elevation (sunPos.y): dark at night, warm near the
            // horizon, white at noon.
            float e = sunPos.y;
            float daylight = glm::clamp((e + 0.1f) / 0.3f, 0.0f, 1.0f);
            daylight = daylight * daylight * (3.0f - 2.0f * daylight);  // smoothstep
            float warm = glm::clamp(e / 0.35f, 0.0f, 1.0f);
            glm::vec3 sunColor = glm::mix(glm::vec3(1.0f, 0.45f, 0.25f),
                                          glm::vec3(1.0f, 0.98f, 0.95f), warm);

            // Weather dimming: heavy cloud cover occludes the sun and moon.
            // WeatherSystem resolves the factor (runs before this system).
            float weatherDim = 1.0f;
            if (const WeatherParams* wp = activeWeatherParams(reg)) weatherDim = wp->sunDim;

            auto sunView = reg.view<DirectionalLightComponent, SunComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : sunView) {
                auto& dl = sunView.get<DirectionalLightComponent>(entity);
                dl.direction = glm::normalize(-sunPos);  // light travels away from the sun
                dl.color     = sunColor;
                dl.intensity = tod.maxSunIntensity * daylight * weatherDim;
                break;
            }

            // Moon: opposite the sun (moonPos = -sunPos), so it is up while the
            // sun is down. Intensity ramps with the moon's elevation, giving a
            // dim cool fill at night. This matches the visual moon in the
            // Atmosphere pass (moonDir = -sunDir). Direction travels away from
            // the moon (= sunPos). Unshadowed for now (PSSM tracks the sun only).
            glm::vec3 moonPos = -sunPos;
            float moonUp = glm::clamp(moonPos.y / 0.15f, 0.0f, 1.0f);
            moonUp = moonUp * moonUp * (3.0f - 2.0f * moonUp);  // smoothstep
            auto moonView = reg.view<DirectionalLightComponent, MoonComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : moonView) {
                auto& dl = moonView.get<DirectionalLightComponent>(entity);
                dl.direction = glm::normalize(-moonPos);  // = normalize(sunPos)
                dl.color     = tod.moonLightColor;
                dl.intensity = tod.maxMoonIntensity * moonUp * weatherDim;
                break;
            }
        }
    };

    // ============================================================================
    // 風場系統 - resolves the shared WindFieldComponent for the renderer
    // ============================================================================
    // Wind direction is one shared field (WindFieldComponent) — clouds, fog and
    // the particle sim all blow the same way. This pushes it to the renderer each
    // frame (wind is live/animatable). Only runs when a WindFieldComponent exists,
    // so scenes without wind keep the renderer's panel-set direction. Particles
    // read the same component directly via ParticleForceFieldSystem.
    class WindSystem {
    public:
        static void update(entt::registry& reg, IRenderer* renderer) {
            if (!renderer) return;
            auto view = reg.view<WindFieldComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                const auto& wf = view.get<WindFieldComponent>(entity);
                // Weather multiplies at push time — the authored strength stays
                // untouched, so clearing weather restores the scene's wind.
                float windMul = 1.0f;
                if (const WeatherParams* wp = activeWeatherParams(reg)) windMul = wp->windMul;
                WindRenderData data;
                data.direction  = wf.direction;
                data.strength   = wf.strength * windMul;
                data.turbulence = wf.turbulence;
                renderer->setWind(data);
                break;  // singleton: the first wind field wins
            }
        }
    };

    // ============================================================================
    // 體積霧系統 - resolves every VolumetricFogComponent for the renderer
    // ============================================================================
    // The opt-in volumetric fog (froxel grid). Unlike the old singleton path this
    // gathers ALL enabled VolumetricFogComponents into one list — global height
    // fog plus any bounded AABB banks — and pushes them each frame so the renderer
    // blends overlapping volumes in the grid. A bounded volume bakes its world
    // AABB from the entity transform here (position ± halfExtent * scale). Pushed
    // every frame (fog is live-tunable); an empty list turns the froxel pass off,
    // so removing the last fog component at runtime cleanly disables it.
    class VolumetricFogSystem {
    public:
        static void update(entt::registry& reg, IRenderer* renderer) {
            if (!renderer) return;
            std::vector<VolumetricFogVolumeData> volumes;
            // Weather multiplies fog density at push time (authored density untouched).
            float fogMul = 1.0f;
            if (const WeatherParams* wp = activeWeatherParams(reg)) fogMul = wp->fogDensityMul;
            auto view = reg.view<VolumetricFogComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                const auto& f = view.get<VolumetricFogComponent>(entity);
                if (!f.enabled) continue;
                if (static_cast<int>(volumes.size()) >= kMaxFogVolumes) break;  // grid buffer cap

                VolumetricFogVolumeData v;
                v.bounded          = f.bounded;
                v.edgeFalloff      = f.edgeFalloff;
                v.blendWeight      = f.blendWeight;
                v.density          = f.density * fogMul;   // weather density multiplier
                v.heightFalloff    = f.heightFalloff;
                v.baseHeight       = f.baseHeight;
                v.maxHeight        = f.maxHeight;
                v.albedo           = f.albedo;
                v.anisotropy       = f.anisotropy;
                v.ambientIntensity = f.ambientIntensity;
                v.noiseScale       = f.noiseScale;
                v.noiseIntensity   = f.noiseIntensity;
                v.windSpeed        = f.windSpeed;

                // Bounded volume: world AABB from the entity's world-space
                // position and scale (rotation ignored — the box is axis-aligned).
                if (f.bounded) {
                    glm::vec3 center(0.0f), scale(1.0f);
                    if (const auto* t = reg.try_get<TransformComponent>(entity)) {
                        center = t->position;
                        scale  = t->scale;
                    }
                    const glm::vec3 ext = f.halfExtent * scale;
                    v.boundsMin = center - ext;
                    v.boundsMax = center + ext;
                }
                volumes.push_back(v);
            }
            renderer->setVolumetricFogVolumes(volumes);
        }
    };

    // ============================================================================
    // 相機控制系統 — fly / follow camera rigs
    // ============================================================================
    // Intent-driven: each camera entity carries a CharacterIntent written by the
    // app's input-mapping layer. The InputManager overload is a thin adapter for
    // demos without an action-mapping layer: it synthesizes a transient intent
    // from the default actions and runs the exact same handlers.
    class CameraControlSystem {
    public:
        static void update(entt::registry& registry, float deltaTime) {
            auto view = registry.view<VirtualCameraComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& cam = view.get<VirtualCameraComponent>(entity);
                if (!cam.isActive) continue;

                if (auto* fly = registry.try_get<FlyCameraComponent>(entity)) {
                    if (auto* intent = registry.try_get<CharacterIntent>(entity)) {
                        handleFlyCamera(cam, fly, *intent, deltaTime);
                    }
                }
                if (auto* follow = registry.try_get<FollowCameraComponent>(entity)) {
                    handleFollowCamera(cam, follow, deltaTime, registry);
                }
                updateMatrices(cam);
            }
        }

        static void update(entt::registry& registry, InputManager& inputManager, float deltaTime) {
            const auto& inputState = inputManager.getInputState();
            // Local only — never stomps a CharacterIntent component the app owns.
            CharacterIntent intent;
            intent.lookVector = inputState.getVector(
                InputAction::LookLeft, InputAction::LookRight, InputAction::LookDown, InputAction::LookUp
            );
            intent.moveVector = inputState.getVector(
                InputAction::StrafeLeft, InputAction::StrafeRight, InputAction::MoveBackward, InputAction::MoveForward
            );
            intent.moveVerticalAxis = inputState.getAxis(InputAction::MoveDown, InputAction::MoveUp);

            auto view = registry.view<VirtualCameraComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& cam = view.get<VirtualCameraComponent>(entity);
                if (!cam.isActive) continue;

                if (auto* fly = registry.try_get<FlyCameraComponent>(entity)) {
                    // Z/X throttle: exponential, so holding X roughly doubles
                    // the speed every half second (Z halves it). Sticky — the
                    // adjusted moveSpeed stays until changed again.
                    float speedAxis = inputState.getAxis(InputAction::SpeedDown, InputAction::SpeedUp);
                    if (speedAxis != 0.0f)
                        fly->moveSpeed = glm::clamp(
                            fly->moveSpeed * std::exp(speedAxis * 1.5f * deltaTime), 0.5f, 2000.0f);
                    handleFlyCamera(cam, fly, intent, deltaTime);
                }
                if (auto* follow = registry.try_get<FollowCameraComponent>(entity)) {
                    handleFollowCamera(cam, follow, deltaTime, registry);
                }
                updateMatrices(cam);
            }
        }

        static entt::entity getActiveCamera(entt::registry& registry) {
            auto view = registry.view<VirtualCameraComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                if (view.get<VirtualCameraComponent>(entity).isActive) {
                    return entity;
                }
            }
            return entt::null;
        }

    private:
        static void handleFlyCamera(
            VirtualCameraComponent& cam, FlyCameraComponent* fly, const CharacterIntent& intent, float deltaTime
        ) {
            // Rotation (lookVector: +x = look right, +y = look up)
            fly->pitch -= intent.lookVector.y * fly->rotateSpeed * deltaTime;
            fly->yaw -= intent.lookVector.x * fly->rotateSpeed * deltaTime;
            fly->pitch = glm::clamp(fly->pitch, -89.0f, 89.0f);

            cam.rotation = glm::quat(glm::vec3(glm::radians(-fly->pitch), glm::radians(fly->yaw - 90.0f), 0.0f));

            glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);// -Z is forward
            glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
            glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);

            // Movement (moveVector: +x = strafe right, +y = forward)
            float speed = fly->moveSpeed * deltaTime;
            if (intent.moveVector.x != 0.0f) cam.position += intent.moveVector.x * right * speed;
            if (intent.moveVector.y != 0.0f) cam.position += intent.moveVector.y * front * speed;
            if (intent.moveVerticalAxis != 0.0f) cam.position += intent.moveVerticalAxis * up * speed;
        }

        static void handleFollowCamera(VirtualCameraComponent& cam, FollowCameraComponent* follow, float deltaTime, entt::registry& registry) {
            if (follow->target == entt::null || !registry.valid(follow->target)) return;

            // Get target's transform component
            auto* targetTransform = registry.try_get<TransformComponent>(follow->target);
            if (!targetTransform) return;

            // Simple follow logic: Target Position + Offset
            glm::vec3 targetPos = targetTransform->position;
            glm::vec3 desiredPos = targetPos + follow->offset;

            // Smooth lerp
            cam.position = glm::mix(cam.position, desiredPos, 1.0f - pow(follow->smoothFactor, deltaTime));

            // Look at target
            cam.rotation = glm::quatLookAt(glm::normalize(targetPos - cam.position), glm::vec3(0, 1, 0));
        }

        static void updateMatrices(VirtualCameraComponent& cam) {
            // View Matrix
            glm::mat4 rotation = glm::mat4_cast(cam.rotation);
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
            cam.viewMatrix = glm::inverse(translation * rotation);

            // Projection Matrix
            cam.projectionMatrix = glm::perspective(cam.fov, cam.aspect, cam.near, cam.far);
        }
    };

    // ============================================================================
    // 粒子力場系統 - 收集 ECS attractor + wind 並上傳至 Renderer
    // ============================================================================
    class ParticleForceFieldSystem {
    public:
        static void update(entt::registry& registry, IRenderer* renderer) {
            if (!renderer) return;

            ParticleForceField field;

            auto attrView = registry.view<ParticleAttractorComponent, TransformComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : attrView) {
                const auto& t = attrView.get<TransformComponent>(entity);
                const auto& a = attrView.get<ParticleAttractorComponent>(entity);
                ParticleAttractor pa;
                pa.position = t.position;
                pa.strength = a.strength;
                field.attractors.push_back(pa);
                if (field.attractors.size() >= MAX_PARTICLE_ATTRACTORS) break;
            }

            auto windView = registry.view<WindFieldComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : windView) {
                const auto& w = windView.get<WindFieldComponent>(entity);
                // Same weather multiplier WindSystem applies for clouds/fog, so
                // particles feel the storm's wind too.
                float windMul = 1.0f;
                if (const WeatherParams* wp = activeWeatherParams(registry)) windMul = wp->windMul;
                field.wind       = glm::vec4(w.direction, w.strength * windMul);
                field.turbulence = w.turbulence;
                break;
            }

            renderer->setParticleForceField(field);
        }
    };

    // ============================================================================
    // 粒子發射器系統 - 累積器式發射、ring-buffer 複寫
    // ============================================================================
    class ParticleEmitterSystem {
    public:
        // emissionEnabled == false is a graceful global stop: no emitter spawns
        // new particles, but existing particles keep living and simulating, and
        // one-shot slot reclamation still runs.
        static void update(entt::registry& registry, IRenderer* renderer, float deltaTime,
                           bool emissionEnabled = true) {
            if (!renderer) return;

            static std::mt19937 rng(std::random_device{}());
            static std::uniform_real_distribution<float> u01(0.0f, 1.0f);

            auto view = registry.view<ParticleEmitterComponent, TransformComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& emit = view.get<ParticleEmitterComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);

                // enabled == false is an IMMEDIATE clear: the emitter and its
                // particles are gone now, regardless of lifetime/age. This is the
                // intuitive on/off for a boolean toggle, and the only thing that
                // works uniformly — a lifetime-based drain can never clear
                // immortal particles. Reset so a re-enable starts fresh (and
                // re-fires a one-shot). enabled is still read, never written.
                if (!emit.enabled) {
                    if (emit._slotBegin != ~0u) {
                        renderer->releaseParticleSlots(emit._slotBegin, emit._slotCount);
                        emit._slotBegin = ~0u;
                        emit._slotCount = 0;
                    }
                    emit._ringCursor   = 0;
                    emit._accumulator  = 0.0f;
                    emit._reclaimTimer = -1.0f;
                    emit._hasFired     = false;
                    emit._cleared      = true;
                    continue;
                }

                // Auto-reclaim (graceful, natural death): a finite emitter that
                // has stopped producing — a fired one-shot — frees + zero-clears
                // its slots once its particles have all aged out. Immortal
                // particles never age out, so their slots are kept until the
                // emitter is disabled (immediate clear) or destroyed.
                if (emit._reclaimTimer >= 0.0f) {
                    emit._reclaimTimer = emit._reclaimTimer - deltaTime;
                    if (emit._reclaimTimer <= 0.0f) {
                        if (emit._slotBegin != ~0u)
                            renderer->releaseParticleSlots(emit._slotBegin, emit._slotCount);
                        emit._slotBegin    = ~0u;
                        emit._slotCount    = 0;
                        emit._ringCursor   = 0;
                        emit._reclaimTimer = -1.0f;
                        emit._cleared      = true;
                    }
                }

                // Graceful global stop: keep slots and let particles keep aging,
                // just don't spawn. Re-enabling emission resumes into the slots.
                if (!emissionEnabled) continue;

                // Graceful per-emitter stop (Stop semantic): emitting == false stops
                // spawning and lets the in-flight particles finish. Arm the reclaim
                // so finite particles' slots are freed once they've aged out;
                // immortal particles stay until the emitter is disabled or resumed.
                if (!emit.emitting) {
                    if (emit._slotBegin != ~0u && emit._reclaimTimer < 0.0f
                        && emit.particleLifetime >= 0.0f)
                        emit._reclaimTimer = emit.particleLifetime;
                    continue;
                }

                // A fired one-shot has nothing more to spawn.
                if (emit.oneShot && emit._hasFired) continue;

                // Actively emitting — cancel any pending graceful-stop drain so a
                // resumed emitter isn't reclaimed out from under itself.
                emit._reclaimTimer = -1.0f;

                // Claim or re-claim slots when maxParticles changed at runtime.
                if (emit._slotBegin == ~0u || emit._slotCount != emit.maxParticles) {
                    if (emit._slotBegin != ~0u)
                        renderer->releaseParticleSlots(emit._slotBegin, emit._slotCount);
                    emit._slotCount  = emit.maxParticles;
                    emit._slotBegin  = renderer->claimParticleSlots(emit._slotCount);
                    emit._ringCursor = 0;
                    emit._accumulator = 0.0f;
                    if (emit._slotBegin == ~0u) continue; // pool full
                    emit._cleared = false; // slots claimed → particles about to exist
                }

                uint32_t spawns;
                if (emit.oneShot) {
                    // Emit the whole batch in one frame.
                    spawns = static_cast<uint32_t>(emit._slotCount);
                } else {
                    emit._accumulator += emit.emitRate * deltaTime;
                    spawns = static_cast<uint32_t>(emit._accumulator);
                    if (spawns == 0) continue;
                    emit._accumulator -= static_cast<float>(spawns);
                    spawns = std::min(spawns, static_cast<uint32_t>(emit._slotCount));
                }

                // Sample random cone directions around emitDirection
                glm::vec3 fwd = glm::normalize(emit.emitDirection);
                glm::vec3 right = glm::abs(fwd.x) < 0.9f
                    ? glm::normalize(glm::cross(fwd, glm::vec3(1, 0, 0)))
                    : glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
                glm::vec3 up = glm::cross(fwd, right);

                const bool boxEmit = emit.emitExtents.x > 0.0f || emit.emitExtents.y > 0.0f ||
                                     emit.emitExtents.z > 0.0f;
                std::vector<GPUParticleData> batch(spawns);
                for (uint32_t i = 0; i < spawns; ++i) {
                    float theta  = u01(rng) * 2.0f * 3.14159265f;
                    float phi    = u01(rng) * emit.spread;
                    glm::vec3 dir = glm::normalize(fwd * std::cos(phi)
                        + (right * std::cos(theta) + up * std::sin(theta)) * std::sin(phi));

                    GPUParticleData& p = batch[i];
                    p.position = t.position;
                    if (boxEmit) {
                        // Area emission: uniform point inside the half-extent box
                        // (rain/snow sheets — direction still follows the cone).
                        p.position += glm::vec3(
                            (u01(rng) * 2.0f - 1.0f) * emit.emitExtents.x,
                            (u01(rng) * 2.0f - 1.0f) * emit.emitExtents.y,
                            (u01(rng) * 2.0f - 1.0f) * emit.emitExtents.z);
                    }
                    p.lifetime = emit.particleLifetime;
                    p.age      = 0.0f;
                    p.velocity = dir * emit.speed;
                    p.force    = glm::vec3(0.0f);
                    p.killPlaneY = emit.groundKillY;
                    p.color    = emit.color;
                }

                if (emit.oneShot) {
                    // Mark fired internally — never touch enabled (gameplay owns
                    // it). Arm the drain so slots are reclaimed after the batch
                    // ages out; immortal particles (lifetime < 0) are kept.
                    emit._hasFired = true;
                    if (emit.particleLifetime >= 0.0f)
                        emit._reclaimTimer = emit.particleLifetime;
                }

                // Ring-buffer write: record slot BEFORE advancing cursor
                uint32_t writeSlot = emit._slotBegin + emit._ringCursor;
                // Handle wrap-around by splitting into at most two uploads
                uint32_t remaining = emit._slotCount - emit._ringCursor;
                if (spawns <= remaining) {
                    renderer->uploadParticles(writeSlot, batch);
                    emit._ringCursor = (emit._ringCursor + spawns) % emit._slotCount;
                } else {
                    // Split: first part fills to end, second part wraps
                    std::vector<GPUParticleData> part1(batch.begin(), batch.begin() + remaining);
                    std::vector<GPUParticleData> part2(batch.begin() + remaining, batch.end());
                    renderer->uploadParticles(writeSlot, part1);
                    renderer->uploadParticles(emit._slotBegin, part2);
                    emit._ringCursor = static_cast<uint32_t>(part2.size());
                }
            }
        }

        // Wire slot cleanup to component destruction. Call once at startup. The
        // renderer is stashed in the registry context so the EnTT destroy
        // callback (which only receives registry + entity) can reach it.
        static void attach(entt::registry& registry, IRenderer* renderer) {
            registry.ctx().emplace<IRenderer*>(renderer);
            registry.on_destroy<ParticleEmitterComponent>()
                    .connect<&ParticleEmitterSystem::onDestroy>();
        }

        // Destroying an emitter entity (or removing the component) returns its
        // slots to the pool and zero-clears them, so gameplay owns the emitter's
        // lifecycle end-to-end. Fires before the component is erased, so get() is
        // valid here.
        static void onDestroy(entt::registry& registry, entt::entity entity) {
            auto* rptr = registry.ctx().find<IRenderer*>();
            if (!rptr || !*rptr) return;
            auto& emit = registry.get<ParticleEmitterComponent>(entity);
            if (emit._slotBegin != ~0u)
                (*rptr)->releaseParticleSlots(emit._slotBegin, emit._slotCount);
        }
    };

    // ============================================================================
    // 粒子渲染收集系統 - 每個 emitter 一個 draw packet(per-material draws)
    // ============================================================================
    // The renderer-module gather (Niagara-style split): emission decides where
    // particles are born, ParticleRendererComponent decides how they look. Each
    // slot-holding emitter becomes one ParticleDrawPacket (blend + texture +
    // size); emitters without the component draw with defaults. Runs after
    // ParticleEmitterSystem so slot ranges are current.
    class ParticleRenderSystem {
    public:
        static void update(entt::registry& registry, IRenderer* renderer) {
            if (!renderer) return;

            std::vector<ParticleDrawPacket> draws;
            auto view = registry.view<ParticleEmitterComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                const auto& emit = view.get<ParticleEmitterComponent>(entity);
                if (emit._slotBegin == ~0u || emit._slotCount == 0) continue;

                ParticleDrawPacket p;
                p.slotBegin = emit._slotBegin;
                p.slotCount = emit._slotCount;
                if (const auto* r = registry.try_get<ParticleRendererComponent>(entity)) {
                    p.blendMode = static_cast<Uint8>(r->blendMode);
                    p.texture   = r->texture;
                    p.size      = r->size;
                    p.velocityStretch = r->velocityStretch;
                }
                draws.push_back(p);
                if (draws.size() >= MAX_PARTICLE_DRAWS) break;
            }
            renderer->setParticleDrawList(draws);
        }
    };

    // ============================================================================
    // 粒子爆發系統 - 處理一次性爆發請求
    // ============================================================================
    class ParticleBurstSystem {
    public:
        static void update(entt::registry& registry, IRenderer* renderer) {
            if (!renderer) return;

            static std::mt19937 rng(std::random_device{}());
            static std::uniform_real_distribution<float> u01(0.0f, 1.0f);

            auto view = registry.view<ParticleBurstRequest>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                const auto& req = view.get<ParticleBurstRequest>(entity);
                auto* t = registry.try_get<TransformComponent>(entity);
                glm::vec3 origin = t ? t->position : glm::vec3(0.0f);

                // Reuse the emitter's slot range if available, else claim a fresh range
                auto* emit = registry.try_get<ParticleEmitterComponent>(entity);
                uint32_t slotBegin = ~0u;
                uint32_t slotCount = req.count;
                if (emit && emit->_slotBegin != ~0u) {
                    slotBegin = emit->_slotBegin;
                    slotCount = std::min(req.count, static_cast<uint32_t>(emit->_slotCount));
                } else {
                    slotBegin = renderer->claimParticleSlots(slotCount);
                }
                if (slotBegin == ~0u) {
                    registry.remove<ParticleBurstRequest>(entity);
                    continue;
                }

                std::vector<GPUParticleData> batch(slotCount);
                for (uint32_t i = 0; i < slotCount; ++i) {
                    // Uniform sphere sampling
                    float cosTheta = 2.0f * u01(rng) - 1.0f;
                    float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
                    float phi      = u01(rng) * 2.0f * 3.14159265f;
                    glm::vec3 dir(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));

                    GPUParticleData& p = batch[i];
                    p.position = origin;
                    p.lifetime = req.lifetime;
                    p.age      = 0.0f;
                    p.velocity = dir * req.speed;
                    p.force    = glm::vec3(0.0f);
                    p.color    = req.color;
                }
                renderer->uploadParticles(slotBegin, batch);
                registry.remove<ParticleBurstRequest>(entity);
            }
        }
    };

    // ============================================================================
    // 法術彈系統 - 二次貝塞爾弧線移動 + curl noise 流體軌跡
    // ============================================================================
    class SpellBoltSystem {
    public:
        static void update(entt::registry& registry, IRenderer* renderer, float deltaTime) {
            auto view = registry.view<SpellBoltComponent, TransformComponent>(entt::exclude<InactiveComponent>);
            for (auto entity : view) {
                auto& bolt = view.get<SpellBoltComponent>(entity);
                auto& t    = view.get<TransformComponent>(entity);

                float totalDist = glm::length(bolt.target - bolt.origin);
                if (totalDist < 0.001f) {
                    registry.remove<SpellBoltComponent>(entity);
                    continue;
                }
                bolt._progress += (bolt.speed / totalDist) * deltaTime;

                // Mid-point of the quadratic Bezier arc (raised by arcHeight)
                glm::vec3 mid = (bolt.origin + bolt.target) * 0.5f
                                + glm::vec3(0.0f, bolt.arcHeight, 0.0f);
                float s = glm::clamp(static_cast<float>(bolt._progress), 0.0f, 1.0f);

                // Bezier position
                t.position = (1.0f - s) * (1.0f - s) * bolt.origin
                           + 2.0f * (1.0f - s) * s * mid
                           + s * s * bolt.target;
                t.isDirty = true;

                // Bezier tangent → emitter direction (fluid feel from curl noise)
                glm::vec3 tan = glm::normalize(
                    2.0f * (1.0f - s) * (mid - bolt.origin)
                    + 2.0f * s * (bolt.target - mid));
                if (auto* emit = registry.try_get<ParticleEmitterComponent>(entity)) {
                    emit->emitDirection = tan;
                }

                // On arrival: burst and remove the bolt
                if (bolt._progress >= 1.0f) {
                    glm::vec4 boltColor = glm::vec4(0.4f, 0.8f, 1.0f, 1.0f);
                    if (auto* emit = registry.try_get<ParticleEmitterComponent>(entity))
                        boltColor = emit->color;
                    registry.emplace_or_replace<ParticleBurstRequest>(entity,
                        ParticleBurstRequest{ 80, 4.0f, 3.14159f, 0.8f, boltColor });
                    if (auto* emit = registry.try_get<ParticleEmitterComponent>(entity))
                        emit->enabled = false;
                    registry.remove<SpellBoltComponent>(entity);
                }
            }
        }
    };

}// namespace Vapor
