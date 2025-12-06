#pragma once
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace Vapor {

// Vertex for debug drawing (position + color)
struct DebugVertex {
    glm::vec3 position;
    glm::vec4 color;
};

// Debug draw command queue - graphics layer agnostic
// Collects draw commands from various systems (physics, AI, etc.)
// and is consumed by DebugDrawPass for rendering
class DebugDraw {
public:
    // Primitive drawing
    void addLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4& color);
    void addTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                     const glm::vec4& color, bool wireframe = true);

    // Shape helpers - all generate line primitives
    void addBox(const glm::vec3& center, const glm::vec3& halfExtents,
                const glm::quat& rotation, const glm::vec4& color);
    void addSphere(const glm::vec3& center, float radius, const glm::vec4& color,
                   int segments = 16);
    void addCapsule(const glm::vec3& center, float halfHeight, float radius,
                    const glm::quat& rotation, const glm::vec4& color, int segments = 12);
    void addCylinder(const glm::vec3& center, float halfHeight, float radius,
                     const glm::quat& rotation, const glm::vec4& color, int segments = 12);
    void addCone(const glm::vec3& apex, const glm::vec3& direction, float height,
                 float radius, const glm::vec4& color, int segments = 12);
    void addArrow(const glm::vec3& start, const glm::vec3& end, const glm::vec4& color,
                  float headSize = 0.1f);
    void addAABB(const glm::vec3& min, const glm::vec3& max, const glm::vec4& color);
    void addFrustum(const glm::mat4& viewProj, const glm::vec4& color);
    void addCircle(const glm::vec3& center, const glm::vec3& normal, float radius,
                   const glm::vec4& color, int segments = 32);
    void addArc(const glm::vec3& center, const glm::vec3& normal, const glm::vec3& startDir,
                float radius, float angle, const glm::vec4& color, int segments = 16);

    // Cross/axes marker
    void addCross(const glm::vec3& center, float size, const glm::vec4& color);
    void addAxes(const glm::vec3& center, const glm::quat& rotation, float size);

    // Text (screen-space, requires separate handling)
    // void addText(const glm::vec3& worldPos, const std::string& text, const glm::vec4& color);

    // Access to generated vertices (for renderer)
    const std::vector<DebugVertex>& getLineVertices() const { return lineVertices; }
    const std::vector<DebugVertex>& getTriangleVertices() const { return triangleVertices; }

    size_t getLineVertexCount() const { return lineVertices.size(); }
    size_t getTriangleVertexCount() const { return triangleVertices.size(); }

    bool hasContent() const { return !lineVertices.empty() || !triangleVertices.empty(); }

    // Clear all queued commands (call after rendering)
    void clear();

private:
    std::vector<DebugVertex> lineVertices;      // For line primitives
    std::vector<DebugVertex> triangleVertices;  // For filled triangles

    // Helper to rotate a point around origin
    glm::vec3 rotatePoint(const glm::vec3& point, const glm::quat& rotation) const;
};

// Predefined colors for convenience
namespace DebugColors {
    constexpr glm::vec4 Red     = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    constexpr glm::vec4 Green   = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    constexpr glm::vec4 Blue    = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    constexpr glm::vec4 Yellow  = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
    constexpr glm::vec4 Cyan    = glm::vec4(0.0f, 1.0f, 1.0f, 1.0f);
    constexpr glm::vec4 Magenta = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
    constexpr glm::vec4 White   = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    constexpr glm::vec4 Gray    = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    constexpr glm::vec4 Orange  = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f);

    // Physics specific
    constexpr glm::vec4 StaticBody   = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);  // Gray
    constexpr glm::vec4 DynamicBody  = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);  // Green
    constexpr glm::vec4 KinematicBody = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f); // Light blue
    constexpr glm::vec4 SleepingBody = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f);  // Orange
    constexpr glm::vec4 Trigger      = glm::vec4(0.0f, 1.0f, 1.0f, 0.5f);  // Cyan semi-transparent
}

} // namespace Vapor
