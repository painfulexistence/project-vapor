#include "character_controller.hpp"
#include "physics_3d.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Core/Factory.h>

CharacterController::CharacterController(Physics3D* physics, const CharacterControllerSettings& settings)
    : physics(physics)
    , settings(settings)
    , currentGravity(0, -9.81f, 0)
{
    auto* physicsSystem = physics->getPhysicsSystem();

    // Create standing shape (capsule)
    JPH::Ref<JPH::Shape> standingShape = JPH::RotatedTranslatedShapeSettings(
        JPH::Vec3(0, 0.5f * settings.height + settings.radius, 0),
        JPH::Quat::sIdentity(),
        new JPH::CapsuleShape(0.5f * settings.height, settings.radius)
    ).Create().Get();

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
        JPH::RVec3::sZero(),  // Initial position (will be set by warp)
        JPH::Quat::sIdentity(),
        0,  // User data
        physicsSystem
    );

    character->SetListener(nullptr);  // Can add custom listener later

    // Set initial max speed
    maxSpeed = 5.0f;  // Default movement speed
}

CharacterController::~CharacterController() {
    // character will be automatically destroyed
}

void CharacterController::move(const glm::vec3& movementDirection, float deltaTime) {
    // Get current velocity to preserve vertical component
    JPH::Vec3 currentVel = character->GetLinearVelocity();

    // Apply horizontal movement (preserve vertical velocity for gravity/jumping)
    JPH::Vec3 horizontalVel(movementDirection.x, 0, movementDirection.z);
    float horizontalSpeed = horizontalVel.Length();

    // Clamp horizontal speed to max speed
    if (horizontalSpeed > maxSpeed) {
        horizontalVel = horizontalVel.Normalized() * maxSpeed;
    }

    // Combine horizontal movement with current vertical velocity
    JPH::Vec3 newVelocity(horizontalVel.GetX(), currentVel.GetY(), horizontalVel.GetZ());
    character->SetLinearVelocity(newVelocity);
}

void CharacterController::moveRelativeToDirection(const glm::vec2& inputVector, const glm::vec3& forwardDirection, float deltaTime) {
    // Normalize the forward direction and remove vertical component
    glm::vec3 forward = glm::normalize(glm::vec3(forwardDirection.x, 0.0f, forwardDirection.z));

    // Calculate right direction (perpendicular to forward)
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

    // Calculate movement direction based on input
    // inputVector.x = right/left (-1 to 1)
    // inputVector.y = forward/backward (-1 to 1)
    glm::vec3 movementDirection = forward * inputVector.y + right * inputVector.x;

    // Normalize if needed (diagonal movement)
    float length = glm::length(movementDirection);
    if (length > 1.0f) {
        movementDirection = movementDirection / length;
    }

    // Scale by max speed
    movementDirection *= maxSpeed;

    // Apply movement
    move(movementDirection, deltaTime);
}

void CharacterController::jump(float jumpSpeed) {
    // Only jump if on ground and cooldown has expired
    if (isOnGround() && jumpCooldown <= 0.0f) {
        JPH::Vec3 currentVel = character->GetLinearVelocity();
        currentVel.SetY(jumpSpeed);
        character->SetLinearVelocity(currentVel);
        jumpCooldown = JUMP_COOLDOWN_TIME;  // Start cooldown
    }
}

void CharacterController::warp(const glm::vec3& position) {
    character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
}

bool CharacterController::isOnGround() const {
    return character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

bool CharacterController::isSliding() const {
    return character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnSteepGround;
}

glm::vec3 CharacterController::getPosition() const {
    JPH::RVec3 pos = character->GetPosition();
    return glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
}

glm::vec3 CharacterController::getVelocity() const {
    JPH::Vec3 vel = character->GetLinearVelocity();
    return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
}

glm::vec3 CharacterController::getGroundNormal() const {
    JPH::Vec3 normal = character->GetGroundNormal();
    return glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
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

    // Update jump cooldown
    if (jumpCooldown > 0.0f) {
        jumpCooldown -= deltaTime;
    }

    // Update character (performs collision detection and movement)
    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;

    // Use system default filters
    auto broadPhaseFilter = physicsSystem->GetDefaultBroadPhaseLayerFilter(1);  // MOVING layer
    auto layerFilter = physicsSystem->GetDefaultLayerFilter(1);  // MOVING layer

    character->ExtendedUpdate(
        deltaTime,
        JPH::Vec3(gravity.x, gravity.y, gravity.z),
        updateSettings,
        broadPhaseFilter,
        layerFilter,
        {},  // Body filter
        {},  // Shape filter
        *tempAllocator
    );
}
