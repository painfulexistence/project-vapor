#pragma once

// SceneSavePanel — lightweight ImGui window for serialising the current
// registry to a JSON scene file via SceneSerializer.
//
// Vaporware-specific; the full entity list / component inspector is handled
// by Vapor::SceneInspector (engine level).

#include "scene_serializer.hpp"
#include "imgui.h"
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <string>

class SceneSavePanel {
public:
    void draw(entt::registry& registry) {
        ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(10, 545), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Save Scene##save_panel")) {
            ImGui::End();
            return;
        }

        ImGui::InputText("Output path", m_savePath, sizeof(m_savePath));
        ImGui::InputText("GLTF path",   m_gltfPathBuf, sizeof(m_gltfPathBuf));
        ImGui::Checkbox("Optimized GLTF", &m_gltfOptimized);

        if (ImGui::Button("Save")) {
            auto result = SceneSerializer::save(
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

    // Call once after init so the save panel knows the source GLTF.
    void setGltfPath(const std::string& path, bool optimized = true) {
        strncpy(m_gltfPathBuf, path.c_str(), sizeof(m_gltfPathBuf) - 1);
        m_gltfOptimized = optimized;
    }

private:
    char m_savePath[256]    = "scene.json";
    char m_gltfPathBuf[256] = "models/Sponza/Sponza.gltf";
    bool m_gltfOptimized    = true;
    std::string m_saveStatus;
    bool m_saveOk = true;
};
