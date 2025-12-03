#include "physics_debug_renderer.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>

namespace Vapor {

PhysicsDebugRenderer::PhysicsDebugRenderer() = default;
PhysicsDebugRenderer::~PhysicsDebugRenderer() = default;

void PhysicsDebugRenderer::setPhysicsSystem(Physics3D* physics) {
    this->physics = physics;
}

void PhysicsDebugRenderer::setDebugDraw(DebugDraw* debugDraw) {
    this->debugDraw = debugDraw;
}

void PhysicsDebugRenderer::update() {
    if (!enabled || !physics || !debugDraw) {
        return;
    }

    JPH::PhysicsSystem* physicsSystem = physics->getPhysicsSystem();
    if (!physicsSystem) {
        return;
    }

    const JPH::BodyLockInterface& bodyLockInterface = physicsSystem->GetBodyLockInterface();

    // Get all body IDs
    JPH::BodyIDVector bodyIDs;
    physicsSystem->GetBodies(bodyIDs);

    // Draw each body
    for (const JPH::BodyID& bodyID : bodyIDs) {
        JPH::BodyLockRead lock(bodyLockInterface, bodyID);
        if (!lock.Succeeded()) {
            continue;
        }

        const JPH::Body& body = lock.GetBody();

        // Skip triggers if not configured to draw them
        if (body.IsSensor() && !config.drawTriggers) {
            continue;
        }

        // Skip regular bodies if not configured to draw them
        if (!body.IsSensor() && !config.drawBodies) {
            continue;
        }

        glm::vec4 color = getBodyColor(body);
        drawBody(body, color);

        // Draw velocity vector if enabled
        if (config.drawVelocities && body.GetMotionType() == JPH::EMotionType::Dynamic) {
            JPH::RVec3 pos = body.GetPosition();
            JPH::Vec3 vel = body.GetLinearVelocity();

            glm::vec3 position(pos.GetX(), pos.GetY(), pos.GetZ());
            glm::vec3 velocity(vel.GetX(), vel.GetY(), vel.GetZ());

            if (glm::length(velocity) > 0.01f) {
                debugDraw->addArrow(position, position + velocity * config.velocityScale,
                                   DebugColors::Yellow, 0.2f);
            }
        }

        // Draw center of mass if enabled
        if (config.drawCenterOfMass && body.GetMotionType() == JPH::EMotionType::Dynamic) {
            JPH::RVec3 com = body.GetCenterOfMassPosition();
            glm::vec3 centerOfMass(com.GetX(), com.GetY(), com.GetZ());
            debugDraw->addCross(centerOfMass, 0.1f, DebugColors::Magenta);
        }
    }
}

void PhysicsDebugRenderer::drawBody(const JPH::Body& body, const glm::vec4& color) {
    JPH::RVec3 pos = body.GetPosition();
    JPH::Quat rot = body.GetRotation();

    glm::vec3 position(pos.GetX(), pos.GetY(), pos.GetZ());
    glm::quat rotation(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());

    const JPH::Shape* shape = body.GetShape();
    drawShape(shape, position, rotation, color);

    // Draw AABB if enabled
    if (config.drawBoundingBoxes) {
        JPH::AABox bounds = body.GetWorldSpaceBounds();
        glm::vec3 min(bounds.mMin.GetX(), bounds.mMin.GetY(), bounds.mMin.GetZ());
        glm::vec3 max(bounds.mMax.GetX(), bounds.mMax.GetY(), bounds.mMax.GetZ());
        debugDraw->addAABB(min, max, glm::vec4(color.r, color.g, color.b, 0.3f));
    }
}

void PhysicsDebugRenderer::drawShape(const JPH::Shape* shape, const glm::vec3& position,
                                     const glm::quat& rotation, const glm::vec4& color) {
    if (!shape) return;

    switch (shape->GetSubType()) {
        case JPH::EShapeSubType::Box: {
            const JPH::BoxShape* box = static_cast<const JPH::BoxShape*>(shape);
            JPH::Vec3 half = box->GetHalfExtent();
            debugDraw->addBox(position,
                             glm::vec3(half.GetX(), half.GetY(), half.GetZ()),
                             rotation, color);
            break;
        }

        case JPH::EShapeSubType::Sphere: {
            const JPH::SphereShape* sphere = static_cast<const JPH::SphereShape*>(shape);
            debugDraw->addSphere(position, sphere->GetRadius(), color);
            break;
        }

        case JPH::EShapeSubType::Capsule: {
            const JPH::CapsuleShape* capsule = static_cast<const JPH::CapsuleShape*>(shape);
            debugDraw->addCapsule(position,
                                 capsule->GetHalfHeightOfCylinder(),
                                 capsule->GetRadius(),
                                 rotation, color);
            break;
        }

        case JPH::EShapeSubType::Cylinder: {
            const JPH::CylinderShape* cylinder = static_cast<const JPH::CylinderShape*>(shape);
            debugDraw->addCylinder(position,
                                  cylinder->GetHalfHeight(),
                                  cylinder->GetRadius(),
                                  rotation, color);
            break;
        }

        case JPH::EShapeSubType::ConvexHull: {
            const JPH::ConvexHullShape* hull = static_cast<const JPH::ConvexHullShape*>(shape);

            // Draw convex hull edges
            uint numFaces = hull->GetNumFaces();
            for (uint f = 0; f < numFaces; ++f) {
                uint numVertices = hull->GetNumVerticesInFace(f);
                if (numVertices < 3) continue;

                // Get face vertices
                const uint8_t* indices = hull->GetFaceVertices(f);

                // Draw edges of this face
                for (uint v = 0; v < numVertices; ++v) {
                    uint idx0 = indices[v];
                    uint idx1 = indices[(v + 1) % numVertices];

                    JPH::Vec3 v0 = hull->GetPoint(idx0);
                    JPH::Vec3 v1 = hull->GetPoint(idx1);

                    glm::vec3 p0 = position + rotation * glm::vec3(v0.GetX(), v0.GetY(), v0.GetZ());
                    glm::vec3 p1 = position + rotation * glm::vec3(v1.GetX(), v1.GetY(), v1.GetZ());

                    debugDraw->addLine(p0, p1, color);
                }
            }
            break;
        }

        case JPH::EShapeSubType::Mesh: {
            // For mesh shapes, just draw the AABB (too expensive to draw all triangles)
            JPH::AABox localBounds = shape->GetLocalBounds();
            glm::vec3 center = position + rotation * glm::vec3(
                (localBounds.mMin.GetX() + localBounds.mMax.GetX()) * 0.5f,
                (localBounds.mMin.GetY() + localBounds.mMax.GetY()) * 0.5f,
                (localBounds.mMin.GetZ() + localBounds.mMax.GetZ()) * 0.5f
            );
            glm::vec3 halfExtents(
                (localBounds.mMax.GetX() - localBounds.mMin.GetX()) * 0.5f,
                (localBounds.mMax.GetY() - localBounds.mMin.GetY()) * 0.5f,
                (localBounds.mMax.GetZ() - localBounds.mMin.GetZ()) * 0.5f
            );
            debugDraw->addBox(center, halfExtents, rotation, color);
            break;
        }

        case JPH::EShapeSubType::RotatedTranslated: {
            const JPH::RotatedTranslatedShape* rts = static_cast<const JPH::RotatedTranslatedShape*>(shape);
            JPH::Vec3 localPos = rts->GetPosition();
            JPH::Quat localRot = rts->GetRotation();

            glm::vec3 newPos = position + rotation * glm::vec3(localPos.GetX(), localPos.GetY(), localPos.GetZ());
            glm::quat newRot = rotation * glm::quat(localRot.GetW(), localRot.GetX(), localRot.GetY(), localRot.GetZ());

            drawShape(rts->GetInnerShape(), newPos, newRot, color);
            break;
        }

        case JPH::EShapeSubType::Scaled: {
            const JPH::ScaledShape* scaled = static_cast<const JPH::ScaledShape*>(shape);
            // For simplicity, just draw the inner shape (scale handling would be complex)
            drawShape(scaled->GetInnerShape(), position, rotation, color);
            break;
        }

        case JPH::EShapeSubType::OffsetCenterOfMass: {
            const JPH::OffsetCenterOfMassShape* offset = static_cast<const JPH::OffsetCenterOfMassShape*>(shape);
            drawShape(offset->GetInnerShape(), position, rotation, color);
            break;
        }

        case JPH::EShapeSubType::StaticCompound:
        case JPH::EShapeSubType::MutableCompound: {
            const JPH::CompoundShape* compound = static_cast<const JPH::CompoundShape*>(shape);

            for (uint i = 0; i < compound->GetNumSubShapes(); ++i) {
                const JPH::CompoundShape::SubShape& sub = compound->GetSubShape(i);
                JPH::Vec3 subPos = sub.GetPositionCOM();
                JPH::Quat subRot = sub.GetRotation();

                glm::vec3 newPos = position + rotation * glm::vec3(subPos.GetX(), subPos.GetY(), subPos.GetZ());
                glm::quat newRot = rotation * glm::quat(subRot.GetW(), subRot.GetX(), subRot.GetY(), subRot.GetZ());

                drawShape(sub.mShape, newPos, newRot, color);
            }
            break;
        }

        default:
            // Unsupported shape - draw local bounds
            JPH::AABox localBounds = shape->GetLocalBounds();
            glm::vec3 center = position + rotation * glm::vec3(
                (localBounds.mMin.GetX() + localBounds.mMax.GetX()) * 0.5f,
                (localBounds.mMin.GetY() + localBounds.mMax.GetY()) * 0.5f,
                (localBounds.mMin.GetZ() + localBounds.mMax.GetZ()) * 0.5f
            );
            glm::vec3 halfExtents(
                (localBounds.mMax.GetX() - localBounds.mMin.GetX()) * 0.5f,
                (localBounds.mMax.GetY() - localBounds.mMin.GetY()) * 0.5f,
                (localBounds.mMax.GetZ() - localBounds.mMin.GetZ()) * 0.5f
            );
            debugDraw->addBox(center, halfExtents, rotation, color);
            break;
    }
}

glm::vec4 PhysicsDebugRenderer::getBodyColor(const JPH::Body& body) const {
    if (!config.colorByState) {
        return config.defaultColor;
    }

    // Color based on body state
    if (body.IsSensor()) {
        return DebugColors::Trigger;  // Cyan, semi-transparent
    }

    switch (body.GetMotionType()) {
        case JPH::EMotionType::Static:
            return DebugColors::StaticBody;  // Gray
        case JPH::EMotionType::Kinematic:
            return DebugColors::KinematicBody;  // Light blue
        case JPH::EMotionType::Dynamic:
            if (body.IsActive()) {
                return DebugColors::DynamicBody;  // Green
            } else {
                return DebugColors::SleepingBody;  // Orange (sleeping)
            }
        default:
            return config.defaultColor;
    }
}

} // namespace Vapor
