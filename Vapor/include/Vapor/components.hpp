#pragma once

#include "graphics.hpp"
#include "physics_3d.hpp"
#include "scene.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <unordered_map>

// Forward declarations for animation types
namespace Vapor {
class Skeleton;
class Animator;
class AnimationClip;
}

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

    // =========================================================================
    // Animation Components
    // =========================================================================

    /**
     * @brief Skinned mesh renderer component for skeletal animation.
     *
     * Contains skinned meshes with joint weights. Used together with
     * AnimatorComponent for animated characters.
     */
    struct SkinnedMeshRendererComponent {
        std::string name;
        std::vector<std::shared_ptr<SkinnedMesh>> meshes;
        std::shared_ptr<Skeleton> skeleton;
        bool visible = true;
        bool castShadow = true;
        bool receiveShadow = true;

        // GPU buffer offset for this instance's bone matrices
        Uint32 boneMatrixOffset = 0;
    };

    /**
     * @brief Animator component for controlling skeletal animations.
     *
     * Manages animation playback, blending, and state for an entity.
     * Designed for per-entity animation control.
     */
    struct AnimatorComponent {
        std::shared_ptr<Animator> animator;

        // Quick access to common operations
        bool isPlaying() const;
        void play(const std::string& animationName, bool loop = true, float blendTime = 0.0f);
        void stop();
        void pause();
        void resume();
        void setSpeed(float speed);
    };

    /**
     * @brief Animation state machine component for complex animation logic.
     *
     * Optional component for entities needing state-based animation control
     * (e.g., Idle -> Walk -> Run transitions).
     */
    struct AnimationStateMachineComponent {
        std::string currentState;
        std::string previousState;
        float transitionTime = 0.0f;
        float transitionDuration = 0.2f;
        bool isTransitioning = false;

        // State-to-animation mapping
        std::unordered_map<std::string, std::string> stateAnimations;

        // Transition rules: from -> to -> blend time
        std::unordered_map<std::string, std::unordered_map<std::string, float>> transitionRules;

        void setState(const std::string& newState);
        void addStateAnimation(const std::string& state, const std::string& animationName);
        void addTransition(const std::string& from, const std::string& to, float blendTime);
    };

    /**
     * @brief Bone attachment component for attaching objects to animated bones.
     *
     * Use this to attach weapons, accessories, or effects to specific bones.
     */
    struct BoneAttachmentComponent {
        std::string boneName;
        int boneIndex = -1;  // Cached bone index for performance
        glm::vec3 localOffset = glm::vec3(0.0f);
        glm::quat localRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 localScale = glm::vec3(1.0f);

        // Cached world transform (updated by animation system)
        glm::mat4 worldTransform = glm::mat4(1.0f);
    };

    // Tag components for filtering
    struct Active {};
    struct Visible {};

}// namespace Vapor
