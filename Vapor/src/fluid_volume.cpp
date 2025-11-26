#include "fluid_volume.hpp"
#include "physics_3d.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/AABox.h>

// Template: Water volume
FluidVolumeSettings FluidVolumeSettings::createWaterVolume(const glm::vec3& position, const glm::vec3& dimensions) {
    FluidVolumeSettings settings;
    settings.position = position;
    settings.dimensions = dimensions;
    settings.density = 1000.0f;  // Water density
    settings.linearDragCoefficient = 0.5f;
    settings.angularDragCoefficient = 0.5f;
    settings.flowVelocity = glm::vec3(0.0f);
    return settings;
}

// Template: Oil volume
FluidVolumeSettings FluidVolumeSettings::createOilVolume(const glm::vec3& position, const glm::vec3& dimensions) {
    FluidVolumeSettings settings;
    settings.position = position;
    settings.dimensions = dimensions;
    settings.density = 900.0f;  // Oil density (lighter than water)
    settings.linearDragCoefficient = 0.8f;  // More viscous
    settings.angularDragCoefficient = 0.8f;
    settings.flowVelocity = glm::vec3(0.0f);
    return settings;
}

FluidVolume::FluidVolume(Physics3D* physics, const FluidVolumeSettings& settings)
    : physics(physics)
    , settings(settings)
{
}

FluidVolume::~FluidVolume() {
}

bool FluidVolume::isPointInFluid(const glm::vec3& point) const {
    // Transform point to fluid's local space
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), settings.position) * glm::mat4_cast(settings.rotation);
    glm::mat4 invTransform = glm::inverse(transform);
    glm::vec3 localPoint = glm::vec3(invTransform * glm::vec4(point, 1.0f));

    // Check if point is inside AABB
    return std::abs(localPoint.x) <= settings.dimensions.x &&
           std::abs(localPoint.y) <= settings.dimensions.y &&
           std::abs(localPoint.z) <= settings.dimensions.z;
}

bool FluidVolume::isBodyInFluid(const JPH::BodyID& bodyID) const {
    auto* bodyInterface = physics->getBodyInterface();
    JPH::RVec3 bodyPos = bodyInterface->GetPosition(bodyID);
    glm::vec3 pos(bodyPos.GetX(), bodyPos.GetY(), bodyPos.GetZ());
    return isPointInFluid(pos);
}

float FluidVolume::calculateSubmergedRatio(const JPH::BodyID& bodyID) const {
    auto* bodyInterface = physics->getBodyInterface();
    auto* physicsSystem = physics->getPhysicsSystem();

    JPH::BodyLockRead lock(physicsSystem->GetBodyLockInterface(), bodyID);
    if (!lock.Succeeded()) return 0.0f;

    const JPH::Body& body = lock.GetBody();
    JPH::AABox worldBounds = body.GetWorldSpaceBounds();

    // Get fluid bounds in world space
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), settings.position) * glm::mat4_cast(settings.rotation);
    glm::vec3 fluidMin = settings.position - settings.dimensions;
    glm::vec3 fluidMax = settings.position + settings.dimensions;

    // Convert to Jolt AABox
    JPH::AABox fluidBounds(
        JPH::Vec3(fluidMin.x, fluidMin.y, fluidMin.z),
        JPH::Vec3(fluidMax.x, fluidMax.y, fluidMax.z)
    );

    // Calculate intersection volume
    JPH::AABox intersection = worldBounds.Intersection(fluidBounds);
    if (intersection.IsValid()) {
        JPH::Vec3 intersectionSize = intersection.GetSize();
        float intersectionVolume = intersectionSize.GetX() * intersectionSize.GetY() * intersectionSize.GetZ();

        JPH::Vec3 bodySize = worldBounds.GetSize();
        float bodyVolume = bodySize.GetX() * bodySize.GetY() * bodySize.GetZ();

        if (bodyVolume > 0.0f) {
            return glm::clamp(intersectionVolume / bodyVolume, 0.0f, 1.0f);
        }
    }

    return 0.0f;
}

float FluidVolume::getSubmergedVolume(const JPH::BodyID& bodyID) const {
    auto* bodyInterface = physics->getBodyInterface();
    auto* physicsSystem = physics->getPhysicsSystem();

    JPH::BodyLockRead lock(physicsSystem->GetBodyLockInterface(), bodyID);
    if (!lock.Succeeded()) return 0.0f;

    const JPH::Body& body = lock.GetBody();
    JPH::AABox worldBounds = body.GetWorldSpaceBounds();

    JPH::Vec3 bodySize = worldBounds.GetSize();
    float bodyVolume = bodySize.GetX() * bodySize.GetY() * bodySize.GetZ();

    float submergedRatio = calculateSubmergedRatio(bodyID);
    return bodyVolume * submergedRatio;
}

glm::vec3 FluidVolume::calculateBuoyancyForce(const JPH::BodyID& bodyID, float submergedVolume) const {
    if (submergedVolume <= 0.0f) return glm::vec3(0.0f);

    // Archimedes' principle: F = ρ * V * g
    // Where ρ is fluid density, V is displaced volume, g is gravity
    glm::vec3 gravity = physics->getGravity();
    float buoyancyMagnitude = settings.density * submergedVolume * glm::length(gravity);

    // Buoyancy acts opposite to gravity
    glm::vec3 buoyancyDirection = -glm::normalize(gravity);
    return buoyancyDirection * buoyancyMagnitude;
}

glm::vec3 FluidVolume::calculateDragForce(const JPH::BodyID& bodyID, const glm::vec3& velocity) const {
    if (glm::length(velocity) < 0.001f) return glm::vec3(0.0f);

    // Drag force: F = -k * v²
    // Simplified drag model using linear drag coefficient
    float velocityMagnitude = glm::length(velocity);
    glm::vec3 dragDirection = -glm::normalize(velocity);

    float dragMagnitude = settings.linearDragCoefficient * velocityMagnitude * velocityMagnitude;
    return dragDirection * dragMagnitude;
}

glm::vec3 FluidVolume::getFluidVelocityAt(const glm::vec3& position) const {
    // For now, uniform flow velocity
    // Can be extended to support vortices, waves, etc.
    return settings.flowVelocity;
}

void FluidVolume::setDensity(float density) {
    settings.density = density;
}

void FluidVolume::setLinearDragCoefficient(float coefficient) {
    settings.linearDragCoefficient = coefficient;
}

void FluidVolume::setAngularDragCoefficient(float coefficient) {
    settings.angularDragCoefficient = coefficient;
}

void FluidVolume::setFlowVelocity(const glm::vec3& velocity) {
    settings.flowVelocity = velocity;
}

void FluidVolume::setPosition(const glm::vec3& position) {
    settings.position = position;
}

void FluidVolume::setRotation(const glm::quat& rotation) {
    settings.rotation = rotation;
}

void FluidVolume::applyForcesToBodies(float deltaTime) {
    auto* bodyInterface = physics->getBodyInterface();
    auto* physicsSystem = physics->getPhysicsSystem();

    // Get all active bodies
    JPH::BodyIDVector activeBodyIDs;
    physicsSystem->GetActiveBodies(JPH::EBodyType::RigidBody, activeBodyIDs);

    for (const JPH::BodyID& bodyID : activeBodyIDs) {
        // Check if body is in fluid
        float submergedRatio = calculateSubmergedRatio(bodyID);
        if (submergedRatio <= 0.0f) continue;

        JPH::BodyLockRead lock(physicsSystem->GetBodyLockInterface(), bodyID);
        if (!lock.Succeeded()) continue;

        const JPH::Body& body = lock.GetBody();

        // Skip static bodies
        if (body.GetMotionType() != JPH::EMotionType::Dynamic) continue;

        // Calculate submerged volume
        float submergedVolume = getSubmergedVolume(bodyID);

        // Apply buoyancy force
        glm::vec3 buoyancyForce = calculateBuoyancyForce(bodyID, submergedVolume);
        if (glm::length(buoyancyForce) > 0.0f) {
            bodyInterface->AddForce(
                bodyID,
                JPH::Vec3(buoyancyForce.x, buoyancyForce.y, buoyancyForce.z)
            );
        }

        // Apply drag forces
        JPH::Vec3 linearVel = body.GetLinearVelocity();
        glm::vec3 bodyVelocity(linearVel.GetX(), linearVel.GetY(), linearVel.GetZ());

        // Relative velocity (body velocity - fluid flow velocity)
        glm::vec3 relativeVelocity = bodyVelocity - settings.flowVelocity;

        glm::vec3 dragForce = calculateDragForce(bodyID, relativeVelocity);
        dragForce *= submergedRatio;  // Scale drag by submerged ratio

        if (glm::length(dragForce) > 0.0f) {
            bodyInterface->AddForce(
                bodyID,
                JPH::Vec3(dragForce.x, dragForce.y, dragForce.z)
            );
        }

        // Apply angular drag
        JPH::Vec3 angularVel = body.GetAngularVelocity();
        glm::vec3 angularVelocity(angularVel.GetX(), angularVel.GetY(), angularVel.GetZ());

        if (glm::length(angularVelocity) > 0.001f) {
            glm::vec3 angularDrag = -angularVelocity * settings.angularDragCoefficient * submergedRatio;
            bodyInterface->AddTorque(
                bodyID,
                JPH::Vec3(angularDrag.x, angularDrag.y, angularDrag.z)
            );
        }
    }
}
