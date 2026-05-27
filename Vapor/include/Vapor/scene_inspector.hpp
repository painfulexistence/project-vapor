#pragma once

#include "Vapor/components.hpp"
#include "Vapor/physics_3d.hpp"
#include "imgui.h"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <string>
#include <vector>
#include <functional>

namespace Vapor {

class SceneInspector {
public:
    using CustomDrawer = std::function<void(entt::registry&, entt::entity)>;
    using CustomMenuDrawer = std::function<void(entt::registry&, entt::entity)>;

    void draw(entt::registry& registry) {
        drawEntityList(registry);
        drawInspector(registry);
    }

    void registerCustomDrawer(CustomDrawer drawer) {
        m_customDrawers.push_back(std::move(drawer));
    }

    void registerCustomMenuDrawer(CustomMenuDrawer drawer) {
        m_customMenuDrawers.push_back(std::move(drawer));
    }

    void selectEntity(entt::entity entity) {
        m_selected = entity;
    }

    entt::entity getSelectedEntity() const {
        return m_selected;
    }

private:
    entt::entity m_selected = entt::null;
    char m_searchBuf[128] = {};
    std::vector<CustomDrawer> m_customDrawers;
    std::vector<CustomMenuDrawer> m_customMenuDrawers;

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

        for (auto entity : registry.storage<entt::entity>()) {
            std::string label = entityLabel(registry, entity);
            std::string labelLower = label;
            for (auto& c : labelLower) c = static_cast<char>(tolower(c));

            if (!filter.empty() && labelLower.find(filter) == std::string::npos)
                continue;

            bool selected = (entity == m_selected);
            ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
            if (ImGui::Selectable(label.c_str(), selected))
                m_selected = entity;
            ImGui::PopID();
        }

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

        // --- Core Engine components ---
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

        if (auto* c = registry.try_get<Vapor::CharacterBodyComponent>(m_selected))
            drawCharacterBodyComponent(*c);

        if (auto* c = registry.try_get<Vapor::VehicleBodyComponent>(m_selected))
            drawVehicleBodyComponent(*c);

        // --- Custom registered components ---
        for (auto& drawer : m_customDrawers) {
            drawer(registry, m_selected);
        }

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
    }

    void drawCharacterBodyComponent(Vapor::CharacterBodyComponent& c) {
        if (!ImGui::CollapsingHeader("Character Body")) return;
        ImGui::DragFloat("Height##cb", &c.settings.height, 0.1f, 0.1f, 10.0f);
        ImGui::DragFloat("Radius##cb", &c.settings.radius, 0.05f, 0.05f, 5.0f);
        ImGui::DragFloat("Mass##cb", &c.settings.mass, 1.0f, 0.1f, 1000.0f);
        ImGui::LabelText("Desired Vel", "(%.2f, %.2f, %.2f)", c.desiredVelocity.x, c.desiredVelocity.y, c.desiredVelocity.z);
        ImGui::Checkbox("Jump Req", &c.jumpRequested);
        ImGui::LabelText("Controller", c.controller ? "Active" : "None");
    }

    void drawVehicleBodyComponent(Vapor::VehicleBodyComponent& c) {
        if (!ImGui::CollapsingHeader("Vehicle Body")) return;
        ImGui::DragFloat("Throttle##vb", &c.throttle, 0.05f, -1.0f, 1.0f);
        ImGui::DragFloat("Steering##vb", &c.steering, 0.05f, -1.0f, 1.0f);
        ImGui::DragFloat("Brake##vb", &c.brake, 0.05f, 0.0f, 1.0f);
        ImGui::Checkbox("Handbrake##vb", &c.handbrake);
        ImGui::LabelText("Controller", c.controller ? "Active" : "None");
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

            // Draw custom component additions
            for (auto& drawer : m_customMenuDrawers) {
                drawer(registry, m_selected);
            }
            ImGui::EndPopup();
        }
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

} // namespace Vapor
