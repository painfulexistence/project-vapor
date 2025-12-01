#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <vector>

namespace JPH {
    class VehicleConstraint;
    class Body;
    class BodyID;
}

class Physics3D;

struct WheelSettings {
    glm::vec3 position;           // Position relative to vehicle center
    glm::vec3 suspensionDirection = glm::vec3(0, -1, 0);  // Usually downward
    glm::vec3 wheelForward = glm::vec3(0, 0, 1);          // Forward direction
    glm::vec3 wheelUp = glm::vec3(0, 1, 0);               // Up direction

    float suspensionMinLength = 0.3f;
    float suspensionMaxLength = 0.5f;
    float suspensionPreloadLength = 0.4f;
    float suspensionFrequency = 1.5f;
    float suspensionDamping = 0.5f;

    float wheelRadius = 0.3f;
    float wheelWidth = 0.1f;
    bool enableTraction = true;
};

struct VehicleSettings {
    float mass = 1500.0f;          // Vehicle mass in kg
    glm::vec3 dimensions = glm::vec3(2.0f, 1.0f, 4.0f);  // Half-extents (width, height, length)

    float maxSteeringAngle = 0.7f; // Radians (~40 degrees)
    float maxEngineTorque = 500.0f;
    float maxBrakeTorque = 1500.0f;

    std::vector<WheelSettings> wheels;

    // Pre-configured templates
    static VehicleSettings createSedan();
    static VehicleSettings createTruck();
};

class VehicleController {
public:
    VehicleController(Physics3D* physics, const VehicleSettings& settings, const glm::vec3& position, const glm::quat& rotation);
    ~VehicleController();

    // Control inputs (normalized -1 to 1)
    void setThrottle(float throttle);    // Forward/backward acceleration
    void setSteering(float steering);    // Left/right steering
    void setBrake(float brake);          // Brake force (0 to 1)
    void setHandbrake(bool enabled);     // Handbrake on/off

    // State queries
    glm::vec3 getPosition() const;
    glm::quat getRotation() const;
    glm::vec3 getLinearVelocity() const;
    glm::vec3 getAngularVelocity() const;
    float getSpeed() const;              // Speed in m/s
    float getSpeedKmh() const;           // Speed in km/h

    // Wheel states
    int getWheelCount() const;
    bool isWheelInContact(int wheelIndex) const;
    glm::vec3 getWheelPosition(int wheelIndex) const;
    glm::vec3 getWheelContactNormal(int wheelIndex) const;
    float getWheelSuspensionLength(int wheelIndex) const;

    // Internal update (called by Physics3D)
    void update(float deltaTime);

    // Get underlying physics body ID
    JPH::BodyID getBodyID() const;

private:
    Physics3D* physics;
    std::unique_ptr<JPH::VehicleConstraint> vehicleConstraint;
    JPH::Body* vehicleBody;
    VehicleSettings settings;

    float currentThrottle = 0.0f;
    float currentSteering = 0.0f;
    float currentBrake = 0.0f;
    bool handbrakeEnabled = false;
};
