#pragma once

#include "Vapor/components.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/scene_serializer.hpp"
#include "Vapor/video_recorder.hpp"
#include "imgui.h"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <span>

namespace Vapor {

class SceneInspector {
public:
    using CustomDrawer     = std::function<void(entt::registry&, entt::entity)>;
    using CustomMenuDrawer = std::function<void(entt::registry&, entt::entity)>;

    // Returns the entity list to serialize. Default: all entities.
    using EntityProvider   = std::function<std::vector<entt::entity>(entt::registry&)>;

    void draw(entt::registry& registry) {
        if (m_videoRecorder && m_videoRecorder->isRecording())
            m_videoRecorder->captureFrame();
        drawEntityList(registry);
        drawInspector(registry);
    }

    void registerCustomDrawer(CustomDrawer drawer) {
        m_customDrawers.push_back(std::move(drawer));
    }

    void registerCustomMenuDrawer(CustomMenuDrawer drawer) {
        m_customMenuDrawers.push_back(std::move(drawer));
    }

    // Attach a configured SceneSerializer to enable the Save Scene section.
    // Call before the first draw(). The serializer must outlive this inspector.
    void attachSerializer(SceneSerializer& serializer) {
        m_serializer = &serializer;
    }

    // Override which entities get serialized when Save is pressed.
    // Default (no provider): every entity in the registry.
    void setEntityProvider(EntityProvider fn) {
        m_entityProvider = std::move(fn);
    }

    // Optionally pre-fill the GLTF path shown in the save section.
    void setGltfPath(const std::string& path, bool optimized = true) {
        strncpy(m_gltfPathBuf, path.c_str(), sizeof(m_gltfPathBuf) - 1);
        m_gltfOptimized = optimized;
    }

    // Attach a VideoRecorder to show the Recording section in the Scene panel.
    // baseOutputDir: directory where recordings are saved (e.g. SDL_GetBasePath() + "output").
    // Created on first Start press if it doesn't exist.
    void attachVideoRecorder(VideoRecorder& recorder, Renderer& renderer,
                             const std::string& baseOutputDir = "output") {
        m_videoRecorder  = &recorder;
        m_recorderRenderer = &renderer;
        m_recordingBaseDir = baseOutputDir;
        refreshRecordingPath();
    }

    void selectEntity(entt::entity entity) { m_selected = entity; }
    entt::entity getSelectedEntity() const  { return m_selected; }

private:
    entt::entity m_selected = entt::null;
    char m_searchBuf[128]   = {};
    std::vector<CustomDrawer>     m_customDrawers;
    std::vector<CustomMenuDrawer> m_customMenuDrawers;

    // Save section state — only active when m_serializer != nullptr
    SceneSerializer* m_serializer    = nullptr;
    EntityProvider   m_entityProvider;           // null → all entities
    char m_savePath[256]             = "scene.json";
    char m_gltfPathBuf[256]          = "";
    bool m_gltfOptimized             = true;
    std::string m_saveStatus;
    bool m_saveOk                    = true;

    // Recording section state — only active when m_videoRecorder != nullptr
    VideoRecorder* m_videoRecorder          = nullptr;
    Renderer*      m_recorderRenderer       = nullptr;
    std::string    m_recordingBaseDir       = "output";
    char           m_recordingOutputBuf[256] = {};
    std::string    m_recordingStatus;
    std::chrono::steady_clock::time_point m_recordingStartTime;

    // -------------------------------------------------------------------------
    // Left panel — entity list + save section
    // -------------------------------------------------------------------------
    void drawEntityList(entt::registry& registry) {
        ImGui::SetNextWindowSize(ImVec2(280, 560), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(10, 30),  ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Scene##inspector_list")) { ImGui::End(); return; }

        // --- Search ---
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

        // --- Entity actions ---
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

        // --- Save section (only if serializer attached) ---
        if (m_serializer) {
            ImGui::Separator();
            drawSaveSection(registry);
        }

        ImGui::End();

        // --- Recording section (only if recorder attached) ---
        if (m_videoRecorder) {
            ImGui::Begin("Engine");
            ImGui::Separator();
            drawRecordingSection();
            ImGui::End();
        }
    }

    // -------------------------------------------------------------------------
    // Save section — embedded in entity list panel
    // -------------------------------------------------------------------------
    void drawSaveSection(entt::registry& registry) {
        ImGui::TextDisabled("Save Scene");
        ImGui::InputText("##savepath",  m_savePath,    sizeof(m_savePath));
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            // Caller decides which entities to serialize;
            // default: every entity in the registry.
            std::vector<entt::entity> entities;
            if (m_entityProvider) {
                entities = m_entityProvider(registry);
            } else {
                for (auto e : registry.storage<entt::entity>())
                    entities.push_back(e);
            }

            auto r = m_serializer->save(
                registry,
                std::span(entities),
                std::string(m_gltfPathBuf),
                m_gltfOptimized,
                std::string(m_savePath)
            );
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
    // Right panel — component inspector for selected entity
    // -------------------------------------------------------------------------
    void drawInspector(entt::registry& registry) {
        ImGui::SetNextWindowSize(ImVec2(380, 500), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(300, 30),  ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Inspector##inspector_detail")) { ImGui::End(); return; }

        if (m_selected == entt::null || !registry.valid(m_selected)) {
            ImGui::TextDisabled("No entity selected");
            ImGui::End();
            return;
        }

        ImGui::Text("Entity %u", static_cast<Uint32>(entt::to_integral(m_selected)));
        ImGui::Separator();

        if (auto* c = registry.try_get<NameComponent>(m_selected))
            drawNameComponent(registry, *c);
        if (auto* c = registry.try_get<TransformComponent>(m_selected))
            drawTransformComponent(registry, *c);
        if (auto* c = registry.try_get<MeshRendererComponent>(m_selected))
            drawMeshRendererComponent(*c);
        if (auto* c = registry.try_get<RigidbodyComponent>(m_selected))
            drawRigidbodyComponent(*c);
        if (auto* c = registry.try_get<BoxColliderComponent>(m_selected))
            drawBoxColliderComponent(*c);
        if (auto* c = registry.try_get<SphereColliderComponent>(m_selected))
            drawSphereColliderComponent(*c);
        if (auto* c = registry.try_get<VirtualCameraComponent>(m_selected))
            drawVirtualCameraComponent(*c);
        if (auto* c = registry.try_get<FlyCameraComponent>(m_selected))
            drawFlyCameraComponent(*c);
        if (auto* c = registry.try_get<FollowCameraComponent>(m_selected))
            drawFollowCameraComponent(registry, *c);
        if (auto* c = registry.try_get<GrabberComponent>(m_selected))
            drawGrabberComponent(registry, *c);
        if (auto* c = registry.try_get<HeldByComponent>(m_selected))
            drawHeldByComponent(registry, *c);
        if (auto* c = registry.try_get<TriggerVolumeComponent>(m_selected))
            drawTriggerVolumeComponent(*c);
        if (auto* c = registry.try_get<CharacterBodyComponent>(m_selected))
            drawCharacterBodyComponent(*c);
        if (auto* c = registry.try_get<VehicleBodyComponent>(m_selected))
            drawVehicleBodyComponent(*c);

        for (auto& drawer : m_customDrawers)
            drawer(registry, m_selected);

        ImGui::Separator();
        drawAddComponentMenu(registry);

        ImGui::End();
    }

    // =========================================================================
    // Component drawers
    // =========================================================================

    void drawNameComponent(entt::registry& registry, NameComponent& c) {
        if (!ImGui::CollapsingHeader("Name", ImGuiTreeNodeFlags_DefaultOpen)) return;
        char buf[256];
        strncpy(buf, c.name.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText("Name##name_field", buf, sizeof(buf)))
            c.name = buf;
    }

    void drawTransformComponent(entt::registry& registry, TransformComponent& c) {
        if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) return;
        bool changed = false;
        changed |= ImGui::DragFloat3("Position", &c.position.x, 0.1f);
        glm::vec3 euler = glm::degrees(glm::eulerAngles(c.rotation));
        if (ImGui::DragFloat3("Rotation (°)", &euler.x, 0.5f)) {
            c.rotation = glm::quat(glm::radians(euler));
            changed = true;
        }
        changed |= ImGui::DragFloat3("Scale", &c.scale.x, 0.01f, 0.001f, 100.0f);
        if (changed) c.isDirty = true;
        if (c.parent != entt::null && registry.valid(c.parent))
            ImGui::LabelText("Parent", "%s", entityLabel(registry, c.parent).c_str());
    }

    void drawMeshRendererComponent(MeshRendererComponent& c) {
        if (!ImGui::CollapsingHeader("Mesh Renderer")) return;
        ImGui::Checkbox("Visible", &c.visible);
        ImGui::LabelText("Mesh count", "%zu", c.meshes.size());
    }

    void drawRigidbodyComponent(RigidbodyComponent& c) {
        if (!ImGui::CollapsingHeader("Rigidbody")) return;
        const char* motionNames[] = { "Static", "Dynamic", "Kinematic" };
        int motion = static_cast<int>(c.motionType);
        if (ImGui::Combo("Motion Type", &motion, motionNames, 3))
            c.motionType = static_cast<BodyMotionType>(motion);
        ImGui::Checkbox("Sync To Physics",   &c.syncToPhysics);
        ImGui::Checkbox("Sync From Physics", &c.syncFromPhysics);
        ImGui::LabelText("Body", c.body.valid() ? "valid" : "invalid");
    }

    void drawBoxColliderComponent(BoxColliderComponent& c) {
        if (!ImGui::CollapsingHeader("Box Collider")) return;
        ImGui::DragFloat3("Half Size", &c.halfSize.x, 0.01f, 0.001f, 100.0f);
    }

    void drawSphereColliderComponent(SphereColliderComponent& c) {
        if (!ImGui::CollapsingHeader("Sphere Collider")) return;
        ImGui::DragFloat("Radius", &c.radius, 0.01f, 0.001f, 100.0f);
    }

    void drawVirtualCameraComponent(VirtualCameraComponent& c) {
        if (!ImGui::CollapsingHeader("Virtual Camera")) return;
        ImGui::Checkbox("Active", &c.isActive);
        ImGui::DragFloat3("Position##vcam", &c.position.x, 0.1f);
        float fovDeg = glm::degrees(c.fov);
        if (ImGui::DragFloat("FOV (°)", &fovDeg, 0.5f, 5.0f, 170.0f))
            c.fov = glm::radians(fovDeg);
        ImGui::DragFloat("Near",   &c.near, 0.001f, 0.001f, 10.0f);
        ImGui::DragFloat("Far",    &c.far,  1.0f,   1.0f,   10000.0f);
        ImGui::LabelText("Aspect", "%.3f", c.aspect);
    }

    void drawFlyCameraComponent(FlyCameraComponent& c) {
        if (!ImGui::CollapsingHeader("Fly Camera")) return;
        ImGui::DragFloat("Move Speed##fly",   &c.moveSpeed,   0.1f, 0.1f, 100.0f);
        ImGui::DragFloat("Rotate Speed##fly", &c.rotateSpeed, 1.0f, 1.0f, 360.0f);
        ImGui::DragFloat("Yaw##fly",   &c.yaw,   0.5f);
        ImGui::DragFloat("Pitch##fly", &c.pitch, 0.5f, -89.0f, 89.0f);
    }

    void drawFollowCameraComponent(entt::registry& registry, FollowCameraComponent& c) {
        if (!ImGui::CollapsingHeader("Follow Camera")) return;
        if (c.target != entt::null && registry.valid(c.target))
            ImGui::LabelText("Target", "%s", entityLabel(registry, c.target).c_str());
        else
            ImGui::LabelText("Target", "(none)");
        ImGui::DragFloat3("Offset##follow", &c.offset.x, 0.1f);
        ImGui::DragFloat("Smooth Factor", &c.smoothFactor, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Deadzone",      &c.deadzone,     0.01f, 0.0f, 10.0f);
    }

    void drawGrabberComponent(entt::registry& registry, GrabberComponent& c) {
        if (!ImGui::CollapsingHeader("Grabber")) return;
        ImGui::DragFloat("Max Pickup Range##grab", &c.maxPickupRange, 0.1f, 0.1f, 100.0f);
        if (c.heldEntity != entt::null && registry.valid(c.heldEntity))
            ImGui::LabelText("Holding", "%s", entityLabel(registry, c.heldEntity).c_str());
        else
            ImGui::LabelText("Holding", "(none)");
    }

    void drawHeldByComponent(entt::registry& registry, HeldByComponent& c) {
        if (!ImGui::CollapsingHeader("Held By")) return;
        if (c.holder != entt::null && registry.valid(c.holder))
            ImGui::LabelText("Holder", "%s", entityLabel(registry, c.holder).c_str());
        ImGui::DragFloat("Hold Distance", &c.holdDistance, 0.1f, 0.1f, 20.0f);
        ImGui::LabelText("Orig Gravity",  "%.3f", c.originalGravityFactor);
    }

    void drawTriggerVolumeComponent(TriggerVolumeComponent& c) {
        if (!ImGui::CollapsingHeader("Trigger Volume")) return;
        ImGui::LabelText("Trigger", c.trigger.valid() ? "valid" : "invalid");
    }

    void drawCharacterBodyComponent(CharacterBodyComponent& c) {
        if (!ImGui::CollapsingHeader("Character Body")) return;
        ImGui::DragFloat("Height##cb", &c.settings.height, 0.1f, 0.1f, 10.0f);
        ImGui::DragFloat("Radius##cb", &c.settings.radius, 0.05f, 0.05f, 5.0f);
        ImGui::DragFloat("Mass##cb",   &c.settings.mass,   1.0f, 0.1f, 1000.0f);
        ImGui::LabelText("Desired Vel", "(%.2f, %.2f, %.2f)",
            c.desiredVelocity.x, c.desiredVelocity.y, c.desiredVelocity.z);
        ImGui::Checkbox("Jump Req", &c.jumpRequested);
        ImGui::LabelText("Controller", c.controller ? "Active" : "None");
    }

    void drawVehicleBodyComponent(VehicleBodyComponent& c) {
        if (!ImGui::CollapsingHeader("Vehicle Body")) return;
        ImGui::DragFloat("Throttle##vb", &c.throttle, 0.05f, -1.0f, 1.0f);
        ImGui::DragFloat("Steering##vb", &c.steering, 0.05f, -1.0f, 1.0f);
        ImGui::DragFloat("Brake##vb",    &c.brake,    0.05f,  0.0f, 1.0f);
        ImGui::Checkbox("Handbrake##vb", &c.handbrake);
        ImGui::LabelText("Controller", c.controller ? "Active" : "None");
    }

    // =========================================================================
    // Add component pop-up
    // =========================================================================
    void drawAddComponentMenu(entt::registry& registry) {
        if (!ImGui::Button("+ Add Component")) return;
        ImGui::OpenPopup("add_component_popup");

        if (ImGui::BeginPopup("add_component_popup")) {
            auto tryAdd = [&]<typename T>(const char* label) {
                if (!registry.all_of<T>(m_selected) && ImGui::MenuItem(label)) {
                    registry.emplace<T>(m_selected);
                    ImGui::CloseCurrentPopup();
                }
            };
            tryAdd.operator()<TransformComponent>("Transform");
            tryAdd.operator()<MeshRendererComponent>("Mesh Renderer");
            tryAdd.operator()<RigidbodyComponent>("Rigidbody");
            tryAdd.operator()<BoxColliderComponent>("Box Collider");
            tryAdd.operator()<SphereColliderComponent>("Sphere Collider");
            tryAdd.operator()<VirtualCameraComponent>("Virtual Camera");
            tryAdd.operator()<FlyCameraComponent>("Fly Camera");
            for (auto& drawer : m_customMenuDrawers)
                drawer(registry, m_selected);
            ImGui::EndPopup();
        }
    }

    // -------------------------------------------------------------------------
    // Recording section — embedded in entity list panel
    // -------------------------------------------------------------------------
    void refreshRecordingPath() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char filename[64];
        std::strftime(filename, sizeof(filename), "recording_%Y%m%d_%H%M%S.mp4", &tm);
        std::string path = (std::filesystem::path(m_recordingBaseDir) / filename).string();
        strncpy(m_recordingOutputBuf, path.c_str(), sizeof(m_recordingOutputBuf) - 1);
        m_recordingOutputBuf[sizeof(m_recordingOutputBuf) - 1] = '\0';
    }

    void drawRecordingSection() {
        ImGui::TextDisabled("Recording");

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##recpath", m_recordingOutputBuf, sizeof(m_recordingOutputBuf));

        const bool isRecording = m_videoRecorder->isRecording();

        if (!isRecording) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
            if (ImGui::Button("Start##rec", ImVec2(-1.0f, 0.0f))) {
                std::error_code ec;
                std::filesystem::create_directories(
                    std::filesystem::path(m_recordingOutputBuf).parent_path(), ec);

                VideoRecorder::Config cfg;
                cfg.outputPath = m_recordingOutputBuf;
                if (m_videoRecorder->startRecording(m_recorderRenderer, cfg)) {
                    m_recordingStartTime = std::chrono::steady_clock::now();
                    m_recordingStatus.clear();
                } else {
                    m_recordingStatus = "Failed to start (FFmpeg unavailable?)";
                }
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Stop##rec", ImVec2(-1.0f, 0.0f))) {
                m_videoRecorder->stopRecording();
                m_recordingStatus = fmt::format("Saved: {}", m_recordingOutputBuf);
                refreshRecordingPath(); // pre-fill next filename
            }
            ImGui::PopStyleColor();

            auto elapsed = std::chrono::steady_clock::now() - m_recordingStartTime;
            auto secs    = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "REC  %02lld:%02lld", secs / 60, secs % 60);
        }

        if (!m_recordingStatus.empty())
            ImGui::TextDisabled("%s", m_recordingStatus.c_str());
    }

    // =========================================================================
    // Helpers
    // =========================================================================
    std::string entityLabel(entt::registry& registry, entt::entity entity) {
        if (auto* name = registry.try_get<NameComponent>(entity))
            return fmt::format("[{}] {}", entt::to_integral(entity), name->name);
        return fmt::format("[{}]", entt::to_integral(entity));
    }
};

} // namespace Vapor
