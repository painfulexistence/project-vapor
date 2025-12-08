#pragma once
#include "Vapor/character_controller.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/rmlui_manager.hpp"
#include "Vapor/scene.hpp"
#include "components.hpp"
#include <fmt/core.h>

class CleanupSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<DeadTag>();
        for (auto entity : view) {
            reg.destroy(entity);
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

void updateLightMovementSystem(entt::registry& reg, Scene* scene, float deltaTime) {
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

void updateAutoRotateSystem(entt::registry& registry, float deltaTime) {
    auto view = registry.view<SceneNodeReferenceComponent, AutoRotateComponent>();
    for (auto entity : view) {
        auto& ref = view.get<SceneNodeReferenceComponent>(entity);
        auto& rotate = view.get<AutoRotateComponent>(entity);
        if (ref.node) {
            ref.node->rotate(rotate.axis, rotate.speed * deltaTime);
        }
    }
}

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

void updateCameraSystem(entt::registry& reg, float deltaTime) {
    auto view = reg.view<Vapor::VirtualCameraComponent>();

    for (auto entity : view) {
        auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);
        if (!cam.isActive) continue;

        // 1. Handle Fly Camera Logic
        if (auto [fly, intent] = reg.try_get<FlyCameraComponent, CharacterIntent>(entity); fly && intent) {
            // Rotation
            fly->pitch -= intent->lookVector.y * fly->rotateSpeed * deltaTime;
            fly->yaw -= intent->lookVector.x * fly->rotateSpeed * deltaTime;
            fly->pitch = glm::clamp(fly->pitch, -89.0f, 89.0f);

            cam.rotation = glm::quat(glm::vec3(glm::radians(-fly->pitch), glm::radians(fly->yaw - 90.0f), 0.0f));

            glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);
            glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
            glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);

            if (intent->moveVector.x != 0.0f) cam.position += intent->moveVector.x * right * fly->moveSpeed * deltaTime;
            if (intent->moveVector.y != 0.0f) cam.position += intent->moveVector.y * front * fly->moveSpeed * deltaTime;
            if (intent->moveVerticalAxis != 0.0f)
                cam.position += intent->moveVerticalAxis * up * fly->moveSpeed * deltaTime;
        }

        // 2. Handle Follow Camera Logic
        if (auto* follow = reg.try_get<FollowCameraComponent>(entity)) {
            if (!reg.valid(follow->target)) continue;
            if (auto* nodeRef = reg.try_get<SceneNodeReferenceComponent>(follow->target)) {
                if (nodeRef->node) {
                    glm::vec3 targetPos = nodeRef->node->getWorldPosition();
                    glm::vec3 desiredPos = targetPos + follow->offset;
                    cam.position = glm::mix(cam.position, desiredPos, 1.0f - pow(follow->smoothFactor, deltaTime));
                    cam.rotation = glm::quatLookAt(glm::normalize(targetPos - cam.position), glm::vec3(0, 1, 0));
                }
            }
        }

        // 3. Update Matrices
        glm::mat4 rotation = glm::mat4_cast(cam.rotation);
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
        cam.viewMatrix = glm::inverse(translation * rotation);
        cam.projectionMatrix = glm::perspective(cam.fov, cam.aspect, cam.near, cam.far);
    }
}

void updateHUDSystem(entt::registry& reg, Vapor::RmlUiManager* rmluiManager, float deltaTime) {
    if (!rmluiManager) return;

    auto view = reg.view<HUDComponent>();
    for (auto entity : view) {
        auto& hud = view.get<HUDComponent>(entity);

        // 1. Load document if not loaded
        if (!hud.document && !hud.documentPath.empty()) {
            hud.document = rmluiManager->LoadDocument(hud.documentPath);
            if (hud.document) {
                fmt::print("Loaded HUD document: {}\n", hud.documentPath);
                // Initialize state based on visibility
                if (hud.isVisible) {
                    hud.state = HUDState::Visible;
                    hud.document->Show();
                    // Force visible class immediately
                    if (auto el = hud.document->GetElementById("hud_content")) {
                        el->SetClass("visible", true);
                    }
                } else {
                    hud.state = HUDState::Hidden;
                    hud.document->Hide();
                }
            } else {
                fmt::print(stderr, "Failed to load HUD document: {}\n", hud.documentPath);
                continue;
            }
        }

        if (!hud.document) continue;

        auto element = hud.document->GetElementById("hud-container");
        if (!element) continue;

        // 2. State Machine
        switch (hud.state) {
        case HUDState::Hidden:
            if (hud.isVisible) {
                hud.state = HUDState::FadingIn;
                hud.document->Show();
                // Trigger fade in
                element->SetClass("visible", true);
                hud.timer = 0.0f;
            }
            break;

        case HUDState::FadingIn:
            hud.timer += deltaTime;
            if (!hud.isVisible) {
                // Interrupted
                hud.state = HUDState::FadingOut;
                element->SetClass("visible", false);
                hud.timer = 0.0f;// Reset timer or calculate remaining? Simple reset for now.
            } else if (hud.timer >= hud.fadeDuration) {
                hud.state = HUDState::Visible;
            }
            break;

        case HUDState::Visible:
            if (!hud.isVisible) {
                hud.state = HUDState::FadingOut;
                // Trigger fade out
                element->SetClass("visible", false);
                hud.timer = 0.0f;
            }
            break;

        case HUDState::FadingOut:
            hud.timer += deltaTime;
            if (hud.isVisible) {
                // Interrupted
                hud.state = HUDState::FadingIn;
                element->SetClass("visible", true);
                hud.timer = 0.0f;
            } else if (hud.timer >= hud.fadeDuration) {
                hud.state = HUDState::Hidden;
                hud.document->Hide();
            }
            break;
        }
    }
}

void updateScrollTextSystem(entt::registry& reg, Vapor::RmlUiManager* rmluiManager, float deltaTime) {
    if (!rmluiManager) return;

    auto view = reg.view<ScrollTextComponent>();
    for (auto entity : view) {
        auto& scroll = view.get<ScrollTextComponent>(entity);

        // 1. Load document if not loaded
        if (!scroll.document && !scroll.documentPath.empty()) {
            scroll.document = rmluiManager->LoadDocument(scroll.documentPath);
            if (scroll.document) {
                fmt::print("Loaded scroll text document: {}\n", scroll.documentPath);
                scroll.document->Show();

                // Set initial text
                if (!scroll.lines.empty()) {
                    if (auto el = scroll.document->GetElementById("scroll-text")) {
                        el->SetInnerRML(scroll.lines[scroll.currentIndex].c_str());
                        el->SetClass("visible", true);
                    }
                }
            } else {
                fmt::print(stderr, "Failed to load scroll text document: {}\n", scroll.documentPath);
                continue;
            }
        }

        if (!scroll.document) continue;

        auto element = scroll.document->GetElementById("scroll-text");
        if (!element) continue;

        // 2. State Machine
        switch (scroll.state) {
        case ScrollTextState::Idle:
            if (scroll.advanceRequested && scroll.currentIndex < (int)scroll.lines.size() - 1) {
                scroll.advanceRequested = false;
                scroll.state = ScrollTextState::ScrollingOut;
                scroll.timer = 0.0f;
                // Trigger scroll out animation
                element->SetClass("visible", false);
                element->SetClass("scroll-out", true);
            } else {
                scroll.advanceRequested = false;// Clear if at end
            }
            break;

        case ScrollTextState::ScrollingOut:
            scroll.timer += deltaTime;
            if (scroll.timer >= scroll.scrollDuration) {
                // Move to next line
                scroll.currentIndex++;
                if (scroll.currentIndex < (int)scroll.lines.size()) {
                    element->SetInnerRML(scroll.lines[scroll.currentIndex].c_str());
                }
                // Prepare for scroll-in: position element below without transition
                scroll.state = ScrollTextState::PreparingScrollIn;
                element->SetClass("scroll-out", false);
                element->SetClass("scroll-in-prepare", true);
            }
            break;

        case ScrollTextState::PreparingScrollIn:
            // Wait one frame for the element to be positioned, then animate in
            scroll.state = ScrollTextState::ScrollingIn;
            scroll.timer = 0.0f;
            element->SetClass("scroll-in-prepare", false);
            element->SetClass("scroll-in", true);
            break;

        case ScrollTextState::ScrollingIn:
            scroll.timer += deltaTime;
            if (scroll.timer >= scroll.scrollDuration) {
                scroll.state = ScrollTextState::Idle;
                element->SetClass("scroll-in", false);
                element->SetClass("visible", true);
            }
            break;
        }
    }
}

void updateLetterboxSystem(entt::registry& reg, Vapor::RmlUiManager* rmluiManager, float deltaTime) {
    if (!rmluiManager) return;

    auto view = reg.view<LetterboxComponent>();
    for (auto entity : view) {
        auto& lb = view.get<LetterboxComponent>(entity);

        // 1. Load document if not loaded
        if (!lb.document && !lb.documentPath.empty()) {
            lb.document = rmluiManager->LoadDocument(lb.documentPath);
            if (lb.document) {
                fmt::print("Loaded letterbox document: {}\n", lb.documentPath);
                lb.document->Show();
            } else {
                fmt::print(stderr, "Failed to load letterbox document: {}\n", lb.documentPath);
                continue;
            }
        }

        if (!lb.document) continue;

        auto topBar = lb.document->GetElementById("letterbox-top");
        auto bottomBar = lb.document->GetElementById("letterbox-bottom");
        if (!topBar || !bottomBar) continue;

        // 2. State Machine
        switch (lb.state) {
        case LetterboxState::Hidden:
            if (lb.isOpen) {
                lb.state = LetterboxState::Opening;
                lb.timer = 0.0f;
                topBar->SetClass("open", true);
                bottomBar->SetClass("open", true);
            }
            break;

        case LetterboxState::Opening:
            lb.timer += deltaTime;
            if (!lb.isOpen) {
                // Interrupted - close
                lb.state = LetterboxState::Closing;
                lb.timer = 0.0f;
                topBar->SetClass("open", false);
                bottomBar->SetClass("open", false);
            } else if (lb.timer >= lb.animDuration) {
                lb.state = LetterboxState::Open;
            }
            break;

        case LetterboxState::Open:
            if (!lb.isOpen) {
                lb.state = LetterboxState::Closing;
                lb.timer = 0.0f;
                topBar->SetClass("open", false);
                bottomBar->SetClass("open", false);
            }
            break;

        case LetterboxState::Closing:
            lb.timer += deltaTime;
            if (lb.isOpen) {
                // Interrupted - reopen
                lb.state = LetterboxState::Opening;
                lb.timer = 0.0f;
                topBar->SetClass("open", true);
                bottomBar->SetClass("open", true);
            } else if (lb.timer >= lb.animDuration) {
                lb.state = LetterboxState::Hidden;
            }
            break;
        }
    }
}