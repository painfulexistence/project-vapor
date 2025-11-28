#include "vehicle_controller.hpp"
#include "physics_3d.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>

// Template: Sedan configuration
VehicleSettings VehicleSettings::createSedan() {
    VehicleSettings settings;
    settings.mass = 1500.0f;
    settings.dimensions = glm::vec3(0.9f, 0.7f, 2.2f);  // Typical sedan size
    settings.maxSteeringAngle = 0.7f;
    settings.maxEngineTorque = 500.0f;
    settings.maxBrakeTorque = 1500.0f;

    // 4 wheels: front-left, front-right, rear-left, rear-right
    settings.wheels = {
        // Front-left wheel (steering)
        WheelSettings{
            .position = glm::vec3(-0.8f, -0.5f, 1.3f),
            .wheelRadius = 0.3f,
            .wheelWidth = 0.2f,
            .enableTraction = true
        },
        // Front-right wheel (steering)
        WheelSettings{
            .position = glm::vec3(0.8f, -0.5f, 1.3f),
            .wheelRadius = 0.3f,
            .wheelWidth = 0.2f,
            .enableTraction = true
        },
        // Rear-left wheel (drive)
        WheelSettings{
            .position = glm::vec3(-0.8f, -0.5f, -1.3f),
            .wheelRadius = 0.3f,
            .wheelWidth = 0.2f,
            .enableTraction = true
        },
        // Rear-right wheel (drive)
        WheelSettings{
            .position = glm::vec3(0.8f, -0.5f, -1.3f),
            .wheelRadius = 0.3f,
            .wheelWidth = 0.2f,
            .enableTraction = true
        }
    };

    return settings;
}

// Template: Truck configuration
VehicleSettings VehicleSettings::createTruck() {
    VehicleSettings settings;
    settings.mass = 3500.0f;
    settings.dimensions = glm::vec3(1.2f, 1.2f, 3.0f);  // Larger truck size
    settings.maxSteeringAngle = 0.5f;  // Less steering angle
    settings.maxEngineTorque = 1000.0f;
    settings.maxBrakeTorque = 3000.0f;

    // 4 wheels with larger radius
    settings.wheels = {
        // Front-left wheel
        WheelSettings{
            .position = glm::vec3(-1.0f, -0.8f, 1.8f),
            .wheelRadius = 0.4f,
            .wheelWidth = 0.3f,
            .enableTraction = true
        },
        // Front-right wheel
        WheelSettings{
            .position = glm::vec3(1.0f, -0.8f, 1.8f),
            .wheelRadius = 0.4f,
            .wheelWidth = 0.3f,
            .enableTraction = true
        },
        // Rear-left wheel
        WheelSettings{
            .position = glm::vec3(-1.0f, -0.8f, -1.8f),
            .wheelRadius = 0.4f,
            .wheelWidth = 0.3f,
            .enableTraction = true
        },
        // Rear-right wheel
        WheelSettings{
            .position = glm::vec3(1.0f, -0.8f, -1.8f),
            .wheelRadius = 0.4f,
            .wheelWidth = 0.3f,
            .enableTraction = true
        }
    };

    return settings;
}

VehicleController::VehicleController(Physics3D* physics, const VehicleSettings& settings, const glm::vec3& position, const glm::quat& rotation)
    : physics(physics)
    , settings(settings)
{
    auto* physicsSystem = physics->getPhysicsSystem();
    auto* bodyInterface = physics->getBodyInterface();

    // Create vehicle body (box shape)
    JPH::BoxShapeSettings bodyShapeSettings(JPH::Vec3(settings.dimensions.x, settings.dimensions.y, settings.dimensions.z));
    JPH::ShapeSettings::ShapeResult bodyShapeResult = bodyShapeSettings.Create();
    JPH::ShapeRefC bodyShape = bodyShapeResult.Get();

    JPH::BodyCreationSettings bodySettings(
        bodyShape,
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        JPH::EMotionType::Dynamic,
        1  // MOVING layer
    );
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = settings.mass;

    vehicleBody = bodyInterface->CreateBody(bodySettings);
    bodyInterface->AddBody(vehicleBody->GetID(), JPH::EActivation::Activate);

    // Create vehicle constraint
    JPH::VehicleConstraintSettings vehicleSettings;

    // Configure wheels using WheelSettingsWV (Wheeled Vehicle)
    vehicleSettings.mWheels.resize(settings.wheels.size());
    for (size_t i = 0; i < settings.wheels.size(); ++i) {
        const auto& wheelSettings = settings.wheels[i];
        JPH::WheelSettingsWV* joltWheel = new JPH::WheelSettingsWV();

        // Base wheel properties
        joltWheel->mPosition = JPH::Vec3(wheelSettings.position.x, wheelSettings.position.y, wheelSettings.position.z);
        joltWheel->mSuspensionDirection = JPH::Vec3(wheelSettings.suspensionDirection.x, wheelSettings.suspensionDirection.y, wheelSettings.suspensionDirection.z);
        joltWheel->mSteeringAxis = JPH::Vec3(wheelSettings.wheelUp.x, wheelSettings.wheelUp.y, wheelSettings.wheelUp.z);
        joltWheel->mWheelForward = JPH::Vec3(wheelSettings.wheelForward.x, wheelSettings.wheelForward.y, wheelSettings.wheelForward.z);
        joltWheel->mWheelUp = JPH::Vec3(wheelSettings.wheelUp.x, wheelSettings.wheelUp.y, wheelSettings.wheelUp.z);

        joltWheel->mSuspensionMinLength = wheelSettings.suspensionMinLength;
        joltWheel->mSuspensionMaxLength = wheelSettings.suspensionMaxLength;
        joltWheel->mSuspensionPreloadLength = wheelSettings.suspensionPreloadLength;
        // Note: mSuspensionFrequency and mSuspensionDamping may not exist in this version of Jolt
        // These properties might be set elsewhere or may have different names
        // joltWheel->mSuspensionFrequency = wheelSettings.suspensionFrequency;
        // joltWheel->mSuspensionDamping = wheelSettings.suspensionDamping;

        joltWheel->mRadius = wheelSettings.wheelRadius;
        joltWheel->mWidth = wheelSettings.wheelWidth;

        // WheelSettingsWV specific properties
        joltWheel->mInertia = 0.9f;  // Wheel rotational inertia
        joltWheel->mAngularDamping = 0.2f;  // Wheel angular damping
        joltWheel->mMaxSteerAngle = (i < 2) ? settings.maxSteeringAngle : 0.0f;  // Front wheels can steer
        joltWheel->mMaxHandBrakeTorque = (i >= 2) ? settings.maxBrakeTorque * 0.5f : 0.0f;  // Rear wheels only

        vehicleSettings.mWheels[i] = joltWheel;
    }

    // Create wheeled vehicle controller
    JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings;
    controller->mEngine.mMaxTorque = settings.maxEngineTorque;
    controller->mTransmission.mMode = JPH::ETransmissionMode::Auto;

    // Configure differential (rear-wheel drive for simplicity)
    controller->mDifferentials.resize(1);
    controller->mDifferentials[0].mLeftWheel = 2;   // Rear-left
    controller->mDifferentials[0].mRightWheel = 3;  // Rear-right

    vehicleSettings.mController = controller;

    // Create the constraint
    vehicleConstraint = std::make_unique<JPH::VehicleConstraint>(*vehicleBody, vehicleSettings);
    physicsSystem->AddConstraint(vehicleConstraint.get());
    physicsSystem->AddStepListener(vehicleConstraint.get());
}

VehicleController::~VehicleController() {
    if (vehicleConstraint) {
        auto* physicsSystem = physics->getPhysicsSystem();
        physicsSystem->RemoveStepListener(vehicleConstraint.get());
        physicsSystem->RemoveConstraint(vehicleConstraint.get());
    }

    if (vehicleBody) {
        auto* bodyInterface = physics->getBodyInterface();
        bodyInterface->RemoveBody(vehicleBody->GetID());
        bodyInterface->DestroyBody(vehicleBody->GetID());
    }
}

void VehicleController::setThrottle(float throttle) {
    currentThrottle = glm::clamp(throttle, -1.0f, 1.0f);
}

void VehicleController::setSteering(float steering) {
    currentSteering = glm::clamp(steering, -1.0f, 1.0f);
}

void VehicleController::setBrake(float brake) {
    currentBrake = glm::clamp(brake, 0.0f, 1.0f);
}

void VehicleController::setHandbrake(bool enabled) {
    handbrakeEnabled = enabled;
}

glm::vec3 VehicleController::getPosition() const {
    JPH::RVec3 pos = vehicleBody->GetPosition();
    return glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
}

glm::quat VehicleController::getRotation() const {
    JPH::Quat rot = vehicleBody->GetRotation();
    return glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
}

glm::vec3 VehicleController::getLinearVelocity() const {
    JPH::Vec3 vel = vehicleBody->GetLinearVelocity();
    return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
}

glm::vec3 VehicleController::getAngularVelocity() const {
    JPH::Vec3 vel = vehicleBody->GetAngularVelocity();
    return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
}

float VehicleController::getSpeed() const {
    return vehicleBody->GetLinearVelocity().Length();
}

float VehicleController::getSpeedKmh() const {
    return getSpeed() * 3.6f;  // m/s to km/h
}

int VehicleController::getWheelCount() const {
    return static_cast<int>(settings.wheels.size());
}

bool VehicleController::isWheelInContact(int wheelIndex) const {
    if (wheelIndex < 0 || wheelIndex >= getWheelCount()) return false;
    const JPH::Wheel* wheel = vehicleConstraint->GetWheel(wheelIndex);
    return wheel->HasContact();
}

glm::vec3 VehicleController::getWheelPosition(int wheelIndex) const {
    if (wheelIndex < 0 || wheelIndex >= getWheelCount()) return glm::vec3(0.0f);
    const JPH::Wheel* wheel = vehicleConstraint->GetWheel(wheelIndex);
    JPH::RVec3 pos = wheel->GetContactPosition();
    return glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
}

glm::vec3 VehicleController::getWheelContactNormal(int wheelIndex) const {
    if (wheelIndex < 0 || wheelIndex >= getWheelCount()) return glm::vec3(0, 1, 0);
    const JPH::Wheel* wheel = vehicleConstraint->GetWheel(wheelIndex);
    JPH::Vec3 normal = wheel->GetContactNormal();
    return glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
}

float VehicleController::getWheelSuspensionLength(int wheelIndex) const {
    if (wheelIndex < 0 || wheelIndex >= getWheelCount()) return 0.0f;
    const JPH::Wheel* wheel = vehicleConstraint->GetWheel(wheelIndex);
    return wheel->GetSuspensionLength();
}

void VehicleController::update(float deltaTime) {
    auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicleConstraint->GetController());

    // Apply throttle (forward/reverse) and steering
    // Note: SetDriverInput already handles steering, so we don't need SetWheelSteering
    controller->SetDriverInput(currentThrottle, currentSteering, currentBrake, handbrakeEnabled ? 1.0f : 0.0f);
}

JPH::BodyID VehicleController::getBodyID() const {
    return vehicleBody->GetID();
}
