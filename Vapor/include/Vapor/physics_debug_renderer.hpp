#pragma once
#include "debug_draw.hpp"
#include "physics_3d.hpp"
#include <memory>

namespace JPH {
    class PhysicsSystem;
    class BodyInterface;
    class Body;
    class Shape;
}// namespace JPH

namespace Vapor {

    // Configuration for what to draw
    struct PhysicsDebugConfig {
        bool drawBodies = true;
        bool drawTriggers = true;
        bool drawConstraints = false;
        bool drawContactPoints = false;
        bool drawVelocities = false;
        bool drawBoundingBoxes = false;
        bool drawCenterOfMass = false;

        // Color based on body state
        bool colorByState = true;// If false, use a single color for all bodies

        // Single color mode
        glm::vec4 defaultColor = DebugColors::Green;

        // Velocity visualization scale
        float velocityScale = 0.1f;
    };

    // Physics debug renderer - collects physics data and generates debug draw commands
    // This class knows about Jolt Physics and translates it to generic debug draw commands
    class PhysicsDebugRenderer {
    public:
        PhysicsDebugRenderer();
        ~PhysicsDebugRenderer();

        // Set the physics system to visualize
        void setPhysicsSystem(Physics3D* physics);

        // Set the debug draw queue to output commands to
        void setDebugDraw(std::shared_ptr<DebugDraw> debugDraw);

        // Configuration
        void setConfig(const PhysicsDebugConfig& config) {
            this->config = config;
        }
        PhysicsDebugConfig& getConfig() {
            return config;
        }
        const PhysicsDebugConfig& getConfig() const {
            return config;
        }

        // Enable/disable rendering
        void setEnabled(bool enabled) {
            this->enabled = enabled;
        }
        bool isEnabled() const {
            return enabled;
        }

        // Generate debug draw commands from current physics state
        // Call this once per frame before rendering
        void update();

    private:
        Physics3D* physics = nullptr;
        std::shared_ptr<DebugDraw> debugDraw = nullptr;
        PhysicsDebugConfig config;
        bool enabled = false;

        // Draw individual body based on its shape
        void drawBody(const JPH::Body& body, const glm::vec4& color);

        // Draw shape at given transform
        void drawShape(
            const JPH::Shape* shape, const glm::vec3& position, const glm::quat& rotation, const glm::vec4& color
        );

        // Get color for body based on its state
        glm::vec4 getBodyColor(const JPH::Body& body) const;
    };

}// namespace Vapor
