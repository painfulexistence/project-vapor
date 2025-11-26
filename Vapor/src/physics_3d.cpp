#include "physics_3d.hpp"
#include "character_controller.hpp"
#include "vehicle_controller.hpp"
#include "fluid_volume.hpp"
#include "jolt_enki_job_system.hpp"
#include "task_scheduler.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <SDL3/SDL_stdinc.h>
#include <fmt/core.h>
#include <thread>

// #include "physics_debug_drawer.hpp"
#include "scene.hpp"

JPH_SUPPRESS_WARNINGS

using namespace JPH::literals;// for real value _r suffix

static void TraceImpl(const char* inFMT, ...) {
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);

    fmt::print("{}", buffer);
}

static constexpr JPH::EMotionType convertMotionType(BodyMotionType motionType) {
    switch (motionType) {
    case BodyMotionType::Static:
        return JPH::EMotionType::Static;
    case BodyMotionType::Dynamic:
        return JPH::EMotionType::Dynamic;
    case BodyMotionType::Kinematic:
        return JPH::EMotionType::Kinematic;
    default:
        return JPH::EMotionType::Static;
    }
}

namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer TRIGGER = 2;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 3;
};// namespace Layers

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        switch (inObject1) {
        case Layers::NON_MOVING:
            return inObject2 == Layers::MOVING;  // Static only collides with Dynamic
        case Layers::MOVING:
            return true;  // Dynamic collides with all layers (including Trigger)
        case Layers::TRIGGER:
            return inObject2 == Layers::MOVING;  // Trigger only detects Dynamic objects
        default:
            JPH_ASSERT(false);
            return false;
        }
    }
};

namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr JPH::BroadPhaseLayer TRIGGER(2);
    static constexpr uint NUM_LAYERS(3);
};// namespace BroadPhaseLayers

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
        mObjectToBroadPhase[Layers::TRIGGER] = BroadPhaseLayers::TRIGGER;
    }

    virtual uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
        return mObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch ((JPH::BroadPhaseLayer::Type)inLayer) {
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
            return "NON_MOVING";
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
            return "MOVING";
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::TRIGGER:
            return "TRIGGER";
        default:
            JPH_ASSERT(false);
            return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
        case Layers::NON_MOVING:
            return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING:
            return true;
        case Layers::TRIGGER:
            return inLayer2 == BroadPhaseLayers::MOVING;
        default:
            JPH_ASSERT(false);
            return false;
        }
    }
};

class MyContactListener : public JPH::ContactListener {
public:
    struct TriggerEvent {
        Node* triggerNode;
        Node* otherNode;
        bool isEnter;  // true = Enter, false = Exit
    };

    struct CollisionEvent {
        Node* node1;
        Node* node2;
        bool isEnter;
    };

    std::vector<TriggerEvent> triggerEvents;
    std::vector<CollisionEvent> collisionEvents;
    std::unordered_map<uint64_t, bool> activeContacts;  // Track active contacts for exit detection

    // Helper to create unique contact ID
    uint64_t makeContactID(JPH::BodyID id1, JPH::BodyID id2) const {
        uint32_t a = id1.GetIndexAndSequenceNumber();
        uint32_t b = id2.GetIndexAndSequenceNumber();
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32) | uint64_t(b);
    }

    virtual JPH::ValidateResult OnContactValidate(
      const JPH::Body& inBody1,
      const JPH::Body& inBody2,
      JPH::RVec3Arg inBaseOffset,
      const JPH::CollideShapeResult& inCollisionResult
    ) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    virtual void OnContactAdded(
      const JPH::Body& inBody1,
      const JPH::Body& inBody2,
      const JPH::ContactManifold& inManifold,
      JPH::ContactSettings& ioSettings
    ) override {
        auto node1 = reinterpret_cast<Node*>(inBody1.GetUserData());
        auto node2 = reinterpret_cast<Node*>(inBody2.GetUserData());

        if (!node1 || !node2) return;

        bool isSensor1 = inBody1.IsSensor();
        bool isSensor2 = inBody2.IsSensor();

        uint64_t contactID = makeContactID(inBody1.GetID(), inBody2.GetID());
        activeContacts[contactID] = true;

        // Trigger event (one or both are sensors)
        if (isSensor1 || isSensor2) {
            Node* triggerNode = isSensor1 ? node1 : node2;
            Node* otherNode = isSensor1 ? node2 : node1;
            triggerEvents.push_back({triggerNode, otherNode, true});
        } else {
            // Regular collision event
            collisionEvents.push_back({node1, node2, true});
        }
    }

    virtual void OnContactPersisted(
      const JPH::Body& inBody1,
      const JPH::Body& inBody2,
      const JPH::ContactManifold& inManifold,
      JPH::ContactSettings& ioSettings
    ) override {
        // Contact still active, no need to add event
    }

    virtual void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override {
        // Note: We can't get UserData here directly, so we need to handle this differently
        // We'll mark the contact as removed and process it in the next physics update
    }

    void clearEvents() {
        triggerEvents.clear();
        collisionEvents.clear();
    }
};

class MyBodyActivationListener : public JPH::BodyActivationListener {
public:
    virtual void OnBodyActivated(const JPH::BodyID& inBodyID, Uint64 inBodyUserData) override {
        // fmt::print("A body got activated\n");
    }

    virtual void OnBodyDeactivated(const JPH::BodyID& inBodyID, Uint64 inBodyUserData) override {
        // fmt::print("A body went to sleep\n");
    }
};


Physics3D* Physics3D::_instance = nullptr;

Physics3D::Physics3D() {
    // if (_instance != nullptr)
    //     throw std::runtime_error("Physics subsystem is already initialized!");

    // _instance = this;
}

Physics3D::~Physics3D() {
    deinit();
}

void Physics3D::init(Vapor::TaskScheduler& taskScheduler) {
    JPH::RegisterDefaultAllocator();
    JPH::Trace = TraceImpl;

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    // Create JoltEnkiJobSystem using the provided task scheduler
    jobSystem = std::make_unique<Vapor::JoltEnkiJobSystem>(taskScheduler, 2048);
    // jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
    //   JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1
    // );

    const uint cMaxBodies = 1024;
    const uint cNumBodyMutexes = 0;
    const uint cMaxBodyPairs = 1024;
    const uint cMaxContactConstraints = 1024;
    broad_phase_layer_interface = std::make_unique<BPLayerInterfaceImpl>();
    object_vs_broadphase_layer_filter = std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();
    object_vs_object_layer_filter = std::make_unique<ObjectLayerPairFilterImpl>();
    physicsSystem = std::make_unique<JPH::PhysicsSystem>();
    physicsSystem->Init(
      cMaxBodies,
      cNumBodyMutexes,
      cMaxBodyPairs,
      cMaxContactConstraints,
      *broad_phase_layer_interface.get(),
      *object_vs_broadphase_layer_filter.get(),
      *object_vs_object_layer_filter.get()
    );

    bodyActivationListener = std::make_unique<MyBodyActivationListener>();
    physicsSystem->SetBodyActivationListener(bodyActivationListener.get());

    contactListener = std::make_unique<MyContactListener>();
    physicsSystem->SetContactListener(contactListener.get());

    bodyInterface = &physicsSystem->GetBodyInterface();

    physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    timeAccum = 0.0f;
    step = 0;

    isInitialized = true;
}

void Physics3D::deinit() {
    if (!isInitialized) {
        return;
    }

    for (auto body : bodies) {
        bodyInterface->RemoveBody(body.second);
        bodyInterface->DestroyBody(body.second);
    }
    bodies.clear();

    tempAllocator.reset();
    jobSystem.reset(); // Physics3D owns this
    physicsSystem.reset();
    bodyActivationListener.reset();
    contactListener.reset();
    bodyInterface = nullptr;
    timeAccum = 0.0f;
    step = 0;

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;

    isInitialized = false;
}

void Physics3D::process(const std::shared_ptr<Scene>& scene, float dt) {
    // sync physics world with scene data (Scene → Physics)
    for (auto& node : scene->nodes) {
        if (node->body.valid()) {
            auto bodyID = bodies[node->body.rid];
            auto motionType = bodyInterface->GetMotionType(bodyID);

            // Only sync Kinematic and Static bodies from scene to physics
            // Dynamic bodies are controlled by physics engine
            if (motionType != JPH::EMotionType::Dynamic) {
                // Sync position
                auto pos = node->getWorldPosition();
                bodyInterface->SetPosition(
                    bodyID,
                    JPH::RVec3(pos.x, pos.y, pos.z),
                    JPH::EActivation::DontActivate
                );

                // Sync rotation
                auto rot = node->getWorldRotation();
                bodyInterface->SetRotation(
                    bodyID,
                    JPH::Quat(rot.x, rot.y, rot.z, rot.w),
                    JPH::EActivation::DontActivate
                );
            }
        }
    }

    // Apply fluid forces before physics update
    for (auto& fluidVolume : scene->fluidVolumes) {
        if (fluidVolume) {
            fluidVolume->applyForcesToBodies(dt);
        }
    }

    // update physics world
    timeAccum += dt;
    while (timeAccum >= FIXED_TIME_STEP) {
        ++step;
        physicsSystem->Update(FIXED_TIME_STEP, 1, tempAllocator.get(), jobSystem.get());

        // Update character controllers
        std::function<void(const std::shared_ptr<Node>&)> updateCharacterControllers = [&](const std::shared_ptr<Node>& node) {
            if (node->characterController) {
                node->characterController->update(FIXED_TIME_STEP, getGravity());
            }
            for (const auto& child : node->children) {
                updateCharacterControllers(child);
            }
        };
        for (auto& node : scene->nodes) {
            updateCharacterControllers(node);
        }

        // Update vehicle controllers
        std::function<void(const std::shared_ptr<Node>&)> updateVehicleControllers = [&](const std::shared_ptr<Node>& node) {
            if (node->vehicleController) {
                node->vehicleController->update(FIXED_TIME_STEP);
            }
            for (const auto& child : node->children) {
                updateVehicleControllers(child);
            }
        };
        for (auto& node : scene->nodes) {
            updateVehicleControllers(node);
        }

        timeAccum -= FIXED_TIME_STEP;
    }

    // sync scene data with physics world (Physics → Scene)
    for (auto& node : scene->nodes) {
        if (node->body.valid()) {
            auto bodyID = bodies[node->body.rid];
            auto motionType = bodyInterface->GetMotionType(bodyID);

            // Only sync Dynamic bodies from physics to scene
            if (motionType == JPH::EMotionType::Dynamic) {
                // Sync position
                auto pos = bodyInterface->GetPosition(bodyID);
                node->setPosition(glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ()));

                // Sync rotation
                auto rot = bodyInterface->GetRotation(bodyID);
                node->setLocalRotation(glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ()));

                node->isTransformDirty = true;
            }
        }
    }

    // Sync character controller positions back to nodes
    std::function<void(const std::shared_ptr<Node>&)> syncCharacterControllers = [&](const std::shared_ptr<Node>& node) {
        if (node->characterController) {
            glm::vec3 charPos = node->characterController->getPosition();
            node->setPosition(charPos);
            node->isTransformDirty = true;
        }
        for (const auto& child : node->children) {
            syncCharacterControllers(child);
        }
    };
    for (auto& node : scene->nodes) {
        syncCharacterControllers(node);
    }

    // Sync vehicle controller positions/rotations back to nodes
    std::function<void(const std::shared_ptr<Node>&)> syncVehicleControllers = [&](const std::shared_ptr<Node>& node) {
        if (node->vehicleController) {
            glm::vec3 vehiclePos = node->vehicleController->getPosition();
            glm::quat vehicleRot = node->vehicleController->getRotation();
            node->setPosition(vehiclePos);
            node->setLocalRotation(vehicleRot);
            node->isTransformDirty = true;
        }
        for (const auto& child : node->children) {
            syncVehicleControllers(child);
        }
    };
    for (auto& node : scene->nodes) {
        syncVehicleControllers(node);
    }

    // Process physics events (triggers and collisions)
    auto* listener = static_cast<MyContactListener*>(contactListener.get());

    // Process trigger events
    for (auto& event : listener->triggerEvents) {
        if (event.isEnter) {
            event.triggerNode->onTriggerEnter(event.otherNode);
            event.otherNode->onTriggerEnter(event.triggerNode);  // Bidirectional notification
        } else {
            event.triggerNode->onTriggerExit(event.otherNode);
            event.otherNode->onTriggerExit(event.triggerNode);
        }
    }

    // Process collision events
    for (auto& event : listener->collisionEvents) {
        if (event.isEnter) {
            event.node1->onCollisionEnter(event.node2);
            event.node2->onCollisionEnter(event.node1);
        } else {
            event.node1->onCollisionExit(event.node2);
            event.node2->onCollisionExit(event.node1);
        }
    }

    // Clear events for next frame
    listener->clearEvents();

    // draw debug UI
    if (isDebugUIEnabled) {
        // TODO: physics debug UI
    }

    // debug output
    // static int debugCounter = 0;
    // if (++debugCounter % 60 == 0) { // print every 60 frames
    //     for (auto& node : scene->nodes) {
    //         if (node->body.valid()) {
    //             auto body = bodies[node->body.rid];
    //             auto pos = bodyInterface->GetPosition(body);
    //             auto motionType = bodyInterface->GetMotionType(body);
    //             fmt::print(
    //                 "Node: {}, Pos: ({:.2f}, {:.2f}, {:.2f}), Motion: {}\n",
    //                 node->name, pos.GetX(), pos.GetY(), pos.GetZ(),
    //                 motionType == JPH::EMotionType::Dynamic ? "Dynamic" : "Static"
    //             );
    //         }
    //     }
    // }
}

void Physics3D::drawImGui(float dt) {
}

BodyHandle Physics3D::createSphereBody(
  float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType
) {
    JPH::BodyCreationSettings bodySettings(
      new JPH::SphereShape(radius),
      JPH::RVec3(position.x, position.y, position.z),
      JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
      convertMotionType(motionType),
      motionType == BodyMotionType::Static ? Layers::NON_MOVING : Layers::MOVING
    );
    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create body");
    }
    bodies[nextBodyID] = body->GetID();

    return BodyHandle{ nextBodyID++ };
}

BodyHandle Physics3D::createBoxBody(
  const glm::vec3& halfSize, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType
) {
    JPH::BoxShapeSettings shapeSettings(JPH::Vec3(halfSize.x, halfSize.y, halfSize.z));
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create box shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
      shape,
      JPH::RVec3(position.x, position.y, position.z),
      JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
      convertMotionType(motionType),
      motionType == BodyMotionType::Static ? Layers::NON_MOVING : Layers::MOVING
    );
    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create body");
    }
    bodies[nextBodyID] = body->GetID();

    return BodyHandle{ nextBodyID++ };
}

void Physics3D::addBody(BodyHandle handle, bool activate) {
    auto id = bodies.at(handle.rid);
    bodyInterface->AddBody(id, activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
}

void Physics3D::removeBody(BodyHandle handle) {
    auto id = bodies.at(handle.rid);
    bodyInterface->RemoveBody(id);
}

void Physics3D::destroyBody(BodyHandle handle) {
    auto id = bodies.at(handle.rid);
    bodyInterface->DestroyBody(id);
}

bool Physics3D::raycast(const glm::vec3& from, const glm::vec3& to, RaycastHit& hit) {
    JPH::RRayCast ray(JPH::RVec3(from.x, from.y, from.z), JPH::RVec3(to.x - from.x, to.y - from.y, to.z - from.z));
    JPH::RayCastResult result;
    bool hasHit = physicsSystem->GetNarrowPhaseQuery().CastRay(ray, result);
    if (hasHit) {
        JPH::BodyID hitBodyID = result.mBodyID;
        JPH::RVec3 hitPoint = ray.GetPointOnRay(result.mFraction);

        hit.point = glm::vec3(hitPoint.GetX(), hitPoint.GetY(), hitPoint.GetZ());
        hit.hitDistance = result.mFraction * glm::distance(from, to);
        hit.hitFraction = result.mFraction;

        // Get Node from UserData
        Uint64 userData = bodyInterface->GetUserData(hitBodyID);
        hit.node = reinterpret_cast<Node*>(userData);

        // Get surface normal at hit point
        JPH::BodyLockRead lock(physicsSystem->GetBodyLockInterface(), hitBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            JPH::Vec3 surfaceNormal = body.GetWorldSpaceSurfaceNormal(
                result.mSubShapeID2,
                hitPoint
            );
            hit.normal = glm::vec3(surfaceNormal.GetX(), surfaceNormal.GetY(), surfaceNormal.GetZ());
        } else {
            hit.normal = glm::vec3(0.0f, 1.0f, 0.0f);  // Default up vector
        }

        return true;
    } else {
        return false;
    }
}

void Physics3D::setGravity(const glm::vec3& acc) {
    currentGravity = acc;
    physicsSystem->SetGravity(JPH::Vec3(acc.x, acc.y, acc.z));
}

glm::vec3 Physics3D::getGravity() const {
    return currentGravity;
}

// ====== 力與力矩 ======
void Physics3D::applyForce(BodyHandle handle, const glm::vec3& force, const glm::vec3& relativePos) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    JPH::Vec3 joltForce(force.x, force.y, force.z);

    if (glm::length(relativePos) > 0.0001f) {
        JPH::Vec3 joltRelPos(relativePos.x, relativePos.y, relativePos.z);
        bodyInterface->AddForce(bodyID, joltForce, joltRelPos);
    } else {
        bodyInterface->AddForce(bodyID, joltForce);
    }
}

void Physics3D::applyCentralForce(BodyHandle handle, const glm::vec3& force) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    JPH::Vec3 joltForce(force.x, force.y, force.z);
    bodyInterface->AddForce(bodyID, joltForce);
}

void Physics3D::applyTorque(BodyHandle handle, const glm::vec3& torque) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    JPH::Vec3 joltTorque(torque.x, torque.y, torque.z);
    bodyInterface->AddTorque(bodyID, joltTorque);
}

void Physics3D::applyImpulse(BodyHandle handle, const glm::vec3& impulse, const glm::vec3& relativePos) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    JPH::Vec3 joltImpulse(impulse.x, impulse.y, impulse.z);

    if (glm::length(relativePos) > 0.0001f) {
        JPH::Vec3 joltRelPos(relativePos.x, relativePos.y, relativePos.z);
        bodyInterface->AddImpulse(bodyID, joltImpulse, joltRelPos);
    } else {
        bodyInterface->AddImpulse(bodyID, joltImpulse);
    }
}

void Physics3D::applyCentralImpulse(BodyHandle handle, const glm::vec3& impulse) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    JPH::Vec3 joltImpulse(impulse.x, impulse.y, impulse.z);
    bodyInterface->AddImpulse(bodyID, joltImpulse);
}

void Physics3D::applyAngularImpulse(BodyHandle handle, const glm::vec3& angularImpulse) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    JPH::Vec3 joltAngularImpulse(angularImpulse.x, angularImpulse.y, angularImpulse.z);
    bodyInterface->AddAngularImpulse(bodyID, joltAngularImpulse);
}

// ====== 速度控制 ======
void Physics3D::setLinearVelocity(BodyHandle handle, const glm::vec3& vel) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetLinearVelocity(bodyID, JPH::Vec3(vel.x, vel.y, vel.z));
}

glm::vec3 Physics3D::getLinearVelocity(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return glm::vec3(0.0f);

    JPH::BodyID bodyID = bodies.at(handle.rid);
    JPH::Vec3 vel = bodyInterface->GetLinearVelocity(bodyID);
    return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
}

void Physics3D::setAngularVelocity(BodyHandle handle, const glm::vec3& vel) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetAngularVelocity(bodyID, JPH::Vec3(vel.x, vel.y, vel.z));
}

glm::vec3 Physics3D::getAngularVelocity(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return glm::vec3(0.0f);

    JPH::BodyID bodyID = bodies.at(handle.rid);
    JPH::Vec3 vel = bodyInterface->GetAngularVelocity(bodyID);
    return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
}

// ====== 物理屬性 ======
void Physics3D::setMass(BodyHandle handle, float mass) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->GetMotionProperties(bodyID)->SetInverseMass(1.0f / mass);
}

float Physics3D::getMass(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return 0.0f;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    auto* motionProps = bodyInterface->GetMotionProperties(bodyID);
    if (!motionProps) return 0.0f;
    return 1.0f / motionProps->GetInverseMass();
}

void Physics3D::setFriction(BodyHandle handle, float friction) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetFriction(bodyID, friction);
}

float Physics3D::getFriction(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return 0.0f;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    return bodyInterface->GetFriction(bodyID);
}

void Physics3D::setRestitution(BodyHandle handle, float restitution) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetRestitution(bodyID, restitution);
}

float Physics3D::getRestitution(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return 0.0f;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    return bodyInterface->GetRestitution(bodyID);
}

void Physics3D::setLinearDamping(BodyHandle handle, float damping) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    auto* motionProps = bodyInterface->GetMotionProperties(bodyID);
    if (motionProps) {
        motionProps->SetLinearDamping(damping);
    }
}

float Physics3D::getLinearDamping(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return 0.0f;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    auto* motionProps = bodyInterface->GetMotionProperties(bodyID);
    return motionProps ? motionProps->GetLinearDamping() : 0.0f;
}

void Physics3D::setAngularDamping(BodyHandle handle, float damping) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    auto* motionProps = bodyInterface->GetMotionProperties(bodyID);
    if (motionProps) {
        motionProps->SetAngularDamping(damping);
    }
}

float Physics3D::getAngularDamping(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return 0.0f;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    auto* motionProps = bodyInterface->GetMotionProperties(bodyID);
    return motionProps ? motionProps->GetAngularDamping() : 0.0f;
}

// ====== 運動狀態 ======
void Physics3D::setMotionType(BodyHandle handle, BodyMotionType type) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetMotionType(bodyID, convertMotionType(type), JPH::EActivation::Activate);
}

BodyMotionType Physics3D::getMotionType(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return BodyMotionType::Static;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    JPH::EMotionType motionType = bodyInterface->GetMotionType(bodyID);

    switch (motionType) {
        case JPH::EMotionType::Static:
            return BodyMotionType::Static;
        case JPH::EMotionType::Dynamic:
            return BodyMotionType::Dynamic;
        case JPH::EMotionType::Kinematic:
            return BodyMotionType::Kinematic;
        default:
            return BodyMotionType::Static;
    }
}

void Physics3D::setGravityFactor(BodyHandle handle, float factor) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    auto* motionProps = bodyInterface->GetMotionProperties(bodyID);
    if (motionProps) {
        motionProps->SetGravityFactor(factor);
    }
}

float Physics3D::getGravityFactor(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return 1.0f;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    auto* motionProps = bodyInterface->GetMotionProperties(bodyID);
    return motionProps ? motionProps->GetGravityFactor() : 1.0f;
}

// ====== 啟用/停用 ======
void Physics3D::activateBody(BodyHandle handle) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->ActivateBody(bodyID);
}

void Physics3D::deactivateBody(BodyHandle handle) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->DeactivateBody(bodyID);
}

bool Physics3D::isActive(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return false;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    return bodyInterface->IsActive(bodyID);
}

// ====== 位置與旋轉 ======
glm::vec3 Physics3D::getPosition(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return glm::vec3(0.0f);

    JPH::BodyID bodyID = bodies.at(handle.rid);
    JPH::RVec3 pos = bodyInterface->GetPosition(bodyID);
    return glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
}

void Physics3D::setPosition(BodyHandle handle, const glm::vec3& position) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetPosition(bodyID, JPH::RVec3(position.x, position.y, position.z), JPH::EActivation::Activate);
}

glm::quat Physics3D::getRotation(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return glm::quat(1, 0, 0, 0);

    JPH::BodyID bodyID = bodies.at(handle.rid);
    JPH::Quat rot = bodyInterface->GetRotation(bodyID);
    return glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
}

void Physics3D::setRotation(BodyHandle handle, const glm::quat& rotation) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetRotation(bodyID, JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w), JPH::EActivation::Activate);
}

// ====== UserData 管理 ======
void Physics3D::setBodyUserData(BodyHandle handle, Uint64 userData) {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return;

    JPH::BodyID bodyID = bodies[handle.rid];
    bodyInterface->SetUserData(bodyID, userData);
}

Uint64 Physics3D::getBodyUserData(BodyHandle handle) const {
    if (!handle.valid() || bodies.find(handle.rid) == bodies.end()) return 0;

    JPH::BodyID bodyID = bodies.at(handle.rid);
    return bodyInterface->GetUserData(bodyID);
}

// ====== 新形狀創建方法 ======
BodyHandle Physics3D::createCapsuleBody(
    float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType
) {
    JPH::CapsuleShapeSettings shapeSettings(halfHeight, radius);
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create capsule shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        convertMotionType(motionType),
        motionType == BodyMotionType::Static ? Layers::NON_MOVING : Layers::MOVING
    );

    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create capsule body");
    }
    bodies[nextBodyID] = body->GetID();

    return BodyHandle{ nextBodyID++ };
}

BodyHandle Physics3D::createCylinderBody(
    float halfHeight, float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType
) {
    JPH::CylinderShapeSettings shapeSettings(halfHeight, radius);
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create cylinder shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        convertMotionType(motionType),
        motionType == BodyMotionType::Static ? Layers::NON_MOVING : Layers::MOVING
    );

    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create cylinder body");
    }
    bodies[nextBodyID] = body->GetID();

    return BodyHandle{ nextBodyID++ };
}

BodyHandle Physics3D::createMeshBody(
    const std::vector<glm::vec3>& vertices,
    const std::vector<Uint32>& indices,
    const glm::vec3& position,
    const glm::quat& rotation,
    BodyMotionType motionType
) {
    // Convert vertices to Jolt format
    JPH::VertexList joltVertices;
    joltVertices.reserve(vertices.size());
    for (const auto& v : vertices) {
        joltVertices.push_back(JPH::Float3(v.x, v.y, v.z));
    }

    // Convert indices to Jolt format (triangles)
    JPH::IndexedTriangleList joltTriangles;
    joltTriangles.reserve(indices.size() / 3);
    for (size_t i = 0; i < indices.size(); i += 3) {
        JPH::IndexedTriangle triangle;
        triangle.mIdx[0] = indices[i];
        triangle.mIdx[1] = indices[i + 1];
        triangle.mIdx[2] = indices[i + 2];
        joltTriangles.push_back(triangle);
    }

    // Create mesh shape
    JPH::MeshShapeSettings shapeSettings(joltVertices, joltTriangles);
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create mesh shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    // Mesh bodies are usually static
    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        convertMotionType(motionType),
        Layers::NON_MOVING  // Mesh shapes should be static
    );

    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create mesh body");
    }
    bodies[nextBodyID] = body->GetID();

    return BodyHandle{ nextBodyID++ };
}

BodyHandle Physics3D::createConvexHullBody(
    const std::vector<glm::vec3>& points,
    const glm::vec3& position,
    const glm::quat& rotation,
    BodyMotionType motionType
) {
    // Convert points to Jolt format
    JPH::Array<JPH::Vec3> joltPoints;
    joltPoints.reserve(points.size());
    for (const auto& p : points) {
        joltPoints.push_back(JPH::Vec3(p.x, p.y, p.z));
    }

    // Create convex hull shape
    JPH::ConvexHullShapeSettings shapeSettings(joltPoints);
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create convex hull shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        convertMotionType(motionType),
        motionType == BodyMotionType::Static ? Layers::NON_MOVING : Layers::MOVING
    );

    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create convex hull body");
    }
    bodies[nextBodyID] = body->GetID();

    return BodyHandle{ nextBodyID++ };
}

// ====== Trigger 創建方法 ======
TriggerHandle Physics3D::createBoxTrigger(
    const glm::vec3& halfSize,
    const glm::vec3& position,
    const glm::quat& rotation
) {
    JPH::BoxShapeSettings shapeSettings(JPH::Vec3(halfSize.x, halfSize.y, halfSize.z));
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create box trigger shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        JPH::EMotionType::Static,  // Triggers are usually static
        Layers::TRIGGER
    );

    bodySettings.mIsSensor = true;  // Critical: Set as sensor (no physical collision)

    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create box trigger body");
    }

    bodyInterface->AddBody(body->GetID(), JPH::EActivation::Activate);
    triggers[nextTriggerID] = body->GetID();

    return TriggerHandle{ nextTriggerID++ };
}

TriggerHandle Physics3D::createSphereTrigger(
    float radius,
    const glm::vec3& position,
    const glm::quat& rotation
) {
    JPH::SphereShapeSettings shapeSettings(radius);
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create sphere trigger shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        JPH::EMotionType::Static,
        Layers::TRIGGER
    );

    bodySettings.mIsSensor = true;

    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create sphere trigger body");
    }

    bodyInterface->AddBody(body->GetID(), JPH::EActivation::Activate);
    triggers[nextTriggerID] = body->GetID();

    return TriggerHandle{ nextTriggerID++ };
}

TriggerHandle Physics3D::createCapsuleTrigger(
    float halfHeight,
    float radius,
    const glm::vec3& position,
    const glm::quat& rotation
) {
    JPH::CapsuleShapeSettings shapeSettings(halfHeight, radius);
    shapeSettings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        throw std::runtime_error("Failed to create capsule trigger shape");
    }
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
        shape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        JPH::EMotionType::Static,
        Layers::TRIGGER
    );

    bodySettings.mIsSensor = true;

    JPH::Body* body = bodyInterface->CreateBody(bodySettings);
    if (!body) {
        throw std::runtime_error("Failed to create capsule trigger body");
    }

    bodyInterface->AddBody(body->GetID(), JPH::EActivation::Activate);
    triggers[nextTriggerID] = body->GetID();

    return TriggerHandle{ nextTriggerID++ };
}

void Physics3D::removeTrigger(TriggerHandle handle) {
    if (!handle.valid() || triggers.find(handle.rid) == triggers.end()) return;

    JPH::BodyID id = triggers.at(handle.rid);
    bodyInterface->RemoveBody(id);
}

void Physics3D::destroyTrigger(TriggerHandle handle) {
    if (!handle.valid() || triggers.find(handle.rid) == triggers.end()) return;

    JPH::BodyID id = triggers.at(handle.rid);
    bodyInterface->RemoveBody(id);
    bodyInterface->DestroyBody(id);
    triggers.erase(handle.rid);
}

void Physics3D::setTriggerUserData(TriggerHandle handle, Uint64 userData) {
    if (!handle.valid() || triggers.find(handle.rid) == triggers.end()) return;

    JPH::BodyID bodyID = triggers[handle.rid];
    bodyInterface->SetUserData(bodyID, userData);
}

Uint64 Physics3D::getTriggerUserData(TriggerHandle handle) const {
    if (!handle.valid() || triggers.find(handle.rid) == triggers.end()) return 0;

    JPH::BodyID bodyID = triggers.at(handle.rid);
    return bodyInterface->GetUserData(bodyID);
}
// ====== 重疊測試方法 ======
OverlapResult Physics3D::overlapSphere(const glm::vec3& center, float radius) {
    OverlapResult result;

    // Create query shape
    JPH::SphereShape queryShape(radius);
    JPH::RVec3 queryPos(center.x, center.y, center.z);
    JPH::Quat queryRot = JPH::Quat::sIdentity();

    // Use CollideShape query
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

    physicsSystem->GetNarrowPhaseQuery().CollideShape(
        &queryShape,
        JPH::Vec3::sReplicate(1.0f),  // Scale
        queryPos,
        queryRot,
        settings,
        JPH::RVec3::sZero(),
        collector
    );

    // Collect results
    for (const auto& hit : collector.mHits) {
        JPH::BodyID hitBodyID = hit.mBodyID2;

        // Find the BodyHandle
        for (const auto& [handleID, bodyID] : bodies) {
            if (bodyID == hitBodyID) {
                BodyHandle handle{handleID};
                result.bodies.push_back(handle);

                // Get Node from UserData
                Uint64 userData = bodyInterface->GetUserData(hitBodyID);
                if (userData != 0) {
                    Node* node = reinterpret_cast<Node*>(userData);
                    result.nodes.push_back(node);
                }
                break;
            }
        }
    }

    return result;
}

OverlapResult Physics3D::overlapBox(
    const glm::vec3& center,
    const glm::vec3& halfExtents,
    const glm::quat& rotation
) {
    OverlapResult result;

    // Create query shape
    JPH::BoxShape queryShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z));
    JPH::RVec3 queryPos(center.x, center.y, center.z);
    JPH::Quat queryRot(rotation.x, rotation.y, rotation.z, rotation.w);

    // Use CollideShape query
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

    physicsSystem->GetNarrowPhaseQuery().CollideShape(
        &queryShape,
        JPH::Vec3::sReplicate(1.0f),
        queryPos,
        queryRot,
        settings,
        JPH::RVec3::sZero(),
        collector
    );

    // Collect results
    for (const auto& hit : collector.mHits) {
        JPH::BodyID hitBodyID = hit.mBodyID2;

        for (const auto& [handleID, bodyID] : bodies) {
            if (bodyID == hitBodyID) {
                BodyHandle handle{handleID};
                result.bodies.push_back(handle);

                Uint64 userData = bodyInterface->GetUserData(hitBodyID);
                if (userData != 0) {
                    Node* node = reinterpret_cast<Node*>(userData);
                    result.nodes.push_back(node);
                }
                break;
            }
        }
    }

    return result;
}

OverlapResult Physics3D::overlapCapsule(
    const glm::vec3& point1,
    const glm::vec3& point2,
    float radius
) {
    OverlapResult result;

    // Calculate capsule parameters
    glm::vec3 axis = point2 - point1;
    float length = glm::length(axis);
    float halfHeight = length * 0.5f;

    if (halfHeight < 0.001f) {
        // Degenerate case: use sphere
        return overlapSphere(point1, radius);
    }

    // Create query shape
    JPH::CapsuleShape queryShape(halfHeight, radius);
    JPH::RVec3 center = JPH::RVec3(
        (point1.x + point2.x) * 0.5f,
        (point1.y + point2.y) * 0.5f,
        (point1.z + point2.z) * 0.5f
    );

    // Calculate rotation to align capsule with axis
    glm::vec3 up = glm::normalize(axis);
    glm::vec3 right = glm::abs(up.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 forward = glm::normalize(glm::cross(right, up));
    right = glm::cross(up, forward);

    glm::mat3 rotMat(right, up, forward);
    glm::quat rot = glm::quat_cast(rotMat);
    JPH::Quat queryRot(rot.x, rot.y, rot.z, rot.w);

    // Use CollideShape query
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

    physicsSystem->GetNarrowPhaseQuery().CollideShape(
        &queryShape,
        JPH::Vec3::sReplicate(1.0f),
        center,
        queryRot,
        settings,
        JPH::RVec3::sZero(),
        collector
    );

    // Collect results
    for (const auto& hit : collector.mHits) {
        JPH::BodyID hitBodyID = hit.mBodyID2;

        for (const auto& [handleID, bodyID] : bodies) {
            if (bodyID == hitBodyID) {
                BodyHandle handle{handleID};
                result.bodies.push_back(handle);

                Uint64 userData = bodyInterface->GetUserData(hitBodyID);
                if (userData != 0) {
                    Node* node = reinterpret_cast<Node*>(userData);
                    result.nodes.push_back(node);
                }
                break;
            }
        }
    }

    return result;
}
