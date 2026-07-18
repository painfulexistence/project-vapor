#pragma once
#include <SDL3/SDL_stdinc.h>
#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class RenderScene;

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

enum class BodyMotionType {
    Static,
    Dynamic,
    Kinematic,
};

template<typename Tag> struct PhysicsHandle {
    Uint32 rid = UINT32_MAX;
    bool valid() const {
        return rid != UINT32_MAX;
    }
    bool operator==(const PhysicsHandle&) const = default;
};

struct BodyTag {};
struct TriggerTag {};
using BodyHandle = PhysicsHandle<BodyTag>;
using TriggerHandle = PhysicsHandle<TriggerTag>;

struct RaycastHit {
    glm::vec3 point;
    glm::vec3 normal;
    BodyHandle body;
    entt::entity entity = entt::null;
    float hitDistance;
    float hitFraction;
};

struct OverlapResult {
    std::vector<BodyHandle> bodies;
    std::vector<entt::entity> entities;
};

// ECS-mode collision events: bodies are identified by BodyHandle (resolve entity via getBodyUserData)
struct CollisionEvent {
    BodyHandle body1;
    BodyHandle body2;
    bool isEnter;
};

struct TriggerEvent {
    BodyHandle triggerBody;
    BodyHandle otherBody;
    bool isEnter;
};

class CharacterController;
class VehicleController;

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
    void process(float dt);
    void attach(entt::registry& reg);
    void process(entt::registry& reg, float dt);

    void registerCharacterController(CharacterController* ctrl);
    void unregisterCharacterController(CharacterController* ctrl);
    void registerVehicleController(VehicleController* ctrl);
    void unregisterVehicleController(VehicleController* ctrl);

    void setDebugEnabled(bool enabled);
    bool isDebugEnabled() const;
    void drawImGui(float dt);
    void deinit();

    // Get interpolation alpha for smooth rendering between physics steps
    float getInterpolationAlpha() const {
        return timeAccum / FIXED_TIME_STEP;
    }

    // ====== 創建剛體（各種形狀） ======
    BodyHandle
        createSphereBody(float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    BodyHandle createBoxBody(
        const glm::vec3& halfSize, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType
    );
    BodyHandle createCapsuleBody(
        float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType
    );
    BodyHandle createCylinderBody(
        float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType
    );
    BodyHandle createMeshBody(
        const std::vector<glm::vec3>& vertices,
        const std::vector<Uint32>& indices,
        const glm::vec3& position,
        const glm::quat& rotation,
        BodyMotionType motionType
    );
    BodyHandle createConvexHullBody(
        const std::vector<glm::vec3>& points,
        const glm::vec3& position,
        const glm::quat& rotation,
        BodyMotionType motionType
    );

    void addBody(BodyHandle body, bool activate = false);
    void removeBody(BodyHandle body);
    void destroyBody(BodyHandle body);

    bool raycast(const glm::vec3& from, const glm::vec3& to, RaycastHit& hit, BodyHandle ignoreBody = BodyHandle{});
    void setGravity(const glm::vec3& acc);
    glm::vec3 getGravity() const;

    // ====== ECS 碰撞事件（每幀 process() 後可取用，取完即清空） ======
    std::vector<CollisionEvent> popCollisionEvents();
    std::vector<TriggerEvent> popTriggerEvents();

    // ====== Trigger 創建 ======
    TriggerHandle createBoxTrigger(
        const glm::vec3& halfSize, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0)
    );
    TriggerHandle
        createSphereTrigger(float radius, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0));
    TriggerHandle createCapsuleTrigger(
        float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0)
    );
    void removeTrigger(TriggerHandle trigger);
    void destroyTrigger(TriggerHandle trigger);

    // ====== Trigger UserData 管理 ======
    void setTriggerUserData(TriggerHandle trigger, Uint64 userData);
    Uint64 getTriggerUserData(TriggerHandle trigger) const;

    // ====== 重疊測試 (Overlap Tests) ======
    OverlapResult overlapSphere(const glm::vec3& center, float radius);
    OverlapResult overlapBox(
        const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation = glm::quat(1, 0, 0, 0)
    );
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
    JPH::PhysicsSystem* getPhysicsSystem() {
        return physicsSystem.get();
    }
    JPH::BodyInterface* getBodyInterface() {
        return bodyInterface;
    }
    JPH::TempAllocatorImpl* getTempAllocator() {
        return tempAllocator.get();
    }
    JPH::BodyID getBodyID(BodyHandle handle) const;

private:
    constexpr static float FIXED_TIME_STEP = 1.0f / 60.0f;

    std::unordered_map<Uint32, JPH::BodyID> bodies;
    std::unordered_map<Uint32, Uint32> bodyIDToRid; // JPH::BodyID.GetIndexAndSequenceNumber() -> rid
    Uint32 nextBodyID = 0;

    std::vector<CollisionEvent> pendingCollisionEvents;
    std::vector<TriggerEvent> pendingTriggerEvents;
    std::mutex popMutex; // protects pendingCollisionEvents / pendingTriggerEvents

    std::unordered_map<Uint32, JPH::BodyID> triggers;
    Uint32 nextTriggerID = 0;

    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystem> jobSystem;// Owned by Physics3D
    std::unique_ptr<JPH::PhysicsSystem> physicsSystem;
    std::unique_ptr<BPLayerInterfaceImpl> broadPhaseLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> objectVsBroadphaseLayerFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> objectVsObjectLayerFilter;
    std::unique_ptr<JPH::ContactListener> contactListener;
    std::unique_ptr<JPH::BodyActivationListener> bodyActivationListener;
    std::unique_ptr<Vapor::PhysicsDebugRenderer> debugRenderer;
    bool debugDrawEnabled = false;

    JPH::BodyInterface* bodyInterface;

    std::vector<CharacterController*> characterControllers;
    std::vector<VehicleController*>   vehicleControllers;

    float timeAccum;
    Uint32 step;
    bool isInitialized = false;
    bool isDebugUIEnabled = false;
    glm::vec3 currentGravity = glm::vec3(0.0f, -9.81f, 0.0f);
};