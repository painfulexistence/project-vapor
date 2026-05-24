#pragma once

#include "Vapor/components.hpp"
#include "Vapor/physics_3d.hpp"
#include "components.hpp"
#include "level_serializer.hpp"
#include "imgui.h"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <string>

// ============================================================================
// SceneInspector — ImGui panel: entity list + component inspector
// ============================================================================

class SceneInspector {
public:
    void draw(entt::registry& registry) {
        drawEntityList(registry);
        drawInspector(registry);
        drawSavePanel(registry);
    }

    // Call once after init so the save panel knows the source GLTF.
    void setGltfPath(const std::string& path, bool optimized = true) {
        strncpy(m_gltfPathBuf, path.c_str(), sizeof(m_gltfPathBuf) - 1);
        m_gltfOptimized = optimized;
    }

private:
    entt::entity m_selected = entt::null;
    char m_searchBuf[128] = {};

    // Save panel state
    char m_savePath[256]    = "scene.json";
    char m_gltfPathBuf[256] = "models/Sponza/Sponza.gltf";
    bool m_gltfOptimized    = true;
    std::string m_saveStatus;   // feedback message shown after save
    bool m_saveOk = true;

    // -------------------------------------------------------------------------
    // Left panel — entity list
    // -------------------------------------------------------------------------
    void drawEntityList(entt::registry& registry) {
        ImGui::SetNextWindowSize(ImVec2(280, 500), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Scene##inspector_list")) {
            ImGui::End();
            return;
        }

        ImGui::InputText("Search", m_searchBuf, sizeof(m_searchBuf));
        ImGui::Separator();

        std::string filter(m_searchBuf);
        for (auto& c : filter) c = static_cast<char>(tolower(c));

        registry.each([&](entt::entity entity) {
            std::string label = entityLabel(registry, entity);
            std::string labelLower = label;
            for (auto& c : labelLower) c = static_cast<char>(tolower(c));

            if (!filter.empty() && labelLower.find(filter) == std::string::npos)
                return;

            bool selected = (entity == m_selected);
            ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
            if (ImGui::Selectable(label.c_str(), selected))
                m_selected = entity;
            ImGui::PopID();
        });

        ImGui::Separator();
        if (ImGui::Button("Create Entity")) {
            m_selected = registry.create();
            registry.emplace<Vapor::NameComponent>(m_selected, Vapor::NameComponent{ "New Entity" });
            registry.emplace<Vapor::TransformComponent>(m_selected);
        }
        if (m_selected != entt::null && registry.valid(m_selected)) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Destroy")) {
                registry.destroy(m_selected);
                m_selected = entt::null;
            }
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }

    // -------------------------------------------------------------------------
    // Right panel — component inspector for selected entity
    // -------------------------------------------------------------------------
    void drawInspector(entt::registry& registry) {
        ImGui::SetNextWindowSize(ImVec2(380, 500), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(300, 30), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Inspector##inspector_detail")) {
            ImGui::End();
            return;
        }

        if (m_selected == entt::null || !registry.valid(m_selected)) {
            ImGui::TextDisabled("No entity selected");
            ImGui::End();
            return;
        }

        ImGui::Text("Entity %u", static_cast<Uint32>(entt::to_integral(m_selected)));
        ImGui::Separator();

        // --- Engine components ---
        if (auto* c = registry.try_get<Vapor::NameComponent>(m_selected))
            drawNameComponent(registry, *c);

        if (auto* c = registry.try_get<Vapor::TransformComponent>(m_selected))
            drawTransformComponent(registry, *c);

        if (auto* c = registry.try_get<Vapor::MeshRendererComponent>(m_selected))
            drawMeshRendererComponent(*c);

        if (auto* c = registry.try_get<Vapor::RigidbodyComponent>(m_selected))
            drawRigidbodyComponent(*c);

        if (auto* c = registry.try_get<Vapor::BoxColliderComponent>(m_selected))
            drawBoxColliderComponent(*c);

        if (auto* c = registry.try_get<Vapor::SphereColliderComponent>(m_selected))
            drawSphereColliderComponent(*c);

        if (auto* c = registry.try_get<Vapor::VirtualCameraComponent>(m_selected))
            drawVirtualCameraComponent(*c);

        if (auto* c = registry.try_get<Vapor::FlyCameraComponent>(m_selected))
            drawFlyCameraComponent(*c);

        if (auto* c = registry.try_get<Vapor::FollowCameraComponent>(m_selected))
            drawFollowCameraComponent(registry, *c);

        if (auto* c = registry.try_get<Vapor::GrabberComponent>(m_selected))
            drawGrabberComponent(registry, *c);

        if (auto* c = registry.try_get<Vapor::HeldByComponent>(m_selected))
            drawHeldByComponent(registry, *c);

        if (auto* c = registry.try_get<Vapor::TriggerVolumeComponent>(m_selected))
            drawTriggerVolumeComponent(*c);

        // --- Game (Vaporware) components ---
        if (auto* c = registry.try_get<AutoRotateComponent>(m_selected))
            drawAutoRotateComponent(*c);

        if (auto* c = registry.try_get<CharacterControllerComponent>(m_selected))
            drawCharacterControllerComponent(*c);

        if (auto* c = registry.try_get<CharacterIntent>(m_selected))
            drawCharacterIntent(*c);

        if (auto* c = registry.try_get<LightMovementLogicComponent>(m_selected))
            drawLightMovementLogicComponent(*c);

        if (auto* c = registry.try_get<DirectionalLightLogicComponent>(m_selected))
            drawDirectionalLightLogicComponent(*c);

        if (auto* c = registry.try_get<ScenePointLightReferenceComponent>(m_selected))
            drawScenePointLightRefComponent(*c);

        if (auto* c = registry.try_get<SceneDirectionalLightReferenceComponent>(m_selected))
            drawSceneDirectionalLightRefComponent(*c);

        if (auto* c = registry.try_get<GrabbableComponent>(m_selected))
            drawGrabbableComponent(*c);

        if (registry.all_of<PersistentTag>(m_selected))
            componentTag("PersistentTag");

        if (registry.all_of<DeadTag>(m_selected))
            componentTag("DeadTag");

        // --- Add component button ---
        ImGui::Separator();
        drawAddComponentMenu(registry);

        ImGui::End();
    }

    // =========================================================================
    // Component drawers
    // =========================================================================

    void drawNameComponent(entt::registry& registry, Vapor::NameComponent& c) {
        if (!ImGui::CollapsingHeader("Name", ImGuiTreeNodeFlags_DefaultOpen)) return;
        char buf[256];
        strncpy(buf, c.name.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText("Name##name_field", buf, sizeof(buf)))
            c.name = buf;
    }

    void drawTransformComponent(entt::registry& registry, Vapor::TransformComponent& c) {
        if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) return;

        bool changed = false;
        changed |= ImGui::DragFloat3("Position", &c.position.x, 0.1f);

        // Quat <-> Euler round-trip for editing
        glm::vec3 euler = glm::degrees(glm::eulerAngles(c.rotation));
        if (ImGui::DragFloat3("Rotation (°)", &euler.x, 0.5f)) {
            c.rotation = glm::quat(glm::radians(euler));
            changed = true;
        }

        changed |= ImGui::DragFloat3("Scale", &c.scale.x, 0.01f, 0.001f, 100.0f);

        if (changed) c.isDirty = true;

        if (auto parent = c.parent; parent != entt::null && registry.valid(parent)) {
            ImGui::LabelText("Parent", "%s", entityLabel(registry, parent).c_str());
        }
    }

    void drawMeshRendererComponent(Vapor::MeshRendererComponent& c) {
        if (!ImGui::CollapsingHeader("Mesh Renderer")) return;
        ImGui::Checkbox("Visible", &c.visible);
        ImGui::LabelText("Mesh count", "%zu", c.meshes.size());
    }

    void drawRigidbodyComponent(Vapor::RigidbodyComponent& c) {
        if (!ImGui::CollapsingHeader("Rigidbody")) return;

        const char* motionNames[] = { "Static", "Dynamic", "Kinematic" };
        int motion = static_cast<int>(c.motionType);
        if (ImGui::Combo("Motion Type", &motion, motionNames, 3))
            c.motionType = static_cast<BodyMotionType>(motion);

        ImGui::Checkbox("Sync To Physics", &c.syncToPhysics);
        ImGui::Checkbox("Sync From Physics", &c.syncFromPhysics);
        ImGui::LabelText("Body", c.body.valid() ? "valid" : "invalid");
    }

    void drawBoxColliderComponent(Vapor::BoxColliderComponent& c) {
        if (!ImGui::CollapsingHeader("Box Collider")) return;
        ImGui::DragFloat3("Half Size", &c.halfSize.x, 0.01f, 0.001f, 100.0f);
    }

    void drawSphereColliderComponent(Vapor::SphereColliderComponent& c) {
        if (!ImGui::CollapsingHeader("Sphere Collider")) return;
        ImGui::DragFloat("Radius", &c.radius, 0.01f, 0.001f, 100.0f);
    }

    void drawVirtualCameraComponent(Vapor::VirtualCameraComponent& c) {
        if (!ImGui::CollapsingHeader("Virtual Camera")) return;
        ImGui::Checkbox("Active", &c.isActive);
        ImGui::DragFloat3("Position##vcam", &c.position.x, 0.1f);
        float fovDeg = glm::degrees(c.fov);
        if (ImGui::DragFloat("FOV (°)", &fovDeg, 0.5f, 5.0f, 170.0f))
            c.fov = glm::radians(fovDeg);
        ImGui::DragFloat("Near", &c.near, 0.001f, 0.001f, 10.0f);
        ImGui::DragFloat("Far", &c.far, 1.0f, 1.0f, 10000.0f);
        ImGui::LabelText("Aspect", "%.3f", c.aspect);
    }

    void drawFlyCameraComponent(Vapor::FlyCameraComponent& c) {
        if (!ImGui::CollapsingHeader("Fly Camera")) return;
        ImGui::DragFloat("Move Speed##fly", &c.moveSpeed, 0.1f, 0.1f, 100.0f);
        ImGui::DragFloat("Rotate Speed##fly", &c.rotateSpeed, 1.0f, 1.0f, 360.0f);
        ImGui::DragFloat("Yaw##fly", &c.yaw, 0.5f);
        ImGui::DragFloat("Pitch##fly", &c.pitch, 0.5f, -89.0f, 89.0f);
    }

    void drawFollowCameraComponent(entt::registry& registry, Vapor::FollowCameraComponent& c) {
        if (!ImGui::CollapsingHeader("Follow Camera")) return;
        if (c.target != entt::null && registry.valid(c.target))
            ImGui::LabelText("Target", "%s", entityLabel(registry, c.target).c_str());
        else
            ImGui::LabelText("Target", "(none)");
        ImGui::DragFloat3("Offset##follow", &c.offset.x, 0.1f);
        ImGui::DragFloat("Smooth Factor", &c.smoothFactor, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Deadzone", &c.deadzone, 0.01f, 0.0f, 10.0f);
    }

    void drawGrabberComponent(entt::registry& registry, Vapor::GrabberComponent& c) {
        if (!ImGui::CollapsingHeader("Grabber")) return;
        ImGui::DragFloat("Max Pickup Range##grab", &c.maxPickupRange, 0.1f, 0.1f, 100.0f);
        if (c.heldEntity != entt::null && registry.valid(c.heldEntity))
            ImGui::LabelText("Holding", "%s", entityLabel(registry, c.heldEntity).c_str());
        else
            ImGui::LabelText("Holding", "(none)");
    }

    void drawHeldByComponent(entt::registry& registry, Vapor::HeldByComponent& c) {
        if (!ImGui::CollapsingHeader("Held By")) return;
        if (c.holder != entt::null && registry.valid(c.holder))
            ImGui::LabelText("Holder", "%s", entityLabel(registry, c.holder).c_str());
        ImGui::DragFloat("Hold Distance", &c.holdDistance, 0.1f, 0.1f, 20.0f);
        ImGui::LabelText("Orig Gravity", "%.3f", c.originalGravityFactor);
    }

    void drawTriggerVolumeComponent(Vapor::TriggerVolumeComponent& c) {
        if (!ImGui::CollapsingHeader("Trigger Volume")) return;
        ImGui::LabelText("Trigger", c.trigger.valid() ? "valid" : "invalid");
        ImGui::LabelText("onEnter", c.onEnter ? "set" : "none");
        ImGui::LabelText("onExit", c.onExit ? "set" : "none");
    }

    // --- Game components ---

    void drawAutoRotateComponent(AutoRotateComponent& c) {
        if (!ImGui::CollapsingHeader("Auto Rotate")) return;
        ImGui::DragFloat3("Axis##arot", &c.axis.x, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat("Speed##arot", &c.speed, 0.01f);
    }

    void drawCharacterControllerComponent(CharacterControllerComponent& c) {
        if (!ImGui::CollapsingHeader("Character Controller")) return;
        ImGui::DragFloat("Move Speed##cc", &c.moveSpeed, 0.1f, 0.1f, 50.0f);
        ImGui::DragFloat("Rotate Speed##cc", &c.rotateSpeed, 1.0f, 1.0f, 360.0f);
    }

    void drawCharacterIntent(CharacterIntent& c) {
        if (!ImGui::CollapsingHeader("Character Intent")) return;
        ImGui::LabelText("Look", "(%.2f, %.2f)", c.lookVector.x, c.lookVector.y);
        ImGui::LabelText("Move", "(%.2f, %.2f)", c.moveVector.x, c.moveVector.y);
        ImGui::LabelText("Vert Axis", "%.2f", c.moveVerticalAxis);
        ImGui::LabelText("Jump", c.jump ? "true" : "false");
        ImGui::LabelText("Sprint", c.sprint ? "true" : "false");
        ImGui::LabelText("Interact", c.interact ? "true" : "false");
    }

    void drawLightMovementLogicComponent(LightMovementLogicComponent& c) {
        if (!ImGui::CollapsingHeader("Light Movement")) return;
        const char* patterns[] = { "Circle", "Figure8", "Linear", "Spiral" };
        int p = static_cast<int>(c.pattern);
        if (ImGui::Combo("Pattern", &p, patterns, 4))
            c.pattern = static_cast<MovementPattern>(p);
        ImGui::DragFloat("Speed##lm", &c.speed, 0.01f);
        ImGui::DragFloat("Radius##lm", &c.radius, 0.1f, 0.0f, 100.0f);
        ImGui::DragFloat("Height##lm", &c.height, 0.1f, -50.0f, 50.0f);
        ImGui::LabelText("Timer##lm", "%.2f", c.timer);
    }

    void drawDirectionalLightLogicComponent(DirectionalLightLogicComponent& c) {
        if (!ImGui::CollapsingHeader("Directional Light Logic")) return;
        ImGui::DragFloat3("Base Dir", &c.baseDirection.x, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat("Speed##dl", &c.speed, 0.01f);
        ImGui::DragFloat("Magnitude##dl", &c.magnitude, 0.001f, 0.0f, 1.0f);
    }

    void drawScenePointLightRefComponent(ScenePointLightReferenceComponent& c) {
        if (!ImGui::CollapsingHeader("Point Light Ref")) return;
        ImGui::LabelText("Light Index", "%d", c.lightIndex);
    }

    void drawSceneDirectionalLightRefComponent(SceneDirectionalLightReferenceComponent& c) {
        if (!ImGui::CollapsingHeader("Directional Light Ref")) return;
        ImGui::LabelText("Light Index", "%d", c.lightIndex);
    }

    void drawGrabbableComponent(GrabbableComponent& c) {
        if (!ImGui::CollapsingHeader("Grabbable")) return;
        ImGui::DragFloat("Pickup Range##gb", &c.pickupRange, 0.1f, 0.0f, 50.0f);
        ImGui::DragFloat("Hold Offset##gb", &c.holdOffset, 0.1f, 0.0f, 20.0f);
        ImGui::DragFloat("Throw Force##gb", &c.throwForce, 10.0f, 0.0f, 5000.0f);
        ImGui::Checkbox("Is Held##gb", &c.isHeld);
    }

    void componentTag(const char* name) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.4f, 0.2f, 1.0f));
        ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet);
        ImGui::PopStyleColor();
    }

    // =========================================================================
    // Add component pop-up
    // =========================================================================
    void drawAddComponentMenu(entt::registry& registry) {
        if (!ImGui::Button("+ Add Component"))
            return;
        ImGui::OpenPopup("add_component_popup");

        if (ImGui::BeginPopup("add_component_popup")) {
            auto tryAdd = [&]<typename T>(const char* label) {
                if (!registry.all_of<T>(m_selected) && ImGui::MenuItem(label)) {
                    registry.emplace<T>(m_selected);
                    ImGui::CloseCurrentPopup();
                }
            };
            tryAdd.operator()<Vapor::TransformComponent>("Transform");
            tryAdd.operator()<Vapor::MeshRendererComponent>("Mesh Renderer");
            tryAdd.operator()<Vapor::RigidbodyComponent>("Rigidbody");
            tryAdd.operator()<Vapor::BoxColliderComponent>("Box Collider");
            tryAdd.operator()<Vapor::SphereColliderComponent>("Sphere Collider");
            tryAdd.operator()<Vapor::VirtualCameraComponent>("Virtual Camera");
            tryAdd.operator()<Vapor::FlyCameraComponent>("Fly Camera");
            tryAdd.operator()<AutoRotateComponent>("Auto Rotate");
            tryAdd.operator()<GrabbableComponent>("Grabbable");
            tryAdd.operator()<PersistentTag>("Persistent Tag");
            ImGui::EndPopup();
        }
    }

    // =========================================================================
    // Save panel
    // =========================================================================
    void drawSavePanel(entt::registry& registry) {
        ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(10, 545), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Save Level##save_panel")) {
            ImGui::End();
            return;
        }

        ImGui::InputText("Output path", m_savePath, sizeof(m_savePath));
        ImGui::InputText("GLTF path",   m_gltfPathBuf, sizeof(m_gltfPathBuf));
        ImGui::Checkbox("Optimized GLTF", &m_gltfOptimized);

        if (ImGui::Button("Save")) {
            auto result = LevelSerializer::save(
                registry,
                std::string(m_gltfPathBuf),
                m_gltfOptimized,
                std::string(m_savePath)
            );
            m_saveOk = result.ok;
            if (result.ok)
                m_saveStatus = fmt::format(
                    "Saved {} entities ({} GLTF skipped) → {}",
                    result.entityCount, result.skippedCount, m_savePath
                );
            else
                m_saveStatus = fmt::format("Error: {}", result.error);
        }

        if (!m_saveStatus.empty()) {
            ImGui::SameLine();
            if (m_saveOk)
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", m_saveStatus.c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_saveStatus.c_str());
        }

        ImGui::End();
    }

    // =========================================================================
    // Helpers
    // =========================================================================
    std::string entityLabel(entt::registry& registry, entt::entity entity) {
        if (auto* name = registry.try_get<Vapor::NameComponent>(entity))
            return fmt::format("[{}] {}", entt::to_integral(entity), name->name);
        return fmt::format("[{}]", entt::to_integral(entity));
    }
};
