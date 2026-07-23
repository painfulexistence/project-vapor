#pragma once
#include "Vapor/character_controller.hpp"
#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/fsm.hpp"
#include "Vapor/fsm_system.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/resource_manager.hpp"
#include "Vapor/rmlui_manager.hpp"
#include "components.hpp"
#include "pages/chapter_title_page.hpp"
#include "pages/page_system.hpp"
#include "pages/scroll_text_page.hpp"
#include "pages/subtitle_page.hpp"
#include <algorithm>
#include <fmt/core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <unordered_map>




class CleanupSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<DeadTag>();
        for (auto entity : view) {
            reg.destroy(entity);
        }
    }
};

// Flipbook playback now lives in the engine (Vapor::FlipbookSystem, driven by
// shared FlipbookClips in the AnimationClipLibrary) — the game loop calls it
// directly. The old per-entity frameIndices system was retired with the
// handle-based FlipbookComponent.

class Sprite2DRenderSystem {
public:
    static void update(
        entt::registry& reg,
        IRenderer* renderer,
        Vapor::ResourceManager* resourceManager
    ) {
        // Collect visible sprites
        std::vector<std::tuple<glm::mat4, Vapor::Sprite2DComponent*, entt::entity>> sprites;

        auto view = reg.view<Vapor::TransformComponent, Vapor::Sprite2DComponent>(
            entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& sprite = view.get<Vapor::Sprite2DComponent>(entity);
            if (!sprite.visible || !sprite.atlas.valid()) continue;

            auto& transform = view.get<Vapor::TransformComponent>(entity);
            sprites.push_back({transform.worldTransform, &sprite, entity});
        }

        // Sort by layer, then order
        std::sort(sprites.begin(), sprites.end(), [](const auto& a, const auto& b) {
            auto* sa = std::get<1>(a);
            auto* sb = std::get<1>(b);
            if (sa->sortingLayer != sb->sortingLayer) return sa->sortingLayer < sb->sortingLayer;
            return sa->orderInLayer < sb->orderInLayer;
        });

        // Render
        for (auto& [worldTransform, sprite, entity] : sprites) {
            const auto* atlas = resourceManager->getAtlas(sprite->atlas);
            if (!atlas) continue;

            const auto* frame = atlas->getFrame(sprite->frameIndex);
            if (!frame) continue;

            // Build sprite transform with pivot offset
            glm::vec2 pivotOffset = (sprite->pivot - glm::vec2(0.5f)) * sprite->size;
            glm::mat4 spriteTransform = worldTransform;
            spriteTransform = glm::translate(spriteTransform, glm::vec3(-pivotOffset, 0.0f));
            spriteTransform = glm::scale(spriteTransform, glm::vec3(sprite->size, 1.0f));

            // Handle flip
            glm::vec4 uv = frame->uvRect;
            if (sprite->flipX) std::swap(uv.x, uv.z);
            if (sprite->flipY) std::swap(uv.y, uv.w);

            // Convert uvRect to texCoords array
            glm::vec2 texCoords[4] = {
                {uv.x, uv.w},  // bottom-left
                {uv.z, uv.w},  // bottom-right
                {uv.z, uv.y},  // top-right
                {uv.x, uv.y}   // top-left
            };

            renderer->drawQuad2D(spriteTransform, atlas->texture, texCoords, sprite->tint, static_cast<int>(entity));
        }
    }
};

// World-space sprites (Sprite3DComponent) drawn into the 3D batch. billboard
// sprites re-orient to face the active camera each frame; the rest use their
// entity transform. Runs before beginFrame like the 2D path — drawQuad3D only
// queues into the 3D batch, which the frame flushes.
class Sprite3DRenderSystem {
public:
    static void update(
        entt::registry& reg,
        IRenderer* renderer,
        Vapor::ResourceManager* resourceManager
    ) {
        // Camera basis for billboarding: the active camera's world-space right
        // and up are the first two rows of the view rotation. Identity fallback
        // keeps non-billboard sprites correct even with no camera.
        glm::vec3 camRight(1.0f, 0.0f, 0.0f);
        glm::vec3 camUp(0.0f, 1.0f, 0.0f);
        const entt::entity camEntity = Vapor::CameraControlSystem::getActiveCamera(reg);
        if (camEntity != entt::null && reg.all_of<Vapor::VirtualCameraComponent>(camEntity)) {
            const glm::mat4& view = reg.get<Vapor::VirtualCameraComponent>(camEntity).viewMatrix;
            camRight = glm::vec3(view[0][0], view[1][0], view[2][0]);
            camUp = glm::vec3(view[0][1], view[1][1], view[2][1]);
        }

        auto view = reg.view<Vapor::TransformComponent, Vapor::Sprite3DComponent>(
            entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& sprite = view.get<Vapor::Sprite3DComponent>(entity);
            if (!sprite.visible || !sprite.atlas.valid()) continue;
            const auto* atlas = resourceManager->getAtlas(sprite.atlas);
            if (!atlas) continue;
            const auto* frame = atlas->getFrame(sprite.frameIndex);
            if (!frame) continue;

            auto& transform = view.get<Vapor::TransformComponent>(entity);
            glm::mat4 quadTransform;
            if (sprite.billboard) {
                // Camera-facing quad at the entity's world position. The batch's
                // local quad spans -0.5..+0.5, so the basis axes carry the full
                // size (±0.5 * size along camera right/up).
                const glm::vec3 pos = glm::vec3(transform.worldTransform[3]);
                quadTransform = glm::mat4(1.0f);
                quadTransform[0] = glm::vec4(camRight * sprite.size.x, 0.0f);
                quadTransform[1] = glm::vec4(camUp * sprite.size.y, 0.0f);
                quadTransform[3] = glm::vec4(pos, 1.0f);
            } else {
                // Oriented by the entity transform (poster/decal), sized in the
                // entity's local XY plane.
                quadTransform = glm::scale(transform.worldTransform, glm::vec3(sprite.size, 1.0f));
            }

            glm::vec4 uv = frame->uvRect;
            if (sprite.flipX) std::swap(uv.x, uv.z);
            if (sprite.flipY) std::swap(uv.y, uv.w);
            glm::vec2 texCoords[4] = {
                {uv.x, uv.w},  // bottom-left
                {uv.z, uv.w},  // bottom-right
                {uv.z, uv.y},  // top-right
                {uv.x, uv.y}   // top-left
            };

            renderer->drawQuad3D(quadTransform, atlas->texture, texCoords, sprite.tint, static_cast<int>(entity));
        }
    }
};

class Text2DRenderSystem {
public:
    // fontCache maps font path -> handle, owned by the app (no globals). The
    // handle is created on first use at the component's fontSize; later
    // entities sharing the path reuse it.
    static void update(
        entt::registry& reg,
        IRenderer* renderer,
        std::unordered_map<std::string, FontHandle>& fontCache
    ) {
        auto view = reg.view<Vapor::TransformComponent, Vapor::Text2DComponent>(
            entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            const auto& text = view.get<Vapor::Text2DComponent>(entity);
            if (!text.visible || text.text.empty() || text.font.empty()) continue;

            auto [it, inserted] = fontCache.try_emplace(text.font);
            if (inserted) {
                it->second = renderer->loadFont(text.font, text.fontSize);
            }
            if (!it->second.isValid()) continue;

            const auto& transform = view.get<Vapor::TransformComponent>(entity);
            renderer->drawText2D(
                it->second, text.text, glm::vec2(transform.worldTransform[3]), text.scale, text.color
            );
        }
    }
};

class Shape2DRenderSystem {
public:
    static void update(entt::registry& reg, IRenderer* renderer) {
        auto view = reg.view<Vapor::TransformComponent, Vapor::Shape2DComponent>(
            entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            const auto& shape = view.get<Vapor::Shape2DComponent>(entity);
            if (!shape.visible) continue;
            const glm::vec2 pos = glm::vec2(view.get<Vapor::TransformComponent>(entity).worldTransform[3]);
            using Kind = Vapor::Shape2DComponent::Kind;
            switch (shape.kind) {
            case Kind::Quad: renderer->drawQuad2D(pos, shape.size, shape.color); break;
            case Kind::Rect: renderer->drawRect2D(pos, shape.size, shape.color, shape.thickness); break;
            case Kind::Circle: renderer->drawCircleFilled2D(pos, shape.radius, shape.color); break;
            case Kind::Triangle:
                renderer->drawTriangleFilled2D(pos, pos + shape.p1, pos + shape.p2, shape.color);
                break;
            }
        }
    }
};

// Rewrites the text of every {FpsTextComponent, Text2DComponent} entity with
// the current frame rate — the dynamic-text counterpart of the static JSON-
// authored labels.
class FpsTextSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        auto view = reg.view<FpsTextComponent, Vapor::Text2DComponent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            view.get<Vapor::Text2DComponent>(entity).text =
                fmt::format("FPS: {:.1f}", deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f);
        }
    }
};

class BodyCreateSystem {
public:
    static void update(entt::registry& reg, Physics3D* physics) {
        // TODO: batch create
        auto boxView = reg.view<Vapor::RigidbodyComponent, Vapor::TransformComponent, Vapor::BoxColliderComponent>();
        for (auto entity : boxView) {
            auto& transform = boxView.get<Vapor::TransformComponent>(entity);
            auto& rb = boxView.get<Vapor::RigidbodyComponent>(entity);
            auto& col = boxView.get<Vapor::BoxColliderComponent>(entity);
            if (rb.body.valid()) continue;
            rb.body = physics->createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
            physics->addBody(rb.body, true);
        }
        auto sphereView =
            reg.view<Vapor::RigidbodyComponent, Vapor::TransformComponent, Vapor::SphereColliderComponent>();
        for (auto entity : sphereView) {
            auto& transform = sphereView.get<Vapor::TransformComponent>(entity);
            auto& rb = sphereView.get<Vapor::RigidbodyComponent>(entity);
            auto& col = sphereView.get<Vapor::SphereColliderComponent>(entity);
            if (rb.body.valid()) continue;
            rb.body = physics->createSphereBody(col.radius, transform.position, transform.rotation, rb.motionType);
            physics->addBody(rb.body, true);
        }
    }
};

class BodyDestroySystem {
public:
    static void update(entt::registry& reg, Physics3D* physics) {
        auto view = reg.view<Vapor::RigidbodyComponent, DeadTag>();
        for (auto entity : view) {
            auto& rb = view.get<Vapor::RigidbodyComponent>(entity);
            if (!rb.body.valid()) continue;
            physics->destroyBody(rb.body);
        }
    }
};

class LightMovementSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        auto pointView = reg.view<Vapor::PointLightComponent, Vapor::TransformComponent, LightMovementLogicComponent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : pointView) {
            auto& transform = pointView.get<Vapor::TransformComponent>(entity);
            auto& logic     = pointView.get<LightMovementLogicComponent>(entity);

            logic.timer += deltaTime * logic.speed;

            float x = 0.0f, y = 0.0f, z = 0.0f;
            switch (logic.pattern) {
            case MovementPattern::Circle:
                x = cos(logic.timer) * logic.radius;
                z = sin(logic.timer) * logic.radius;
                y = logic.height;
                break;
            case MovementPattern::Figure8:
                x = cos(logic.timer) * logic.radius;
                z = sin(logic.timer * 2.0f) * (logic.radius * 0.5f);
                y = logic.height;
                break;
            case MovementPattern::Linear:
                x = sin(logic.timer) * logic.radius;
                y = logic.height;
                z = 0.0f;
                break;
            case MovementPattern::Spiral:
                x = cos(logic.timer) * (logic.radius + sin(logic.timer * 0.5f));
                z = sin(logic.timer) * (logic.radius + sin(logic.timer * 0.5f));
                y = logic.height + sin(logic.timer * 0.2f);
                break;
            }

            transform.position = glm::vec3(x, y, z);
            transform.isDirty  = true;
        }

        auto dirView = reg.view<Vapor::DirectionalLightComponent, DirectionalLightLogicComponent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : dirView) {
            auto& light = dirView.get<Vapor::DirectionalLightComponent>(entity);
            auto& logic = dirView.get<DirectionalLightLogicComponent>(entity);

            logic.timer += deltaTime * logic.speed;
            glm::vec3 newDir = logic.baseDirection;
            newDir.z += logic.magnitude * sin(logic.timer);
            light.direction = glm::normalize(newDir);
        }
    }
};

// LightGatherSystem now lives in the engine (Vapor::LightGatherSystem); the
// game calls it directly. It gathers the SunComponent-tagged directional light
// into directionalLights[0].


class AutoRotateSystem {
public:
    static void update(entt::registry& registry, float deltaTime) {
        auto view = registry.view<Vapor::TransformComponent, AutoRotateComponent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& transform = view.get<Vapor::TransformComponent>(entity);
            auto& rotate = view.get<AutoRotateComponent>(entity);
            glm::quat delta = glm::angleAxis(rotate.speed * deltaTime, glm::normalize(rotate.axis));
            transform.rotation = glm::normalize(delta * transform.rotation);
            transform.isDirty = true;
        }
    }
};

class CameraSwitchSystem {
public:
    static void update(entt::registry& reg, entt::entity global) {
        if (auto* request = reg.try_get<CameraSwitchRequest>(global)) {
            switch (request->mode) {
            case CameraSwitchRequest::Mode::Free: {
                auto view = reg.view<Vapor::VirtualCameraComponent>(entt::exclude<Vapor::InactiveComponent>);
                view.each([&](auto entity, auto& cam) { cam.isActive = reg.all_of<FlyCameraComponent>(entity); });
                break;
            }
            case CameraSwitchRequest::Mode::Follow: {
                auto view = reg.view<Vapor::VirtualCameraComponent>(entt::exclude<Vapor::InactiveComponent>);
                view.each([&](auto entity, auto& cam) { cam.isActive = reg.all_of<FollowCameraComponent>(entity); });
                break;
            }
            case CameraSwitchRequest::Mode::FirstPerson: {
                auto view = reg.view<Vapor::VirtualCameraComponent>(entt::exclude<Vapor::InactiveComponent>);
                view.each([&](auto entity, auto& cam) { cam.isActive = reg.all_of<FirstPersonCameraComponent>(entity); }
                );
                break;
            }
            }
            reg.remove<CameraSwitchRequest>(global);
        }
    }
};

class CharacterMovementSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        auto view = reg.view<CharacterControllerComponent, CharacterIntent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& intent = view.get<CharacterIntent>(entity);
            auto& controller = view.get<CharacterControllerComponent>(entity);
            if (intent.jump) {
            }
        }
    }
};

// (Camera control lives in the engine now: Vapor::CameraControlSystem in
// Vapor/systems.hpp consumes the same CharacterIntent this app writes.)

// ============================================================================
// Subtitle Systems - Split into single-responsibility systems
// ============================================================================
// Execution order:
//   1. SubtitleInputSystem      - detect advance request, send "ShowSubtitle"
//   2. SubtitlePageSensorSystem - detect page animation, send "PageVisible"/"PageHidden"
//   3. SubtitleTimerSystem      - update display timer, send "HideSubtitle"
//   4. FSMSystem::update        - process events, emit FSMStateChangeEvent
//   5. SubtitleActionSystem     - respond to FSMStateChangeEvent, call PageSystem

// Detects advance/restart requests and triggers showing next subtitle
class SubtitleInputSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<SubtitleQueueComponent, Vapor::FSMStateComponent, Vapor::FSMEventQueue>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& q = view.get<SubtitleQueueComponent>(entity);
            auto& fsm = view.get<Vapor::FSMStateComponent>(entity);
            auto& events = view.get<Vapor::FSMEventQueue>(entity);

            // Handle restart request
            if (q.restartRequested) {
                q.restartRequested = false;
                q.currentIndex = -1;
                q.advanceRequested = true;
                fsm.currentState = SubtitleStates::Idle;
                fsm.stateTime = 0.0f;
            }

            if (fsm.currentState != SubtitleStates::Idle) continue;

            bool advance = q.advanceRequested || (q.autoAdvance && q.currentIndex < (int)q.queue.size() - 1);
            if (advance) {
                q.advanceRequested = false;
                q.currentIndex++;
                if (q.currentIndex < (int)q.queue.size()) {
                    events.push("ShowSubtitle");
                }
            } else {
                q.advanceRequested = false;
            }
        }
    }
};

// Detects page animation state and sends corresponding events
class SubtitlePageSensorSystem {
public:
    static void update(entt::registry& reg) {
        auto* page = PageSystem::getPage<SubtitlePage>(reg, PageID::Subtitle);
        if (!page) return;

        auto view = reg.view<Vapor::FSMStateComponent, Vapor::FSMEventQueue>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& fsm = view.get<Vapor::FSMStateComponent>(entity);
            auto& events = view.get<Vapor::FSMEventQueue>(entity);

            if (fsm.currentState == SubtitleStates::WaitingForVisible && page->isFullyVisible()) {
                events.push("PageVisible");
            }

            if (fsm.currentState == SubtitleStates::WaitingForHidden && page->isFullyHidden()) {
                events.push("PageHidden");
            }
        }
    }
};

// Updates display timer and triggers hide when done
class SubtitleTimerSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<SubtitleQueueComponent, Vapor::FSMStateComponent, Vapor::FSMEventQueue>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& q = view.get<SubtitleQueueComponent>(entity);
            auto& fsm = view.get<Vapor::FSMStateComponent>(entity);
            auto& events = view.get<Vapor::FSMEventQueue>(entity);

            if (fsm.currentState != SubtitleStates::Displaying) continue;

            q.displayTimer += dt;

            bool done = q.advanceRequested
                        || (q.autoAdvance && q.currentIndex < (int)q.queue.size()
                            && q.displayTimer >= q.queue[q.currentIndex].duration);
            if (done) {
                q.advanceRequested = false;
                events.push("HideSubtitle");
            }
        }
    }
};

// Responds to FSMStateChangeEvent and performs actions
class SubtitleActionSystem {
public:
    static void update(entt::registry& reg) {
        auto* page = PageSystem::getPage<SubtitlePage>(reg, PageID::Subtitle);
        if (!page) return;

        auto view = reg.view<SubtitleQueueComponent, Vapor::FSMStateChangeEvent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& q = view.get<SubtitleQueueComponent>(entity);
            auto& event = view.get<Vapor::FSMStateChangeEvent>(entity);

            // Entering WaitingForVisible: set content and show page
            if (event.toState == SubtitleStates::WaitingForVisible) {
                if (q.currentIndex >= 0 && q.currentIndex < (int)q.queue.size()) {
                    auto& entry = q.queue[q.currentIndex];
                    page->setContent(entry.speaker, entry.text);
                    PageSystem::show(reg, PageID::Subtitle);
                    q.displayTimer = 0.0f;
                }
            }

            // Entering WaitingForHidden: hide page
            if (event.toState == SubtitleStates::WaitingForHidden) {
                PageSystem::hide(reg, PageID::Subtitle);
            }
        }
    }
};

class ScrollTextQueueSystem {
public:
    static void update(entt::registry& reg) {
        auto* page = PageSystem::getPage<ScrollTextPage>(reg, PageID::ScrollText);
        if (!page) return;

        auto view = reg.view<ScrollTextQueueComponent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& q = view.get<ScrollTextQueueComponent>(entity);
            if (!q.advanceRequested || !page->isIdle()) {
                q.advanceRequested = false;
                continue;
            }
            q.advanceRequested = false;

            if (q.currentIndex >= (int)q.lines.size() - 1) {
                q.currentIndex = 0;
                page->setLine(q.lines[0]);
            } else {
                q.currentIndex++;
                page->scrollToNext(q.lines[q.currentIndex]);
            }
        }
    }
};

class ChapterTitleTriggerSystem {
public:
    static void update(entt::registry& reg) {
        auto* page = PageSystem::getPage<ChapterTitlePage>(reg, PageID::ChapterTitle);
        if (!page) return;

        auto view = reg.view<ChapterTitleTriggerComponent>(entt::exclude<Vapor::InactiveComponent>);
        for (auto entity : view) {
            auto& t = view.get<ChapterTitleTriggerComponent>(entity);
            if (t.showRequested && page->isFullyHidden()) {
                t.showRequested = false;
                page->display(t.number, t.title);
            }
        }
    }
};
