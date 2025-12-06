#pragma once
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <SDL3/SDL_stdinc.h>
#include <unordered_map>

class Node;
class Scene;

namespace JPH {
    class TempAllocatorImpl;
    class JobSystem;
    class PhysicsSystem;
    class BodyInterface;
    class BodyID;
    class ContactListener;
    class BodyActivationListener;
    class BroadPhaseLayerInterface;
    class ObjectVsBroadPhaseLayerFilter;
    class ObjectLayerPairFilter;
}// namespace JPH

namespace Vapor {
    class JoltEnkiJobSystem;
    class TaskScheduler;
    class DebugDraw;
    class PhysicsDebugRenderer;
}// namespace Vapor

class BPLayerInterfaceImpl;
class ObjectVsBroadPhaseLayerFilterImpl;
class ObjectLayerPairFilterImpl;

enum class PhysicsDebugMode {
    NONE = 0,
    WIREFRAME = 1,
};

struct RaycastHit {
    glm::vec3 point;
    glm::vec3 normal;
    Node* node;
    float hitDistance;
    float hitFraction;
};

enum class BodyMotionType {
    Static,
    Dynamic,
    Kinematic,
};

struct BodyHandle {
    Uint32 rid = UINT32_MAX;

    bool valid() const {
        return rid != UINT32_MAX;
    }
};

struct TriggerHandle {
    Uint32 rid = UINT32_MAX;

    bool valid() const {
        return rid != UINT32_MAX;
    }
};

struct OverlapResult {
    std::vector<Node*> nodes;
    std::vector<BodyHandle> bodies;
};

class Physics3D {
private:
    static Physics3D* _instance;

public:
    static Physics3D* Get() {
        return _instance;
    }

    Physics3D();
    ~Physics3D();

    void init(Vapor::TaskScheduler& taskScheduler, std::shared_ptr<Vapor::DebugDraw> debugDraw = nullptr);
    void process(const std::shared_ptr<Scene>& scene, float dt);

    void setDebugEnabled(bool enabled);
    bool isDebugEnabled() const;
    void drawImGui(float dt);
    void deinit();

    // Get interpolation alpha for smooth rendering between physics steps
    float getInterpolationAlpha() const {
        return timeAccum / FIXED_TIME_STEP;
    }

    // ====== 創建剛體（各種形狀） ======
    BodyHandle createSphereBody(float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    BodyHandle createBoxBody(const glm::vec3& halfSize, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    BodyHandle createCapsuleBody(float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    BodyHandle createCylinderBody(float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    BodyHandle createMeshBody(const std::vector<glm::vec3>& vertices, const std::vector<Uint32>& indices, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    BodyHandle createConvexHullBody(const std::vector<glm::vec3>& points, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);

    void addBody(BodyHandle body, bool activate = false);
    void removeBody(BodyHandle body);
    void destroyBody(BodyHandle body);

    bool raycast(const glm::vec3& from, const glm::vec3& to, RaycastHit& hit);
    void setGravity(const glm::vec3& acc);
    glm::vec3 getGravity() const;

    // ====== Trigger 創建 ======
    TriggerHandle createBoxTrigger(const glm::vec3& halfSize, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
    TriggerHandle createSphereTrigger(float radius, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
    TriggerHandle createCapsuleTrigger(float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
    void removeTrigger(TriggerHandle trigger);
    void destroyTrigger(TriggerHandle trigger);

    // ====== Trigger UserData 管理 ======
    void setTriggerUserData(TriggerHandle trigger, Uint64 userData);
    Uint64 getTriggerUserData(TriggerHandle trigger) const;

    // ====== 重疊測試 (Overlap Tests) ======
    OverlapResult overlapSphere(const glm::vec3& center, float radius);
    OverlapResult overlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
    OverlapResult overlapCapsule(const glm::vec3& point1, const glm::vec3& point2, float radius);

    // ====== 力與力矩 ======
    void applyForce(BodyHandle body, const glm::vec3& force, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyCentralForce(BodyHandle body, const glm::vec3& force);
    void applyTorque(BodyHandle body, const glm::vec3& torque);
    void applyImpulse(BodyHandle body, const glm::vec3& impulse, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyCentralImpulse(BodyHandle body, const glm::vec3& impulse);
    void applyAngularImpulse(BodyHandle body, const glm::vec3& angularImpulse);

    // ====== 速度控制 ======
    void setLinearVelocity(BodyHandle body, const glm::vec3& vel);
    glm::vec3 getLinearVelocity(BodyHandle body) const;
    void setAngularVelocity(BodyHandle body, const glm::vec3& vel);
    glm::vec3 getAngularVelocity(BodyHandle body) const;

    // ====== 物理屬性 ======
    void setMass(BodyHandle body, float mass);
    float getMass(BodyHandle body) const;
    void setFriction(BodyHandle body, float friction);
    float getFriction(BodyHandle body) const;
    void setRestitution(BodyHandle body, float restitution);
    float getRestitution(BodyHandle body) const;
    void setLinearDamping(BodyHandle body, float damping);
    float getLinearDamping(BodyHandle body) const;
    void setAngularDamping(BodyHandle body, float damping);
    float getAngularDamping(BodyHandle body) const;

    // ====== 運動狀態 ======
    void setMotionType(BodyHandle body, BodyMotionType type);
    BodyMotionType getMotionType(BodyHandle body) const;
    void setGravityFactor(BodyHandle body, float factor);
    float getGravityFactor(BodyHandle body) const;

    // ====== 啟用/停用 ======
    void activateBody(BodyHandle body);
    void deactivateBody(BodyHandle body);
    bool isActive(BodyHandle body) const;

    // ====== 位置與旋轉 ======
    glm::vec3 getPosition(BodyHandle body) const;
    void setPosition(BodyHandle body, const glm::vec3& position);
    glm::quat getRotation(BodyHandle body) const;
    void setRotation(BodyHandle body, const glm::quat& rotation);

    // ====== UserData 管理（用於 raycast 和 trigger） ======
    void setBodyUserData(BodyHandle body, Uint64 userData);
    Uint64 getBodyUserData(BodyHandle body) const;

    // void drawDebug();

    void enableDebugUI(bool enable = true) {
        isDebugUIEnabled = enable;
    }

    // ====== 內部訪問器（供其他物理組件使用） ======
    JPH::PhysicsSystem* getPhysicsSystem() { return physicsSystem.get(); }
    JPH::BodyInterface* getBodyInterface() { return bodyInterface; }
    JPH::TempAllocatorImpl* getTempAllocator() { return tempAllocator.get(); }
    JPH::BodyID getBodyID(BodyHandle handle) const;

private:
    constexpr static float FIXED_TIME_STEP = 1.0f / 60.0f;

    // ====== 形狀快取系統 ======
    struct ShapeDesc {
        enum Type { Sphere, Box, Capsule, Cylinder } type;
        glm::vec3 dimensions;  // Sphere: (radius, 0, 0), Box: (hx, hy, hz), Capsule: (halfHeight, radius, 0), Cylinder: (halfHeight, radius, 0)

        bool operator==(const ShapeDesc& other) const {
            return type == other.type &&
                   glm::all(glm::epsilonEqual(dimensions, other.dimensions, 0.001f));
        }
    };

    struct ShapeDescHash {
        std::size_t operator()(const ShapeDesc& desc) const {
            return std::hash<int>()(static_cast<int>(desc.type)) ^
                   (std::hash<float>()(desc.dimensions.x) << 1) ^
                   (std::hash<float>()(desc.dimensions.y) << 2) ^
                   (std::hash<float>()(desc.dimensions.z) << 3);
        }
    };

    std::unordered_map<Uint32, JPH::BodyID> bodies;
    Uint32 nextBodyID = 0;

    std::unordered_map<Uint32, JPH::BodyID> triggers;
    Uint32 nextTriggerID = 0;

    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<Vapor::JoltEnkiJobSystem> jobSystem; // Owned by Physics3D
    std::unique_ptr<JPH::PhysicsSystem> physicsSystem;
    std::unique_ptr<BPLayerInterfaceImpl> broad_phase_layer_interface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> object_vs_broadphase_layer_filter;
    std::unique_ptr<ObjectLayerPairFilterImpl> object_vs_object_layer_filter;
    std::unique_ptr<JPH::ContactListener> contactListener;
    std::unique_ptr<JPH::BodyActivationListener> bodyActivationListener;
    std::unique_ptr<Vapor::PhysicsDebugRenderer> debugRenderer;
    bool debugDrawEnabled = false;

    JPH::BodyInterface* bodyInterface;

    float timeAccum;
    Uint32 step;
    bool isInitialized = false;
    bool isDebugUIEnabled = false;
    glm::vec3 currentGravity = glm::vec3(0.0f, -9.81f, 0.0f);
};