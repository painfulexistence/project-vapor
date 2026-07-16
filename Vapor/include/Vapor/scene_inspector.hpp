#pragma once

#include "Vapor/component_reflector.hpp"
#include "Vapor/components.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/scene_serializer.hpp"
#include "imgui.h"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <span>

namespace Vapor {

class SceneInspector {
public:
    using CustomDrawer     = std::function<void(entt::registry&, entt::entity)>;
    using CustomMenuDrawer = std::function<void(entt::registry&, entt::entity)>;
    using EntityProvider   = std::function<std::vector<entt::entity>(entt::registry&)>;
    // Draws the "Systems" section (system-level, non-per-entity controls).
    using SystemsDrawer    = std::function<void(entt::registry&)>;

    SceneInspector() {
        // Register all built-in engine components for auto-draw.
        registerComponent<NameComponent>("Name");
        registerComponent<TransformComponent>("Transform");
        registerComponent<MeshRendererComponent>("Mesh Renderer", false); // not trivially addable
        registerComponent<RigidbodyComponent>("Rigidbody");
        registerComponent<BoxColliderComponent>("Box Collider");
        registerComponent<SphereColliderComponent>("Sphere Collider");
        registerComponent<VirtualCameraComponent>("Virtual Camera");
        registerComponent<FlyCameraComponent>("Fly Camera");
        registerComponent<FollowCameraComponent>("Follow Camera");
        registerComponent<GrabberComponent>("Grabber");
        registerComponent<HeldByComponent>("Held By");
        registerComponent<TriggerVolumeComponent>("Trigger Volume", false);
        registerComponent<CharacterBodyComponent>("Character Body", false);
        registerComponent<VehicleBodyComponent>("Vehicle Body", false);
        registerComponent<SpriteComponent>("Sprite");
        registerComponent<FlipbookComponent>("Flipbook");
    }

    // -------------------------------------------------------------------------
    // Main entry point — called from the renderer's ImGui callback each frame.
    // Compiled out entirely in release builds.
    // -------------------------------------------------------------------------
    void draw(entt::registry& registry) {
#ifndef NDEBUG
        // F1 visibility is owned globally by the renderer (m_imGuiVisible),
        // which gates the whole engine overlay including this inspector. Do not
        // toggle on F1 here too, or the same key press flips both states in the
        // frame the overlay re-appears. m_visible stays for programmatic hiding.
        if (!m_visible) return;

        ImGui::SetNextWindowSize(ImVec2(680, 580), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(10, 30),   ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Application##debug_ui")) { ImGui::End(); return; }

        // Left panel — entity list + systems + save
        ImGui::BeginChild("##scene_panel", ImVec2(280, 0), true);
        drawEntityListContent(registry);
        if (m_systemsDrawer) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Systems"))
                m_systemsDrawer(registry);
        }
        if (m_serializer) {
            ImGui::Separator();
            drawSaveSection(registry);
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel — component inspector
        ImGui::BeginChild("##inspector_panel", ImVec2(0, 0), true);
        drawInspectorContent(registry);
        ImGui::EndChild();

        ImGui::End();
#endif
    }

    // -------------------------------------------------------------------------
    // Registration API
    // -------------------------------------------------------------------------

    // Register a component type for automatic field-by-field drawing via
    // Boost.PFR. All aggregate-compatible field types are displayed; others
    // are silently skipped.
    template<typename T>
    void registerComponent(const char* displayName, bool addable = true) {
        ComponentEntry entry;
        entry.typeId      = entt::type_hash<T>::value();
        entry.displayName = displayName;
        entry.hasComponent = [](entt::registry& reg, entt::entity e) {
            return reg.all_of<T>(e);
        };
        entry.draw = [dn = std::string(displayName)](entt::registry& reg, entt::entity e) {
            if (auto* c = reg.try_get<T>(e)) {
                ImGui::PushID(static_cast<int>(entt::type_hash<T>::value()));
                if (ImGui::CollapsingHeader(dn.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                    drawComponentFields(*c);
                ImGui::PopID();
            }
        };
        if (addable) {
            entry.addComponent = [](entt::registry& reg, entt::entity e) {
                if (!reg.all_of<T>(e)) reg.emplace<T>(e);
            };
        }
        m_componentEntries.push_back(std::move(entry));
    }

    // Register a fully custom per-entity draw callback.
    // Use this when auto-draw isn't sufficient (combos, cross-entity lookups,
    // progress bars, colored headers for tag components, etc.).
    void registerCustomDrawer(CustomDrawer drawer) {
        m_customDrawers.push_back(std::move(drawer));
    }

    void registerCustomMenuDrawer(CustomMenuDrawer drawer) {
        m_customMenuDrawers.push_back(std::move(drawer));
    }

    void attachSerializer(SceneSerializer& serializer) {
        m_serializer = &serializer;
    }

    void setEntityProvider(EntityProvider fn) {
        m_entityProvider = std::move(fn);
    }

    // Register the "Systems" section content (global, system-level controls).
    // This is the ECS-conventional counterpart to the per-entity inspector.
    void setSystemsDrawer(SystemsDrawer fn) {
        m_systemsDrawer = std::move(fn);
    }

    void setGltfPath(const std::string& path, bool optimized = true) {
        strncpy(m_gltfPathBuf, path.c_str(), sizeof(m_gltfPathBuf) - 1);
        m_gltfOptimized = optimized;
    }

    void selectEntity(entt::entity entity) { m_selected = entity; }
    entt::entity getSelectedEntity() const  { return m_selected; }

private:
    // -------------------------------------------------------------------------
    // Component entry — one per registered component type.
    // -------------------------------------------------------------------------
    struct ComponentEntry {
        entt::id_type typeId;
        std::string   displayName;
        std::function<bool(entt::registry&, entt::entity)> hasComponent;
        std::function<void(entt::registry&, entt::entity)> draw;
        std::function<void(entt::registry&, entt::entity)> addComponent; // null → not addable
    };

    bool         m_visible  = true;
    entt::entity m_selected = entt::null;
    char m_searchBuf[128]   = {};

    std::vector<ComponentEntry>   m_componentEntries;
    std::vector<CustomDrawer>     m_customDrawers;
    std::vector<CustomMenuDrawer> m_customMenuDrawers;
    SystemsDrawer                 m_systemsDrawer;

    // Save section state
    SceneSerializer* m_serializer   = nullptr;
    EntityProvider   m_entityProvider;
    char m_savePath[256]            = "scene.json";
    char m_gltfPathBuf[256]         = "";
    bool m_gltfOptimized            = true;
    std::string m_saveStatus;
    bool m_saveOk                   = true;

    // -------------------------------------------------------------------------
    // Left panel — entity list content
    // -------------------------------------------------------------------------
    void drawEntityListContent(entt::registry& registry) {
        ImGui::TextDisabled("Scene");
        ImGui::InputText("Search", m_searchBuf, sizeof(m_searchBuf));
        ImGui::Separator();

        std::string filter(m_searchBuf);
        for (auto& c : filter) c = static_cast<char>(tolower(c));

        for (auto entity : registry.storage<entt::entity>()) {
            std::string label = entityLabel(registry, entity);
            std::string lower = label;
            for (auto& c : lower) c = static_cast<char>(tolower(c));
            if (!filter.empty() && lower.find(filter) == std::string::npos) continue;

            bool selected = (entity == m_selected);
            ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
            if (ImGui::Selectable(label.c_str(), selected)) m_selected = entity;
            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button("Create Entity")) {
            m_selected = registry.create();
            registry.emplace<NameComponent>(m_selected, NameComponent{ "New Entity" });
            registry.emplace<TransformComponent>(m_selected);
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
    }

    // -------------------------------------------------------------------------
    // Save section
    // -------------------------------------------------------------------------
    void drawSaveSection(entt::registry& registry) {
        ImGui::TextDisabled("Save Scene");
        ImGui::InputText("##savepath", m_savePath, sizeof(m_savePath));
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            std::vector<entt::entity> entities;
            if (m_entityProvider) {
                entities = m_entityProvider(registry);
            } else {
                for (auto e : registry.storage<entt::entity>())
                    entities.push_back(e);
            }
            auto r = m_serializer->save(
                registry, std::span(entities),
                std::string(m_gltfPathBuf), m_gltfOptimized,
                std::string(m_savePath));
            m_saveOk     = r.ok;
            m_saveStatus = r.ok
                ? fmt::format("OK  {} entities", r.entityCount)
                : fmt::format("ERR  {}", r.error);
        }
        ImGui::InputText("GLTF path##gltfpath", m_gltfPathBuf, sizeof(m_gltfPathBuf));
        ImGui::Checkbox("Optimized##gltfopt", &m_gltfOptimized);
        if (!m_saveStatus.empty()) {
            auto col = m_saveOk ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::TextColored(col, "%s", m_saveStatus.c_str());
        }
    }

    // -------------------------------------------------------------------------
    // Right panel — inspector content
    // -------------------------------------------------------------------------
    void drawInspectorContent(entt::registry& registry) {
        ImGui::TextDisabled("Inspector");
        ImGui::Separator();

        if (m_selected == entt::null || !registry.valid(m_selected)) {
            ImGui::TextDisabled("No entity selected");
            return;
        }

        ImGui::Text("Entity %u", static_cast<uint32_t>(entt::to_integral(m_selected)));
        ImGui::Separator();

        // Registered auto-draw components
        for (auto& entry : m_componentEntries)
            entry.draw(registry, m_selected);

        // Fully custom drawers (combos, cross-entity lookups, tag headers, etc.)
        for (auto& drawer : m_customDrawers)
            drawer(registry, m_selected);

        ImGui::Separator();
        drawAddComponentMenu(registry);
    }

    // -------------------------------------------------------------------------
    // Add Component popup — auto-built from registered entries
    // -------------------------------------------------------------------------
    void drawAddComponentMenu(entt::registry& registry) {
        if (!ImGui::Button("+ Add Component")) return;
        ImGui::OpenPopup("add_component_popup");

        if (ImGui::BeginPopup("add_component_popup")) {
            for (auto& entry : m_componentEntries) {
                if (entry.addComponent && !entry.hasComponent(registry, m_selected)) {
                    if (ImGui::MenuItem(entry.displayName.c_str())) {
                        entry.addComponent(registry, m_selected);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            for (auto& drawer : m_customMenuDrawers)
                drawer(registry, m_selected);
            ImGui::EndPopup();
        }
    }

    // -------------------------------------------------------------------------
    // Recording section
    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    std::string entityLabel(entt::registry& registry, entt::entity entity) {
        if (auto* name = registry.try_get<NameComponent>(entity))
            return fmt::format("[{}] {}", entt::to_integral(entity), name->name);
        return fmt::format("[{}]", entt::to_integral(entity));
    }
};

} // namespace Vapor
