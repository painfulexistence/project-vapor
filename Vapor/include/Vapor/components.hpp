#pragma once

#include "Vapor/world.hpp"
#include "graphics.hpp"
#include "physics_3d.hpp"
#include "scene.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

namespace Vapor {

    struct TransformComponent {
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale = glm::vec3(1.0f);

        // 緩存的世界變換矩陣（由 TransformSystem 計算）
        glm::mat4 worldTransform = glm::mat4(1.0f);
        bool isDirty = true;

        // 父實體（用於層級變換）
        Entity parent = NULL_ENTITY;
    };

    struct MeshComponent {
        std::shared_ptr<MeshGroup> meshGroup = nullptr;
        glm::vec4 color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        bool visible = true;
        bool castShadow = true;
        bool receiveShadow = true;
    };

    struct RigidbodyComponent {
        BodyHandle body;
        BodyMotionType motionType = BodyMotionType::Dynamic;
        bool syncToPhysics = false;// Kinematic/Static 同步到物理
        bool syncFromPhysics = true;// Dynamic 從物理同步
        float gravityFactor = 1.0f;
    };

    struct TagComponent {
        std::string tag;
    };

    struct NameComponent {
        std::string name;
    };

    struct VirtualCameraComponent {
        float fov = glm::radians(60.0f);
        float aspect = 16.0f / 9.0f;
        float near = 0.05f;
        float far = 500.0f;

        bool isActive = false;

        // Self-contained Transform Data (Logic Driver Output)
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        // Output matrices
        glm::mat4 viewMatrix = glm::mat4(1.0f);
        glm::mat4 projectionMatrix = glm::mat4(1.0f);
    };

}// namespace Vapor
