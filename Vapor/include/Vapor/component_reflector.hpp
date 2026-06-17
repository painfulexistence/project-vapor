#pragma once
#include "Vapor/physics_3d.hpp"
#include "imgui.h"
#include <boost/pfr.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <string>
#include <string_view>
#include <type_traits>

namespace Vapor {

// Forward declaration for mutual recursion between drawField and drawComponentFields.
template<typename T> bool drawComponentFields(T& comp);

// ---------------------------------------------------------------------------
// drawField — draw one named field with an appropriate ImGui widget.
// Returns true if the value was modified.
// Unknown/complex types are silently skipped.
// ---------------------------------------------------------------------------
template<typename T>
bool drawField(std::string_view name, T& value) {
    using V = std::remove_cvref_t<T>;

    if constexpr (std::is_same_v<V, float>) {
        return ImGui::DragFloat(name.data(), &value, 0.01f);

    } else if constexpr (std::is_same_v<V, double>) {
        float f = static_cast<float>(value);
        if (ImGui::DragFloat(name.data(), &f, 0.01f)) { value = f; return true; }
        return false;

    } else if constexpr (std::is_same_v<V, bool>) {
        return ImGui::Checkbox(name.data(), &value);

    } else if constexpr (std::is_same_v<V, int> || std::is_same_v<V, int32_t>) {
        return ImGui::DragInt(name.data(), &value);

    } else if constexpr (std::is_same_v<V, uint32_t> || std::is_same_v<V, uint16_t> ||
                         std::is_same_v<V, uint8_t>) {
        int i = static_cast<int>(value);
        if (ImGui::DragInt(name.data(), &i, 1, 0)) { value = static_cast<V>(i); return true; }
        return false;

    } else if constexpr (std::is_same_v<V, glm::vec2>) {
        return ImGui::DragFloat2(name.data(), &value.x, 0.01f);

    } else if constexpr (std::is_same_v<V, glm::vec3>) {
        return ImGui::DragFloat3(name.data(), &value.x, 0.01f);

    } else if constexpr (std::is_same_v<V, glm::vec4>) {
        return ImGui::DragFloat4(name.data(), &value.x, 0.01f);

    } else if constexpr (std::is_same_v<V, glm::quat>) {
        glm::vec3 euler = glm::degrees(glm::eulerAngles(value));
        if (ImGui::DragFloat3(name.data(), &euler.x, 0.5f)) {
            value = glm::quat(glm::radians(euler));
            return true;
        }
        return false;

    } else if constexpr (std::is_same_v<V, std::string>) {
        char buf[256] = {};
        strncpy(buf, value.c_str(), sizeof(buf) - 1);
        if (ImGui::InputText(name.data(), buf, sizeof(buf))) { value = buf; return true; }
        return false;

    } else if constexpr (std::is_same_v<V, entt::entity>) {
        if (value == entt::null)
            ImGui::LabelText(name.data(), "null");
        else
            ImGui::LabelText(name.data(), "%u", static_cast<uint32_t>(entt::to_integral(value)));
        return false;

    } else if constexpr (std::is_same_v<V, BodyHandle> || std::is_same_v<V, TriggerHandle>) {
        ImGui::LabelText(name.data(), value.valid() ? "valid (%u)" : "invalid", value.rid);
        return false;

    } else if constexpr (std::is_enum_v<V>) {
        int i = static_cast<int>(value);
        if (ImGui::DragInt(name.data(), &i)) { value = static_cast<V>(i); return true; }
        return false;

    } else if constexpr (std::is_aggregate_v<V> && !std::is_array_v<V>) {
        // Nested aggregate struct — recurse into its fields under a tree node.
        bool changed = false;
        if (ImGui::TreeNodeEx(name.data(), ImGuiTreeNodeFlags_DefaultOpen))  {
            changed = drawComponentFields(value);
            ImGui::TreePop();
        }
        return changed;

    } else {
        // Pointer, vector, unique_ptr, etc. — skip silently.
        (void)name; (void)value;
        return false;
    }
}

// ---------------------------------------------------------------------------
// drawComponentFields — iterate every field of an aggregate struct via
// Boost.PFR and call drawField for each one.
// Non-aggregate types (e.g. those with user-provided constructors) are
// guarded by std::is_aggregate_v and produce no output.
// ---------------------------------------------------------------------------
template<typename T>
bool drawComponentFields(T& comp) {
    if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
        constexpr size_t N = boost::pfr::tuple_size_v<T>;
        bool changed = false;
        [&]<size_t... Is>(std::index_sequence<Is...>) {
            ((changed |= drawField(
                boost::pfr::get_name<Is, T>(),
                boost::pfr::get<Is>(comp)
            )), ...);
        }(std::make_index_sequence<N>{});
        return changed;
    }
    return false;
}

} // namespace Vapor
