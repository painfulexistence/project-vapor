#pragma once
#include "Vapor/character_controller.hpp"
#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/resource_manager.hpp"
#include "Vapor/rmlui_manager.hpp"
#include "Vapor/scene.hpp"
#include "components.hpp"
#include "pages/chapter_title_page.hpp"
#include "pages/page_system.hpp"
#include "pages/scroll_text_page.hpp"
#include "pages/subtitle_page.hpp"
#include <algorithm>
#include <fmt/core.h>
#include <glm/gtc/matrix_transform.hpp>



class TransformSystem {
public:
    static void update(entt::registry& registry) {
        auto view = registry.view<Vapor::TransformComponent>();
        for (auto entity : view) {
            auto& tc = view.get<Vapor::TransformComponent>(entity);
            if (!tc.isDirty) continue;
            tc.worldTransform =
                glm::translate(glm::mat4(1.0f), tc.position) *
                glm::mat4_cast(tc.rotation) *
                glm::scale(glm::mat4(1.0f), tc.scale);
            tc.isDirty = false;
        }
    }
};

class CleanupSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<DeadTag>();
        for (auto entity : view) {
            reg.destroy(entity);
        }
    }
};

class FlipbookSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        auto view = reg.view<Vapor::SpriteComponent, Vapor::FlipbookComponent>();
        for (auto entity : view) {
            auto& sprite = view.get<Vapor::SpriteComponent>(entity);
            auto& flipbook = view.get<Vapor::FlipbookComponent>(entity);

            if (!flipbook.playing || flipbook.frameIndices.empty()) continue;

            flipbook.timer += deltaTime;
            if (flipbook.timer >= flipbook.frameTime) {
                flipbook.timer -= flipbook.frameTime;
                flipbook.currentIndex++;
                if (flipbook.currentIndex >= flipbook.frameIndices.size()) {
                    flipbook.currentIndex = flipbook.loop ? 0 : flipbook.frameIndices.size() - 1;
                    if (!flipbook.loop) flipbook.playing = false;
                }
                sprite.frameIndex = flipbook.frameIndices[flipbook.currentIndex];
            }
        }
    }
};

class SpriteRenderSystem {
public:
    static void update(
        entt::registry& reg,
        Renderer* renderer,
        Vapor::ResourceManager* resourceManager
    ) {
        // Collect visible sprites
        std::vector<std::tuple<glm::mat4, Vapor::SpriteComponent*, entt::entity>> sprites;

        auto view = reg.view<Vapor::TransformComponent, Vapor::SpriteComponent>();
        for (auto entity : view) {
            auto& sprite = view.get<Vapor::SpriteComponent>(entity);
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
    static void update(entt::registry& reg, Scene* scene, float deltaTime) {
        auto pointLightView = reg.view<ScenePointLightReferenceComponent, LightMovementLogicComponent>();
        for (auto entity : pointLightView) {
            auto& ref = pointLightView.get<ScenePointLightReferenceComponent>(entity);
            auto& logic = pointLightView.get<LightMovementLogicComponent>(entity);

            if (ref.lightIndex < 0 || ref.lightIndex >= scene->pointLights.size()) continue;

            auto& light = scene->pointLights[ref.lightIndex];
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

            light.position = glm::vec3(x, y, z);
            // Optional: intensity modulation
            // light.intensity = 5.0f + sin(logic.timer * 2.0f) * 2.0f;
        }
        auto directionalLightView = reg.view<SceneDirectionalLightReferenceComponent, DirectionalLightLogicComponent>();
        for (auto entity : directionalLightView) {
            auto& ref = directionalLightView.get<SceneDirectionalLightReferenceComponent>(entity);
            auto& logic = directionalLightView.get<DirectionalLightLogicComponent>(entity);
            if (ref.lightIndex >= 0 && ref.lightIndex < scene->directionalLights.size()) {
                logic.timer += deltaTime * logic.speed;
                // Simple oscillation on Z axis relative to base direction
                glm::vec3 newDir = logic.baseDirection;
                newDir.z += logic.magnitude * sin(logic.timer);
                scene->directionalLights[ref.lightIndex].direction = glm::normalize(newDir);
            }
        }
    }
};

class TransformSystem {
public:
    static void update(entt::registry& registry) {
        auto view = registry.view<Vapor::TransformComponent>();
        for (auto entity : view) {
            auto& t = view.get<Vapor::TransformComponent>(entity);
            if (!t.isDirty) continue;
            glm::mat4 local = glm::translate(glm::mat4(1.0f), t.position)
                            * glm::mat4_cast(t.rotation)
                            * glm::scale(glm::mat4(1.0f), t.scale);
            if (t.parent != entt::null) {
                if (auto* parentT = registry.try_get<Vapor::TransformComponent>(t.parent)) {
                    t.worldTransform = parentT->worldTransform * local;
                } else {
                    t.worldTransform = local;
                }
            } else {
                t.worldTransform = local;
            }
            t.isDirty = false;
        }
    }
};

class AutoRotateSystem {
public:
    static void update(entt::registry& registry, float deltaTime) {
        auto view = registry.view<Vapor::TransformComponent, AutoRotateComponent>();
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
                auto view = reg.view<Vapor::VirtualCameraComponent>();
                view.each([&](auto entity, auto& cam) { cam.isActive = reg.all_of<FlyCameraComponent>(entity); });
                break;
            }
            case CameraSwitchRequest::Mode::Follow: {
                auto view = reg.view<Vapor::VirtualCameraComponent>();
                view.each([&](auto entity, auto& cam) { cam.isActive = reg.all_of<FollowCameraComponent>(entity); });
                break;
            }
            case CameraSwitchRequest::Mode::FirstPerson: {
                auto view = reg.view<Vapor::VirtualCameraComponent>();
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
        auto view = reg.view<CharacterControllerComponent, CharacterIntent>();
        for (auto entity : view) {
            auto& intent = view.get<CharacterIntent>(entity);
            auto& controller = view.get<CharacterControllerComponent>(entity);
            if (intent.jump) {
            }
        }
    }
};

class CameraSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        auto view = reg.view<Vapor::VirtualCameraComponent>();

        for (auto entity : view) {
            auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);
            if (!cam.isActive) continue;

            if (auto [fly, intent] = reg.try_get<FlyCameraComponent, CharacterIntent>(entity); fly && intent) {
                fly->pitch -= intent->lookVector.y * fly->rotateSpeed * deltaTime;
                fly->yaw -= intent->lookVector.x * fly->rotateSpeed * deltaTime;
                fly->pitch = glm::clamp(fly->pitch, -89.0f, 89.0f);

                cam.rotation = glm::quat(glm::vec3(glm::radians(-fly->pitch), glm::radians(fly->yaw - 90.0f), 0.0f));

                glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);
                glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
                glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);

                if (intent->moveVector.x != 0.0f)
                    cam.position += intent->moveVector.x * right * fly->moveSpeed * deltaTime;
                if (intent->moveVector.y != 0.0f)
                    cam.position += intent->moveVector.y * front * fly->moveSpeed * deltaTime;
                if (intent->moveVerticalAxis != 0.0f)
                    cam.position += intent->moveVerticalAxis * up * fly->moveSpeed * deltaTime;
            }

            if (auto* follow = reg.try_get<FollowCameraComponent>(entity)) {
                if (!reg.valid(follow->target)) continue;
                if (auto* transform = reg.try_get<Vapor::TransformComponent>(follow->target)) {
                    glm::vec3 targetPos = transform->position;
                    glm::vec3 desiredPos = targetPos + follow->offset;
                    cam.position = glm::mix(cam.position, desiredPos, 1.0f - pow(follow->smoothFactor, deltaTime));
                    cam.rotation = glm::quatLookAt(glm::normalize(targetPos - cam.position), glm::vec3(0, 1, 0));
                }
            }

            glm::mat4 rotation = glm::mat4_cast(cam.rotation);
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
            cam.viewMatrix = glm::inverse(translation * rotation);
            cam.projectionMatrix = glm::perspective(cam.fov, cam.aspect, cam.near, cam.far);
        }
    }
};


// --- UI Trigger Systems ---
// These systems own the content/timing logic for cinematic overlay pages.
// They talk to their corresponding Page via PageSystem::getPage<T>().

class SubtitleQueueSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto* page = PageSystem::getPage<SubtitlePage>(reg, PageID::Subtitle);
        if (!page) return;

        auto view = reg.view<SubtitleQueueComponent>();
        for (auto entity : view) {
            auto& q = view.get<SubtitleQueueComponent>(entity);

            switch (q.state) {
            case SubtitleQueueState::Idle:
                if (page->isFullyHidden()) {
                    bool advance = q.advanceRequested || (q.autoAdvance && q.currentIndex < (int)q.queue.size() - 1);
                    if (advance) {
                        q.advanceRequested = false;
                        q.currentIndex++;
                        if (q.currentIndex < (int)q.queue.size()) {
                            auto& entry = q.queue[q.currentIndex];
                            page->setContent(entry.speaker, entry.text);
                            PageSystem::show(reg, PageID::Subtitle);
                            q.displayTimer = 0.0f;
                            q.state = SubtitleQueueState::WaitingForVisible;
                        }
                    } else {
                        q.advanceRequested = false;
                    }
                }
                break;

            case SubtitleQueueState::WaitingForVisible:
                if (page->isFullyVisible()) {
                    q.state = SubtitleQueueState::Displaying;
                }
                break;

            case SubtitleQueueState::Displaying:
                q.displayTimer += dt;
                {
                    bool done = q.advanceRequested
                                || (q.autoAdvance && q.currentIndex < (int)q.queue.size()
                                    && q.displayTimer >= q.queue[q.currentIndex].duration);
                    if (done) {
                        q.advanceRequested = false;
                        PageSystem::hide(reg, PageID::Subtitle);
                        q.state = SubtitleQueueState::WaitingForHidden;
                    }
                }
                break;

            case SubtitleQueueState::WaitingForHidden:
                if (page->isFullyHidden()) {
                    q.state = SubtitleQueueState::Idle;
                }
                break;
            }
        }
    }

    static void restart(entt::registry& reg) {
        auto view = reg.view<SubtitleQueueComponent>();
        for (auto entity : view) {
            auto& q = view.get<SubtitleQueueComponent>(entity);
            q.currentIndex = -1;
            q.state = SubtitleQueueState::Idle;
            q.advanceRequested = true;
        }
    }

    static void advance(entt::registry& reg) {
        auto view = reg.view<SubtitleQueueComponent>();
        for (auto entity : view)
            view.get<SubtitleQueueComponent>(entity).advanceRequested = true;
    }
};

class ScrollTextQueueSystem {
public:
    static void update(entt::registry& reg) {
        auto* page = PageSystem::getPage<ScrollTextPage>(reg, PageID::ScrollText);
        if (!page) return;

        auto view = reg.view<ScrollTextQueueComponent>();
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

    static void advance(entt::registry& reg) {
        auto view = reg.view<ScrollTextQueueComponent>();
        for (auto entity : view)
            view.get<ScrollTextQueueComponent>(entity).advanceRequested = true;
    }
};

class ChapterTitleTriggerSystem {
public:
    static void update(entt::registry& reg) {
        auto* page = PageSystem::getPage<ChapterTitlePage>(reg, PageID::ChapterTitle);
        if (!page) return;

        auto view = reg.view<ChapterTitleTriggerComponent>();
        for (auto entity : view) {
            auto& t = view.get<ChapterTitleTriggerComponent>(entity);
            if (t.showRequested && page->isFullyHidden()) {
                t.showRequested = false;
                page->display(t.number, t.title);
            }
        }
    }

    static void request(entt::registry& reg, const std::string& number, const std::string& title) {
        auto view = reg.view<ChapterTitleTriggerComponent>();
        for (auto entity : view) {
            auto& t = view.get<ChapterTitleTriggerComponent>(entity);
            t.number = number;
            t.title = title;
            t.showRequested = true;
        }
    }
};
