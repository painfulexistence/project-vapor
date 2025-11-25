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
}

namespace Vapor {
    class JoltEnkiJobSystem;
}

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

class Physics3D {
private:
    static Physics3D* _instance;

public:
    static Physics3D* Get() {
        return _instance;
    }

    Physics3D();
    ~Physics3D();

    void init(Vapor::JoltEnkiJobSystem* jobSystem = nullptr);
    void process(const std::shared_ptr<Scene>& scene, float dt);
    void drawImGui(float dt);
    void deinit();

    BodyHandle createSphereBody(float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    BodyHandle createBoxBody(const glm::vec3& halfSize, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType);
    void addBody(BodyHandle body, bool activate = false);
    void removeBody(BodyHandle body);
    void destroyBody(BodyHandle body);

    bool raycast(const glm::vec3& from, const glm::vec3& to, RaycastHit& hit);
    void setGravity(const glm::vec3& acc);

    // void drawDebug();

    void enableDebugUI(bool enable = true) {
        isDebugUIEnabled = enable;
    }

private:
    constexpr static float FIXED_TIME_STEP = 1.0f / 60.0f;

    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    Vapor::JoltEnkiJobSystem* jobSystem; // Non-owning pointer, managed by EngineCore
    std::unique_ptr<JPH::PhysicsSystem> physicsSystem;
    std::unique_ptr<BPLayerInterfaceImpl> broad_phase_layer_interface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> object_vs_broadphase_layer_filter;
    std::unique_ptr<ObjectLayerPairFilterImpl> object_vs_object_layer_filter;
    std::unique_ptr<JPH::ContactListener> contactListener;
    std::unique_ptr<JPH::BodyActivationListener> bodyActivationListener;

    JPH::BodyInterface* bodyInterface;

    float timeAccum;
    Uint32 step;
    bool isInitialized = false;
    bool isDebugUIEnabled = false;

    Uint32 nextBodyID = 0;
    std::unordered_map<Uint32, JPH::BodyID> bodies;
};