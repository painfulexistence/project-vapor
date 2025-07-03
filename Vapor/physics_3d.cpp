#include "physics_3d.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <fmt/core.h>
#include <SDL3/SDL_stdinc.h>
#include <thread>

// #include "physics_debug_drawer.hpp"
#include "scene.hpp"

JPH_SUPPRESS_WARNINGS

using namespace JPH::literals; // for real value _r suffix

static void TraceImpl(const char *inFMT, ...) {
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
	static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
	virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
		switch (inObject1)
		{
		case Layers::NON_MOVING:
			return inObject2 == Layers::MOVING;
		case Layers::MOVING:
			return true;
		default:
			JPH_ASSERT(false);
			return false;
		}
	}
};

namespace BroadPhaseLayers {
	static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
	static constexpr JPH::BroadPhaseLayer MOVING(1);
	static constexpr uint NUM_LAYERS(2);
};

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
	BPLayerInterfaceImpl() {
		mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
		mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
	}

	virtual uint GetNumBroadPhaseLayers() const override {
		return BroadPhaseLayers::NUM_LAYERS;
	}

	virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
		JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
		return mObjectToBroadPhase[inLayer];
	}

    virtual const char * GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
		switch ((JPH::BroadPhaseLayer::Type)inLayer) {
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
            return "NON_MOVING";
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
            return "MOVING";
		default:
            JPH_ASSERT(false);
            return "INVALID";
		}
	}

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
		default:
			JPH_ASSERT(false);
			return false;
		}
	}
};

class MyContactListener : public JPH::ContactListener {
public:
	virtual JPH::ValidateResult	OnContactValidate(const JPH::Body &inBody1, const JPH::Body &inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult &inCollisionResult) override {
		// fmt::print("Contact validate callback\n");
		return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
	}

	virtual void OnContactAdded(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override {
		// fmt::print("A contact was added\n");
        auto obj1 = reinterpret_cast<Node*>(inBody1.GetUserData());
        auto obj2 = reinterpret_cast<Node*>(inBody2.GetUserData());
        if (obj1 && obj2) {
            // obj1->OnCollision(obj2);
            // obj2->OnCollision(obj1);
        }
	}

	virtual void OnContactPersisted(const JPH::Body &inBody1, const JPH::Body &inBody2, const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override {
		// fmt::print("A contact was persisted\n");
	}

	virtual void OnContactRemoved(const JPH::SubShapeIDPair &inSubShapePair) override {
		// fmt::print("A contact was removed\n");
	}
};

class MyBodyActivationListener : public JPH::BodyActivationListener {
public:
	virtual void OnBodyActivated(const JPH::BodyID &inBodyID, Uint64 inBodyUserData) override {
		// fmt::print("A body got activated\n");
	}

	virtual void OnBodyDeactivated(const JPH::BodyID &inBodyID, Uint64 inBodyUserData) override {
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

void Physics3D::init() {
    JPH::RegisterDefaultAllocator();
    JPH::Trace = TraceImpl;

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

    const uint cMaxBodies = 1024;
    const uint cNumBodyMutexes = 0;
    const uint cMaxBodyPairs = 1024;
    const uint cMaxContactConstraints = 1024;
    broad_phase_layer_interface = std::make_unique<BPLayerInterfaceImpl>();
    object_vs_broadphase_layer_filter = std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();
    object_vs_object_layer_filter = std::make_unique<ObjectLayerPairFilterImpl>();
    physicsSystem = std::make_unique<JPH::PhysicsSystem>();
    physicsSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, *broad_phase_layer_interface.get(), *object_vs_broadphase_layer_filter.get(), *object_vs_object_layer_filter.get());

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
    jobSystem.reset();
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
    // sync physics world with scene data
    for (auto& node : scene->nodes) {
        if (node->body.valid()) {
            auto pos = node->getWorldPosition();
            auto rot = node->getWorldRotation();
            bodyInterface->SetPosition(bodies[node->body.rid], JPH::RVec3(pos.x, pos.y, pos.z), JPH::EActivation::DontActivate);
            // TODO: sync rotation
            // bodyInterface->SetRotation(bodies[node->body.rid], JPH::Quat(rot.x, rot.y, rot.z, rot.w), JPH::EActivation::DontActivate);
        }
    }

    // update physics world
    timeAccum += dt;
    while (timeAccum >= FIXED_TIME_STEP) {
        ++step;
		physicsSystem->Update(FIXED_TIME_STEP, 1, tempAllocator.get(), jobSystem.get());
        timeAccum -= FIXED_TIME_STEP;
    }

    // sync scene data with physics world
    for (auto& node : scene->nodes) {
        if (node->body.valid()) {
            auto body = bodies[node->body.rid];
            if (bodyInterface->GetMotionType(body) == JPH::EMotionType::Dynamic) {
                auto pos = bodyInterface->GetPosition(body);
                auto rot = bodyInterface->GetRotation(body);
                node->setPosition(glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ()));
                // TODO: sync rotation
                // node->setRotation(glm::quat(rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW()));
                node->isTransformDirty = true;
            }
        }
    }

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

BodyHandle Physics3D::createSphereBody(float radius, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType) {
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

    return BodyHandle { nextBodyID++ };
}

BodyHandle Physics3D::createBoxBody(const glm::vec3& halfSize, const glm::vec3& position, const glm::quat& rotation, BodyMotionType motionType) {
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

    return BodyHandle { nextBodyID++ };
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
        auto hitBody = result.mBodyID;
        auto hitPoint = ray.GetPointOnRay(result.mFraction);
        hit.point = glm::vec3(hitPoint.GetX(), hitPoint.GetY(), hitPoint.GetZ());
        hit.node = nullptr; // TODO: get node from hit body
        hit.normal = glm::vec3(0.0f, 0.0f, 0.0f); // TODO: get normal from hit body
        hit.hitDistance = result.mFraction * glm::distance(from, to);
        hit.hitFraction = result.mFraction;
        return true;
    } else {
        return false;
    }
}

void Physics3D::setGravity(const glm::vec3& acc) {
    physicsSystem->SetGravity(JPH::Vec3(acc.x, acc.y, acc.z));
}