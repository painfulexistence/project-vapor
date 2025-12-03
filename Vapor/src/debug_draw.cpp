#include "debug_draw.hpp"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace Vapor {

void DebugDraw::clear() {
    lineVertices.clear();
    triangleVertices.clear();
}

glm::vec3 DebugDraw::rotatePoint(const glm::vec3& point, const glm::quat& rotation) const {
    return rotation * point;
}

void DebugDraw::addLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4& color) {
    lineVertices.push_back({start, color});
    lineVertices.push_back({end, color});
}

void DebugDraw::addTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                            const glm::vec4& color, bool wireframe) {
    if (wireframe) {
        addLine(v0, v1, color);
        addLine(v1, v2, color);
        addLine(v2, v0, color);
    } else {
        triangleVertices.push_back({v0, color});
        triangleVertices.push_back({v1, color});
        triangleVertices.push_back({v2, color});
    }
}

void DebugDraw::addBox(const glm::vec3& center, const glm::vec3& halfExtents,
                       const glm::quat& rotation, const glm::vec4& color) {
    // 8 corners of the box in local space
    glm::vec3 corners[8] = {
        glm::vec3(-halfExtents.x, -halfExtents.y, -halfExtents.z),
        glm::vec3( halfExtents.x, -halfExtents.y, -halfExtents.z),
        glm::vec3( halfExtents.x,  halfExtents.y, -halfExtents.z),
        glm::vec3(-halfExtents.x,  halfExtents.y, -halfExtents.z),
        glm::vec3(-halfExtents.x, -halfExtents.y,  halfExtents.z),
        glm::vec3( halfExtents.x, -halfExtents.y,  halfExtents.z),
        glm::vec3( halfExtents.x,  halfExtents.y,  halfExtents.z),
        glm::vec3(-halfExtents.x,  halfExtents.y,  halfExtents.z),
    };

    // Transform corners to world space
    for (int i = 0; i < 8; ++i) {
        corners[i] = center + rotatePoint(corners[i], rotation);
    }

    // 12 edges of the box
    // Bottom face
    addLine(corners[0], corners[1], color);
    addLine(corners[1], corners[2], color);
    addLine(corners[2], corners[3], color);
    addLine(corners[3], corners[0], color);
    // Top face
    addLine(corners[4], corners[5], color);
    addLine(corners[5], corners[6], color);
    addLine(corners[6], corners[7], color);
    addLine(corners[7], corners[4], color);
    // Vertical edges
    addLine(corners[0], corners[4], color);
    addLine(corners[1], corners[5], color);
    addLine(corners[2], corners[6], color);
    addLine(corners[3], corners[7], color);
}

void DebugDraw::addSphere(const glm::vec3& center, float radius, const glm::vec4& color,
                          int segments) {
    const float pi = glm::pi<float>();

    // Draw 3 circles (XY, XZ, YZ planes)
    for (int i = 0; i < segments; ++i) {
        float angle1 = (float(i) / segments) * 2.0f * pi;
        float angle2 = (float(i + 1) / segments) * 2.0f * pi;

        // XY plane (around Z axis)
        glm::vec3 p1 = center + glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * radius;
        glm::vec3 p2 = center + glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * radius;
        addLine(p1, p2, color);

        // XZ plane (around Y axis)
        p1 = center + glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * radius;
        p2 = center + glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * radius;
        addLine(p1, p2, color);

        // YZ plane (around X axis)
        p1 = center + glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * radius;
        p2 = center + glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * radius;
        addLine(p1, p2, color);
    }
}

void DebugDraw::addCapsule(const glm::vec3& center, float halfHeight, float radius,
                           const glm::quat& rotation, const glm::vec4& color, int segments) {
    const float pi = glm::pi<float>();

    // Local up direction (Y axis in local space)
    glm::vec3 localUp = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 localRight = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 localForward = glm::vec3(0.0f, 0.0f, 1.0f);

    // Transform to world space
    glm::vec3 up = rotatePoint(localUp, rotation);
    glm::vec3 right = rotatePoint(localRight, rotation);
    glm::vec3 forward = rotatePoint(localForward, rotation);

    // Top and bottom sphere centers
    glm::vec3 topCenter = center + up * halfHeight;
    glm::vec3 bottomCenter = center - up * halfHeight;

    // Draw top hemisphere
    for (int i = 0; i < segments; ++i) {
        float angle1 = (float(i) / segments) * 2.0f * pi;
        float angle2 = (float(i + 1) / segments) * 2.0f * pi;

        // Horizontal circle at top
        glm::vec3 p1 = topCenter + (right * std::cos(angle1) + forward * std::sin(angle1)) * radius;
        glm::vec3 p2 = topCenter + (right * std::cos(angle2) + forward * std::sin(angle2)) * radius;
        addLine(p1, p2, color);

        // Horizontal circle at bottom
        p1 = bottomCenter + (right * std::cos(angle1) + forward * std::sin(angle1)) * radius;
        p2 = bottomCenter + (right * std::cos(angle2) + forward * std::sin(angle2)) * radius;
        addLine(p1, p2, color);
    }

    // Draw hemisphere arcs (top)
    int halfSegments = segments / 2;
    for (int i = 0; i < halfSegments; ++i) {
        float angle1 = (float(i) / halfSegments) * pi * 0.5f;
        float angle2 = (float(i + 1) / halfSegments) * pi * 0.5f;

        float h1 = std::sin(angle1) * radius;
        float r1 = std::cos(angle1) * radius;
        float h2 = std::sin(angle2) * radius;
        float r2 = std::cos(angle2) * radius;

        // Front arc
        glm::vec3 p1 = topCenter + up * h1 + forward * r1;
        glm::vec3 p2 = topCenter + up * h2 + forward * r2;
        addLine(p1, p2, color);

        // Back arc
        p1 = topCenter + up * h1 - forward * r1;
        p2 = topCenter + up * h2 - forward * r2;
        addLine(p1, p2, color);

        // Right arc
        p1 = topCenter + up * h1 + right * r1;
        p2 = topCenter + up * h2 + right * r2;
        addLine(p1, p2, color);

        // Left arc
        p1 = topCenter + up * h1 - right * r1;
        p2 = topCenter + up * h2 - right * r2;
        addLine(p1, p2, color);
    }

    // Draw hemisphere arcs (bottom)
    for (int i = 0; i < halfSegments; ++i) {
        float angle1 = (float(i) / halfSegments) * pi * 0.5f;
        float angle2 = (float(i + 1) / halfSegments) * pi * 0.5f;

        float h1 = std::sin(angle1) * radius;
        float r1 = std::cos(angle1) * radius;
        float h2 = std::sin(angle2) * radius;
        float r2 = std::cos(angle2) * radius;

        // Front arc
        glm::vec3 p1 = bottomCenter - up * h1 + forward * r1;
        glm::vec3 p2 = bottomCenter - up * h2 + forward * r2;
        addLine(p1, p2, color);

        // Back arc
        p1 = bottomCenter - up * h1 - forward * r1;
        p2 = bottomCenter - up * h2 - forward * r2;
        addLine(p1, p2, color);

        // Right arc
        p1 = bottomCenter - up * h1 + right * r1;
        p2 = bottomCenter - up * h2 + right * r2;
        addLine(p1, p2, color);

        // Left arc
        p1 = bottomCenter - up * h1 - right * r1;
        p2 = bottomCenter - up * h2 - right * r2;
        addLine(p1, p2, color);
    }

    // Vertical lines connecting hemispheres
    addLine(topCenter + forward * radius, bottomCenter + forward * radius, color);
    addLine(topCenter - forward * radius, bottomCenter - forward * radius, color);
    addLine(topCenter + right * radius, bottomCenter + right * radius, color);
    addLine(topCenter - right * radius, bottomCenter - right * radius, color);
}

void DebugDraw::addCylinder(const glm::vec3& center, float halfHeight, float radius,
                            const glm::quat& rotation, const glm::vec4& color, int segments) {
    const float pi = glm::pi<float>();

    glm::vec3 up = rotatePoint(glm::vec3(0.0f, 1.0f, 0.0f), rotation);
    glm::vec3 right = rotatePoint(glm::vec3(1.0f, 0.0f, 0.0f), rotation);
    glm::vec3 forward = rotatePoint(glm::vec3(0.0f, 0.0f, 1.0f), rotation);

    glm::vec3 topCenter = center + up * halfHeight;
    glm::vec3 bottomCenter = center - up * halfHeight;

    // Draw top and bottom circles
    for (int i = 0; i < segments; ++i) {
        float angle1 = (float(i) / segments) * 2.0f * pi;
        float angle2 = (float(i + 1) / segments) * 2.0f * pi;

        glm::vec3 offset1 = right * std::cos(angle1) * radius + forward * std::sin(angle1) * radius;
        glm::vec3 offset2 = right * std::cos(angle2) * radius + forward * std::sin(angle2) * radius;

        // Top circle
        addLine(topCenter + offset1, topCenter + offset2, color);
        // Bottom circle
        addLine(bottomCenter + offset1, bottomCenter + offset2, color);
    }

    // Vertical lines
    int verticalLines = 4;
    for (int i = 0; i < verticalLines; ++i) {
        float angle = (float(i) / verticalLines) * 2.0f * pi;
        glm::vec3 offset = right * std::cos(angle) * radius + forward * std::sin(angle) * radius;
        addLine(topCenter + offset, bottomCenter + offset, color);
    }
}

void DebugDraw::addCone(const glm::vec3& apex, const glm::vec3& direction, float height,
                        float radius, const glm::vec4& color, int segments) {
    const float pi = glm::pi<float>();

    glm::vec3 dir = glm::normalize(direction);
    glm::vec3 baseCenter = apex + dir * height;

    // Find perpendicular vectors
    glm::vec3 up = std::abs(dir.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(up, dir));
    glm::vec3 forward = glm::cross(dir, right);

    // Draw base circle and lines to apex
    for (int i = 0; i < segments; ++i) {
        float angle1 = (float(i) / segments) * 2.0f * pi;
        float angle2 = (float(i + 1) / segments) * 2.0f * pi;

        glm::vec3 p1 = baseCenter + right * std::cos(angle1) * radius + forward * std::sin(angle1) * radius;
        glm::vec3 p2 = baseCenter + right * std::cos(angle2) * radius + forward * std::sin(angle2) * radius;

        // Base circle
        addLine(p1, p2, color);
    }

    // Lines from apex to base
    int lineCount = 4;
    for (int i = 0; i < lineCount; ++i) {
        float angle = (float(i) / lineCount) * 2.0f * pi;
        glm::vec3 basePoint = baseCenter + right * std::cos(angle) * radius + forward * std::sin(angle) * radius;
        addLine(apex, basePoint, color);
    }
}

void DebugDraw::addArrow(const glm::vec3& start, const glm::vec3& end, const glm::vec4& color,
                         float headSize) {
    glm::vec3 dir = end - start;
    float length = glm::length(dir);
    if (length < 0.0001f) return;

    dir = dir / length;

    // Main line
    addLine(start, end, color);

    // Arrow head
    float headLength = length * headSize;
    float headRadius = headLength * 0.5f;

    glm::vec3 up = std::abs(dir.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(up, dir));
    glm::vec3 forward = glm::cross(dir, right);

    glm::vec3 headBase = end - dir * headLength;

    addLine(end, headBase + right * headRadius, color);
    addLine(end, headBase - right * headRadius, color);
    addLine(end, headBase + forward * headRadius, color);
    addLine(end, headBase - forward * headRadius, color);
}

void DebugDraw::addAABB(const glm::vec3& min, const glm::vec3& max, const glm::vec4& color) {
    glm::vec3 center = (min + max) * 0.5f;
    glm::vec3 halfExtents = (max - min) * 0.5f;
    addBox(center, halfExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), color);
}

void DebugDraw::addFrustum(const glm::mat4& viewProj, const glm::vec4& color) {
    glm::mat4 invViewProj = glm::inverse(viewProj);

    // NDC corners
    glm::vec4 ndcCorners[8] = {
        {-1, -1, -1, 1}, { 1, -1, -1, 1}, { 1,  1, -1, 1}, {-1,  1, -1, 1},  // Near
        {-1, -1,  1, 1}, { 1, -1,  1, 1}, { 1,  1,  1, 1}, {-1,  1,  1, 1},  // Far
    };

    glm::vec3 worldCorners[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec4 world = invViewProj * ndcCorners[i];
        worldCorners[i] = glm::vec3(world) / world.w;
    }

    // Near plane
    addLine(worldCorners[0], worldCorners[1], color);
    addLine(worldCorners[1], worldCorners[2], color);
    addLine(worldCorners[2], worldCorners[3], color);
    addLine(worldCorners[3], worldCorners[0], color);

    // Far plane
    addLine(worldCorners[4], worldCorners[5], color);
    addLine(worldCorners[5], worldCorners[6], color);
    addLine(worldCorners[6], worldCorners[7], color);
    addLine(worldCorners[7], worldCorners[4], color);

    // Connecting edges
    addLine(worldCorners[0], worldCorners[4], color);
    addLine(worldCorners[1], worldCorners[5], color);
    addLine(worldCorners[2], worldCorners[6], color);
    addLine(worldCorners[3], worldCorners[7], color);
}

void DebugDraw::addCircle(const glm::vec3& center, const glm::vec3& normal, float radius,
                          const glm::vec4& color, int segments) {
    const float pi = glm::pi<float>();

    glm::vec3 n = glm::normalize(normal);
    glm::vec3 up = std::abs(n.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(up, n));
    glm::vec3 forward = glm::cross(n, right);

    for (int i = 0; i < segments; ++i) {
        float angle1 = (float(i) / segments) * 2.0f * pi;
        float angle2 = (float(i + 1) / segments) * 2.0f * pi;

        glm::vec3 p1 = center + right * std::cos(angle1) * radius + forward * std::sin(angle1) * radius;
        glm::vec3 p2 = center + right * std::cos(angle2) * radius + forward * std::sin(angle2) * radius;
        addLine(p1, p2, color);
    }
}

void DebugDraw::addArc(const glm::vec3& center, const glm::vec3& normal, const glm::vec3& startDir,
                       float radius, float angle, const glm::vec4& color, int segments) {
    glm::vec3 n = glm::normalize(normal);
    glm::vec3 start = glm::normalize(startDir);

    for (int i = 0; i < segments; ++i) {
        float a1 = (float(i) / segments) * angle;
        float a2 = (float(i + 1) / segments) * angle;

        glm::quat rot1 = glm::angleAxis(a1, n);
        glm::quat rot2 = glm::angleAxis(a2, n);

        glm::vec3 p1 = center + rot1 * start * radius;
        glm::vec3 p2 = center + rot2 * start * radius;
        addLine(p1, p2, color);
    }
}

void DebugDraw::addCross(const glm::vec3& center, float size, const glm::vec4& color) {
    float half = size * 0.5f;
    addLine(center - glm::vec3(half, 0, 0), center + glm::vec3(half, 0, 0), color);
    addLine(center - glm::vec3(0, half, 0), center + glm::vec3(0, half, 0), color);
    addLine(center - glm::vec3(0, 0, half), center + glm::vec3(0, 0, half), color);
}

void DebugDraw::addAxes(const glm::vec3& center, const glm::quat& rotation, float size) {
    glm::vec3 x = rotation * glm::vec3(size, 0, 0);
    glm::vec3 y = rotation * glm::vec3(0, size, 0);
    glm::vec3 z = rotation * glm::vec3(0, 0, size);

    addArrow(center, center + x, DebugColors::Red, 0.15f);
    addArrow(center, center + y, DebugColors::Green, 0.15f);
    addArrow(center, center + z, DebugColors::Blue, 0.15f);
}

} // namespace Vapor
