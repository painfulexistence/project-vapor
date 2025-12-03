#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

namespace JPH {
    class CharacterVirtual;
    class PhysicsSystem;
}

namespace Vapor {
    class TaskScheduler;
}

class Physics3D;

struct CharacterControllerSettings {
    float height = 1.8f;           // Capsule height
    float radius = 0.3f;           // Capsule radius
    float mass = 70.0f;            // Mass in kg
    float maxSlopeAngle = 45.0f;   // Maximum climbable slope angle (degrees)
    float maxStrength = 100.0f;    // Maximum force to push rigidbodies
    float characterPadding = 0.02f; // Padding around character
    float penetrationRecoverySpeed = 1.0f;
    float predictiveContactDistance = 0.1f;
};

class CharacterController {
public:
    CharacterController(Physics3D* physics, const CharacterControllerSettings& settings);
    ~CharacterController();

    // Movement control
    void move(const glm::vec3& movementDirection, float deltaTime);
    void moveAlong(const glm::vec2& inputVector, const glm::vec3& forwardDirection, float deltaTime);
    void jump(float jumpSpeed);
    void warp(const glm::vec3& position);  // Teleport

    // State queries
    bool isOnGround() const;
    bool isSliding() const;  // On slope too steep
    glm::vec3 getPosition() const;
    glm::vec3 getInterpolatedPosition(float alpha) const;  // Get position interpolated between previous and current
    glm::vec3 getVelocity() const;
    glm::vec3 getGroundNormal() const;

    // Property setters
    void setLinearVelocity(const glm::vec3& velocity);
    void setMaxSpeed(float speed);
    void setGravity(const glm::vec3& gravity);

    // Internal update (called by Physics3D)
    void update(float deltaTime, const glm::vec3& gravity);

    // Store current position as previous (for interpolation)
    void storePreviousPosition() {
        previousPosition = currentPosition;
    }

private:
    Physics3D* physics;
    std::unique_ptr<JPH::CharacterVirtual> character;
    CharacterControllerSettings settings;
    glm::vec3 currentGravity;
    float maxSpeed = 10.0f;
    glm::vec3 desiredHorizontalVelocity = glm::vec3(0.0f);  // Desired horizontal movement

    // For interpolation
    glm::vec3 previousPosition = glm::vec3(0.0f);
    glm::vec3 currentPosition = glm::vec3(0.0f);
};
