#include "character_controller.hpp"
#include "physics_3d.hpp"
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

CharacterController::CharacterController(Physics3D* physics, const CharacterControllerSettings& settings)
  : physics(physics), settings(settings), currentGravity(0, -9.81f, 0) {
    auto* physicsSystem = physics->getPhysicsSystem();

    // Create standing shape (capsule)
    // settings.height is the total capsule height
    // Capsule total height = cylinderHeight + 2*radius
    // So cylinderHeight = totalHeight - 2*radius
    // halfHeight (for Jolt) = cylinderHeight / 2 = (totalHeight - 2*radius) / 2
    float halfHeightOfCylinder = (settings.height - 2.0f * settings.radius) * 0.5f;

    // Capsule is centered at character position
    // Bottom at y = -halfHeight - radius, Top at y = +halfHeight + radius
    JPH::Ref<JPH::Shape> standingShape = new JPH::CapsuleShape(halfHeightOfCylinder, settings.radius);

    // Create CharacterVirtual settings
    JPH::CharacterVirtualSettings charSettings;
    charSettings.mShape = standingShape;
    charSettings.mMass = settings.mass;
    charSettings.mMaxSlopeAngle = JPH::DegreesToRadians(settings.maxSlopeAngle);
    charSettings.mMaxStrength = settings.maxStrength;
    charSettings.mCharacterPadding = settings.characterPadding;
    charSettings.mPenetrationRecoverySpeed = settings.penetrationRecoverySpeed;
    charSettings.mPredictiveContactDistance = settings.predictiveContactDistance;

    // Create character
    character = std::make_unique<JPH::CharacterVirtual>(
        &charSettings,
        JPH::RVec3::sZero(),// Initial position (will be set by warp)
        JPH::Quat::sIdentity(),
        0,// User data
        physicsSystem
    );

    character->SetListener(nullptr);// Can add custom listener later

    // Set initial max speed
    maxSpeed = 5.0f;// Default movement speed
    desiredHorizontalVelocity = glm::vec3(0.0f);

    // Initialize positions from character's current position
    JPH::RVec3 initialPos = character->GetPosition();
    currentPosition = glm::vec3(initialPos.GetX(), initialPos.GetY(), initialPos.GetZ());
    previousPosition = currentPosition;
}

CharacterController::~CharacterController() {
    // character will be automatically destroyed
}

void CharacterController::move(const glm::vec3& movementDirection, float deltaTime) {
    // Store desired horizontal velocity (will be applied in update())
    glm::vec3 horizontalDir(movementDirection.x, 0, movementDirection.z);
    float horizontalSpeed = glm::length(horizontalDir);

    if (horizontalSpeed > 0.001f) {
        horizontalDir = glm::normalize(horizontalDir);
        desiredHorizontalVelocity = horizontalDir * maxSpeed;
    } else {
        desiredHorizontalVelocity = glm::vec3(0.0f);
    }
}

void CharacterController::moveAlong(const glm::vec2& inputVector, const glm::vec3& forwardDirection, float deltaTime) {
    glm::vec3 forward = glm::normalize(glm::vec3(forwardDirection.x, 0.0f, forwardDirection.z));
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 movementDirection = forward * inputVector.y + right * inputVector.x;

    float length = glm::length(movementDirection);
    if (length > 1.0f) {
        movementDirection = movementDirection / length;
    }

    movementDirection *= maxSpeed;

    move(movementDirection, deltaTime);
}

void CharacterController::jump(float jumpSpeed) {
    if (isOnGround()) {
        JPH::Vec3 currentVel = character->GetLinearVelocity();
        currentVel.SetY(jumpSpeed);
        character->SetLinearVelocity(currentVel);
    }
}

void CharacterController::warp(const glm::vec3& position) {
    character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
    // Reset interpolation positions on warp
    previousPosition = position;
    currentPosition = position;
}

bool CharacterController::isOnGround() const {
    return character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

bool CharacterController::isSliding() const {
    return character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnSteepGround;
}

glm::vec3 CharacterController::getPosition() const {
    // Return the current physics position (not interpolated)
    return currentPosition;
}

glm::vec3 CharacterController::getInterpolatedPosition(float alpha) const {
    // Linear interpolation between previous and current position
    // alpha = 0.0 means use previous position (start of physics step)
    // alpha = 1.0 means use current position (end of physics step)
    return glm::mix(previousPosition, currentPosition, alpha);
}

glm::vec3 CharacterController::getVelocity() const {
    JPH::Vec3 vel = character->GetLinearVelocity();
    return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
}

glm::vec3 CharacterController::getGroundNormal() const {
    JPH::Vec3 normal = character->GetGroundNormal();
    return glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
}

BodyHandle CharacterController::getBodyHandle() const {
    return BodyHandle{ character->GetInnerBodyID().GetIndexAndSequenceNumber() };
}

void CharacterController::setLinearVelocity(const glm::vec3& velocity) {
    character->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

void CharacterController::setMaxSpeed(float speed) {
    maxSpeed = speed;
}

void CharacterController::setGravity(const glm::vec3& gravity) {
    currentGravity = gravity;
}

void CharacterController::update(float deltaTime, const glm::vec3& gravity) {
    auto* physicsSystem = physics->getPhysicsSystem();
    auto* tempAllocator = physics->getTempAllocator();

    // Note: previousPosition should be set externally before the physics update loop
    // to handle multiple physics steps correctly

    JPH::Vec3 newVelocity;

    if (character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
        newVelocity = JPH::Vec3(desiredHorizontalVelocity.x, 0.0f, desiredHorizontalVelocity.z);
    } else {
        // In air: manually apply gravity to vertical component
        JPH::Vec3 currentVel = character->GetLinearVelocity();

        JPH::Vec3 up = character->GetUp();
        float verticalSpeed = currentVel.Dot(up);

        verticalSpeed += gravity.y * deltaTime;

        newVelocity = JPH::Vec3(desiredHorizontalVelocity.x, verticalSpeed, desiredHorizontalVelocity.z);
    }

    character->SetLinearVelocity(newVelocity);

    // Update character (performs collision detection and movement)
    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;

    // Configure update settings to prevent sticking and improve movement
    // Reduced mStickToFloorStepDown to minimize vertical jitter on flat surfaces
    updateSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -0.01f, 0.0f);// Very small step down - reduces jitter
    updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, 0.15f, 0.0f);// Allow stepping up small obstacles
    updateSettings.mWalkStairsMinStepForward = 0.1f;// Minimum forward distance for step up
    updateSettings.mWalkStairsStepDownExtra = JPH::Vec3(0.0f, 0.0f, 0.0f);// Disabled to reduce jitter

    // Use system default filters
    auto broadPhaseFilter = physicsSystem->GetDefaultBroadPhaseLayerFilter(1);// MOVING layer
    auto layerFilter = physicsSystem->GetDefaultLayerFilter(1);// MOVING layer

    character->ExtendedUpdate(
        deltaTime,
        JPH::Vec3(gravity.x, gravity.y, gravity.z),
        updateSettings,
        broadPhaseFilter,
        layerFilter,
        {},// Body filter
        {},// Shape filter
        *tempAllocator
    );

    // Update current position after physics step
    JPH::RVec3 pos = character->GetPosition();
    currentPosition = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
}
