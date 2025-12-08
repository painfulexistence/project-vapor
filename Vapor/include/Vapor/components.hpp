#pragma once

#include "graphics.hpp"
#include "physics_3d.hpp"
#include "scene.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

namespace Vapor {

    struct NameComponent {
        std::string name;
    };

    struct TransformComponent {
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale = glm::vec3(1.0f);

        // 緩存的世界變換矩陣（由 TransformSystem 計算）
        glm::mat4 worldTransform = glm::mat4(1.0f);
        bool isDirty = true;
    };

    // struct Hierarchy {
    //     entt::entity parent = entt::null;
    // };

    // Mesh rendering component
    struct MeshRendererComponent {
        std::string name;
        std::vector<std::shared_ptr<Mesh>> meshes;
        bool visible = true;
        bool castShadow = true;
        bool receiveShadow = true;
    };

    struct BoxColliderComponent {
        glm::vec3 halfSize = glm::vec3(0.5f);
    };

    struct SphereColliderComponent {
        float radius = 0.5f;
    };

    struct CapsuleColliderComponent {
        float radius = 0.5f;
        float halfHeight = 0.5f;
    };

    struct CylinderColliderComponent {
        float radius = 0.5f;
        float halfHeight = 0.5f;
    };

    struct RigidbodyComponent {
        BodyHandle body;
        BodyMotionType motionType = BodyMotionType::Dynamic;
        bool syncToPhysics = false;// Kinematic/Static 同步到物理
        bool syncFromPhysics = true;// Dynamic 從物理同步
        float gravityFactor = 1.0f;
        float mass = 1.0f;
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

    // Light components
    struct DirectionalLightComponent {
        DirectionalLight light;
    };

    struct PointLightComponent {
        PointLight light;
    };

    // Tag components for filtering
    struct Active {};
    struct Visible {};

    // 2D Sprite rendering component
    struct SpriteComponent {
        AtlasHandle atlas;
        uint16_t frameIndex = 0;

        glm::vec2 size = {1.0f, 1.0f};       // World units
        glm::vec2 pivot = {0.5f, 0.5f};      // Anchor point (0-1)
        glm::vec4 tint = {1, 1, 1, 1};       // Color tint
        int sortingLayer = 0;
        int orderInLayer = 0;
        bool flipX = false;
        bool flipY = false;
        bool visible = true;
    };

    // Flipbook animation component (drives any frame-based animation)
    struct FlipbookComponent {
        std::vector<uint16_t> frameIndices;
        float frameTime = 0.1f;              // Seconds per frame
        float timer = 0.0f;
        uint16_t currentIndex = 0;           // Index into frameIndices
        bool loop = true;
        bool playing = true;

        uint16_t getCurrentFrame() const {
            return frameIndices.empty() ? 0 : frameIndices[currentIndex];
        }
    };

}// namespace Vapor
