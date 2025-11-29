#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <vector>

namespace JPH {
    class BodyID;
}

class Physics3D;

struct FluidVolumeSettings {
    glm::vec3 position;
    glm::vec3 dimensions;  // Half-extents (width, height, depth)
    glm::quat rotation = glm::quat(1, 0, 0, 0);

    float density = 1000.0f;           // Water density in kg/m³ (1000 for water)
    float linearDragCoefficient = 0.5f;   // Drag coefficient for linear velocity
    float angularDragCoefficient = 0.5f;  // Drag coefficient for angular velocity
    glm::vec3 flowVelocity = glm::vec3(0.0f);  // Water current velocity

    // Pre-configured templates
    static FluidVolumeSettings createWaterVolume(const glm::vec3& position, const glm::vec3& dimensions);
    static FluidVolumeSettings createOilVolume(const glm::vec3& position, const glm::vec3& dimensions);
};

class FluidVolume {
public:
    FluidVolume(Physics3D* physics, const FluidVolumeSettings& settings);
    ~FluidVolume();

    // Queries
    bool isBodyInFluid(const JPH::BodyID& bodyID) const;
    float getSubmergedVolume(const JPH::BodyID& bodyID) const;
    glm::vec3 getFluidVelocityAt(const glm::vec3& position) const;

    // Settings
    void setDensity(float density);
    float getDensity() const { return settings.density; }
    void setLinearDragCoefficient(float coefficient);
    float getLinearDragCoefficient() const { return settings.linearDragCoefficient; }
    void setAngularDragCoefficient(float coefficient);
    float getAngularDragCoefficient() const { return settings.angularDragCoefficient; }
    void setFlowVelocity(const glm::vec3& velocity);
    glm::vec3 getFlowVelocity() const { return settings.flowVelocity; }

    // Position/Rotation
    glm::vec3 getPosition() const { return settings.position; }
    glm::vec3 getDimensions() const { return settings.dimensions; }
    glm::quat getRotation() const { return settings.rotation; }
    void setPosition(const glm::vec3& position);
    void setRotation(const glm::quat& rotation);

    // Internal update (called by Physics3D)
    void applyForcesToBodies(float deltaTime);

private:
    Physics3D* physics;
    FluidVolumeSettings settings;

    // Helper methods
    bool isPointInFluid(const glm::vec3& point) const;
    float calculateSubmergedRatio(const JPH::BodyID& bodyID) const;
    glm::vec3 calculateBuoyancyForce(const JPH::BodyID& bodyID, float submergedVolume) const;
    glm::vec3 calculateDragForce(const JPH::BodyID& bodyID, const glm::vec3& velocity) const;
};
