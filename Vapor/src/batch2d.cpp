#include "Vapor/batch2d.hpp"
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

Batch2D::Batch2D() {
    m_textureSlots.resize(Batch2DConstants::MaxTextureSlots);
}

Batch2D::~Batch2D() {
    shutdown();
}

void Batch2D::init() {
    // Allocate CPU-side buffers
    m_vertexBufferBase = new Batch2DVertex[Batch2DConstants::MaxVertices];
    m_indexBufferBase = new Uint32[Batch2DConstants::MaxIndices];

    // Initialize quad vertex positions (centered at origin, size 1x1)
    m_quadVertexPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
    m_quadVertexPositions[1] = { 0.5f, -0.5f, 0.0f, 1.0f };
    m_quadVertexPositions[2] = { 0.5f, 0.5f, 0.0f, 1.0f };
    m_quadVertexPositions[3] = { -0.5f, 0.5f, 0.0f, 1.0f };

    // Initialize default UVs
    m_quadTexCoords[0] = { 0.0f, 0.0f };
    m_quadTexCoords[1] = { 1.0f, 0.0f };
    m_quadTexCoords[2] = { 1.0f, 1.0f };
    m_quadTexCoords[3] = { 0.0f, 1.0f };

    // White texture will be set by the renderer
    m_textureSlots[0] = m_whiteTexture;
}

void Batch2D::shutdown() {
    if (m_vertexBufferBase) {
        delete[] m_vertexBufferBase;
        m_vertexBufferBase = nullptr;
    }

    if (m_indexBufferBase) {
        delete[] m_indexBufferBase;
        m_indexBufferBase = nullptr;
    }
}

void Batch2D::beginScene(const glm::mat4& viewProj, BlendMode blendMode) {
    m_projectionMatrix = viewProj;
    m_currentBlendMode = blendMode;
    startBatch();
}

void Batch2D::endScene() {
    flush();
}

void Batch2D::startBatch() {
    m_indexCount = 0;
    m_vertexBufferPtr = m_vertexBufferBase;
    m_indexBufferPtr = m_indexBufferBase;
    m_textureSlotIndex = 1; // 0 is reserved for white texture
    m_textureSlots[0] = m_whiteTexture;
}

void Batch2D::flush() {
    if (m_indexCount == 0) return;

    m_stats.vertexCount += getVertexCount();
    m_stats.indexCount += m_indexCount;
    m_stats.drawCalls++;

    // Call the flush callback (set by Batch2DPass to upload and draw)
    if (m_flushCallback) {
        m_flushCallback();
    }
}

void Batch2D::resetBatch() {
    startBatch();
}

void Batch2D::nextBatch() {
    flush();
    startBatch();
}

float Batch2D::findOrAddTexture(TextureHandle texture) {
    // If invalid or white texture, use slot 0
    if (texture.rid == UINT32_MAX || texture.rid == m_whiteTexture.rid) {
        return 0.0f;
    }

    // Search for existing texture in slots
    for (Uint32 i = 1; i < m_textureSlotIndex; i++) {
        if (m_textureSlots[i].rid == texture.rid) {
            return static_cast<float>(i);
        }
    }

    // Need to add new texture
    if (m_textureSlotIndex >= Batch2DConstants::MaxTextureSlots) {
        nextBatch();
    }

    float texIndex = static_cast<float>(m_textureSlotIndex);
    m_textureSlots[m_textureSlotIndex] = texture;
    m_textureSlotIndex++;

    return texIndex;
}

// ===== Quad Drawing =====

void Batch2D::drawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) {
    drawQuad(glm::vec3(position, 0.0f), size, color);
}

void Batch2D::drawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
                        * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad(transform, color);
}

void Batch2D::drawQuad(const glm::vec2& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor) {
    drawQuad(glm::vec3(position, 0.0f), size, texture, tintColor);
}

void Batch2D::drawQuad(const glm::vec3& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
                        * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad(transform, texture, tintColor);
}

void Batch2D::drawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color) {
    drawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, color);
}

void Batch2D::drawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
                        * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
                        * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad(transform, color);
}

void Batch2D::drawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor) {
    drawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, texture, tintColor);
}

void Batch2D::drawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
                        * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
                        * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad(transform, texture, tintColor);
}

void Batch2D::drawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    drawQuad(transform, m_whiteTexture, m_quadTexCoords, color, entityID);
}

void Batch2D::drawQuad(const glm::mat4& transform, TextureHandle texture, const glm::vec4& tintColor, int entityID) {
    drawQuad(transform, texture, m_quadTexCoords, tintColor, entityID);
}

void Batch2D::drawQuad(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords, const glm::vec4& tintColor, int entityID) {
    if (m_indexCount >= Batch2DConstants::MaxIndices) {
        nextBatch();
    }

    float textureIndex = findOrAddTexture(texture);
    Uint32 vertexOffset = static_cast<Uint32>(m_vertexBufferPtr - m_vertexBufferBase);

    // Add 4 vertices
    for (int i = 0; i < 4; i++) {
        m_vertexBufferPtr->position = transform * m_quadVertexPositions[i];
        m_vertexBufferPtr->color = tintColor;
        m_vertexBufferPtr->uv = texCoords[i];
        m_vertexBufferPtr->texIndex = textureIndex;
        m_vertexBufferPtr->entityID = static_cast<float>(entityID);
        m_vertexBufferPtr++;
    }

    // Add 6 indices (2 triangles)
    m_indexBufferPtr[0] = vertexOffset + 0;
    m_indexBufferPtr[1] = vertexOffset + 1;
    m_indexBufferPtr[2] = vertexOffset + 2;
    m_indexBufferPtr[3] = vertexOffset + 2;
    m_indexBufferPtr[4] = vertexOffset + 3;
    m_indexBufferPtr[5] = vertexOffset + 0;
    m_indexBufferPtr += 6;

    m_indexCount += 6;
    m_stats.quadCount++;
}

// ===== Line Drawing =====

void Batch2D::drawLine(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness) {
    drawLine(glm::vec3(p0, 0.0f), glm::vec3(p1, 0.0f), color, thickness);
}

void Batch2D::drawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness) {
    glm::vec3 direction = p1 - p0;
    float length = glm::length(glm::vec2(direction));
    if (length < 0.0001f) return;

    glm::vec3 normalized = direction / length;
    glm::vec3 perpendicular(-normalized.y, normalized.x, 0.0f);
    float halfThickness = thickness * 0.5f;

    // Four corners of the line quad
    glm::vec3 v0 = p0 - perpendicular * halfThickness;
    glm::vec3 v1 = p1 - perpendicular * halfThickness;
    glm::vec3 v2 = p1 + perpendicular * halfThickness;
    glm::vec3 v3 = p0 + perpendicular * halfThickness;

    if (m_indexCount >= Batch2DConstants::MaxIndices) {
        nextBatch();
    }

    glm::vec2 defaultUV(0.5f, 0.5f);
    Uint32 vertexOffset = static_cast<Uint32>(m_vertexBufferPtr - m_vertexBufferBase);

    // Add vertices
    m_vertexBufferPtr->position = v0;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    m_vertexBufferPtr->position = v1;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    m_vertexBufferPtr->position = v2;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    m_vertexBufferPtr->position = v3;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    // Add indices
    m_indexBufferPtr[0] = vertexOffset + 0;
    m_indexBufferPtr[1] = vertexOffset + 1;
    m_indexBufferPtr[2] = vertexOffset + 2;
    m_indexBufferPtr[3] = vertexOffset + 2;
    m_indexBufferPtr[4] = vertexOffset + 3;
    m_indexBufferPtr[5] = vertexOffset + 0;
    m_indexBufferPtr += 6;

    m_indexCount += 6;
    m_stats.lineCount++;
}

// ===== Circle Drawing =====

void Batch2D::drawCircle(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    drawCircle(glm::vec3(center, 0.0f), radius, color, segments);
}

void Batch2D::drawCircle(const glm::vec3& center, float radius, const glm::vec4& color, int segments) {
    float angleStep = 2.0f * glm::pi<float>() / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        float angle0 = angleStep * i;
        float angle1 = angleStep * (i + 1);

        glm::vec3 p0 = center + glm::vec3(std::cos(angle0) * radius, std::sin(angle0) * radius, 0.0f);
        glm::vec3 p1 = center + glm::vec3(std::cos(angle1) * radius, std::sin(angle1) * radius, 0.0f);

        drawLine(p0, p1, color, 1.0f);
    }
    m_stats.circleCount++;
}

void Batch2D::drawCircleFilled(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    drawCircleFilled(glm::vec3(center, 0.0f), radius, color, segments);
}

void Batch2D::drawCircleFilled(const glm::vec3& center, float radius, const glm::vec4& color, int segments) {
    float angleStep = 2.0f * glm::pi<float>() / static_cast<float>(segments);

    for (int i = 0; i < segments; ++i) {
        float angle0 = angleStep * i;
        float angle1 = angleStep * (i + 1);

        glm::vec3 p0 = center;
        glm::vec3 p1 = center + glm::vec3(std::cos(angle0) * radius, std::sin(angle0) * radius, 0.0f);
        glm::vec3 p2 = center + glm::vec3(std::cos(angle1) * radius, std::sin(angle1) * radius, 0.0f);

        drawTriangleFilled(p0, p1, p2, color);
    }
    m_stats.circleCount++;
}

// ===== Triangle Drawing =====

void Batch2D::drawTriangle(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    drawTriangle(glm::vec3(p0, 0.0f), glm::vec3(p1, 0.0f), glm::vec3(p2, 0.0f), color);
}

void Batch2D::drawTriangle(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec4& color) {
    drawLine(p0, p1, color, 1.0f);
    drawLine(p1, p2, color, 1.0f);
    drawLine(p2, p0, color, 1.0f);
}

void Batch2D::drawTriangleFilled(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    drawTriangleFilled(glm::vec3(p0, 0.0f), glm::vec3(p1, 0.0f), glm::vec3(p2, 0.0f), color);
}

void Batch2D::drawTriangleFilled(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec4& color) {
    if (m_indexCount >= Batch2DConstants::MaxIndices) {
        nextBatch();
    }

    glm::vec2 defaultUV(0.5f, 0.5f);
    Uint32 vertexOffset = static_cast<Uint32>(m_vertexBufferPtr - m_vertexBufferBase);

    // Vertex 0
    m_vertexBufferPtr->position = p0;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    // Vertex 1
    m_vertexBufferPtr->position = p1;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    // Vertex 2
    m_vertexBufferPtr->position = p2;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    // Degenerate 4th vertex (same as p2)
    m_vertexBufferPtr->position = p2;
    m_vertexBufferPtr->color = color;
    m_vertexBufferPtr->uv = defaultUV;
    m_vertexBufferPtr->texIndex = 0.0f;
    m_vertexBufferPtr->entityID = -1.0f;
    m_vertexBufferPtr++;

    // Add indices (first triangle is the actual triangle, second is degenerate)
    m_indexBufferPtr[0] = vertexOffset + 0;
    m_indexBufferPtr[1] = vertexOffset + 1;
    m_indexBufferPtr[2] = vertexOffset + 2;
    m_indexBufferPtr[3] = vertexOffset + 2;
    m_indexBufferPtr[4] = vertexOffset + 3;
    m_indexBufferPtr[5] = vertexOffset + 0;
    m_indexBufferPtr += 6;

    m_indexCount += 6;
    m_stats.triangleCount++;
}

// ===== Rectangle Drawing =====

void Batch2D::drawRect(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness) {
    drawRect(glm::vec3(position, 0.0f), size, color, thickness);
}

void Batch2D::drawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, float thickness) {
    glm::vec3 topLeft = position;
    glm::vec3 topRight = position + glm::vec3(size.x, 0.0f, 0.0f);
    glm::vec3 bottomRight = position + glm::vec3(size.x, size.y, 0.0f);
    glm::vec3 bottomLeft = position + glm::vec3(0.0f, size.y, 0.0f);

    drawLine(topLeft, topRight, color, thickness);
    drawLine(topRight, bottomRight, color, thickness);
    drawLine(bottomRight, bottomLeft, color, thickness);
    drawLine(bottomLeft, topLeft, color, thickness);
}

// ===== Polygon Drawing =====

void Batch2D::drawPolygon(const std::vector<glm::vec2>& vertices, const glm::vec4& color, float thickness) {
    std::vector<glm::vec3> vertices3D;
    vertices3D.reserve(vertices.size());
    for (const auto& v : vertices) {
        vertices3D.push_back(glm::vec3(v, 0.0f));
    }
    drawPolygon(vertices3D, color, thickness);
}

void Batch2D::drawPolygon(const std::vector<glm::vec3>& vertices, const glm::vec4& color, float thickness) {
    if (vertices.size() < 3) return;

    for (size_t i = 0; i < vertices.size(); ++i) {
        size_t next = (i + 1) % vertices.size();
        drawLine(vertices[i], vertices[next], color, thickness);
    }
}

void Batch2D::drawPolygonFilled(const std::vector<glm::vec2>& vertices, const glm::vec4& color) {
    std::vector<glm::vec3> vertices3D;
    vertices3D.reserve(vertices.size());
    for (const auto& v : vertices) {
        vertices3D.push_back(glm::vec3(v, 0.0f));
    }
    drawPolygonFilled(vertices3D, color);
}

void Batch2D::drawPolygonFilled(const std::vector<glm::vec3>& vertices, const glm::vec4& color) {
    if (vertices.size() < 3) return;

    // Simple triangle fan for convex polygons
    for (size_t i = 1; i < vertices.size() - 1; ++i) {
        drawTriangleFilled(vertices[0], vertices[i], vertices[i + 1], color);
    }
}

// ===== Raw Geometry =====

void Batch2D::drawGeometry(
    const std::vector<Batch2DVertex>& vertices,
    const std::vector<Uint32>& indices,
    TextureHandle texture,
    const glm::mat4& transform
) {
    if (vertices.empty() || indices.empty()) return;

    // Check if we have enough space
    Uint32 currentVertexCount = static_cast<Uint32>(m_vertexBufferPtr - m_vertexBufferBase);
    if (m_indexCount + indices.size() >= Batch2DConstants::MaxIndices ||
        currentVertexCount + vertices.size() >= Batch2DConstants::MaxVertices) {
        nextBatch();
    }

    float textureIndex = findOrAddTexture(texture);
    Uint32 vertexOffset = static_cast<Uint32>(m_vertexBufferPtr - m_vertexBufferBase);

    // Copy vertices with transform
    for (const auto& vertex : vertices) {
        m_vertexBufferPtr->position = transform * glm::vec4(vertex.position, 1.0f);
        m_vertexBufferPtr->color = vertex.color;
        m_vertexBufferPtr->uv = vertex.uv;
        m_vertexBufferPtr->texIndex = textureIndex;
        m_vertexBufferPtr->entityID = vertex.entityID;
        m_vertexBufferPtr++;
    }

    // Copy indices with offset
    for (Uint32 index : indices) {
        *m_indexBufferPtr = vertexOffset + index;
        m_indexBufferPtr++;
    }

    m_indexCount += static_cast<Uint32>(indices.size());
    m_stats.quadCount += static_cast<Uint32>(indices.size()) / 6;
}

// ===== Blend Mode =====

void Batch2D::setBlendMode(BlendMode mode) {
    if (m_currentBlendMode != mode) {
        // Flush current batch before changing blend mode
        if (m_indexCount > 0) {
            flush();
            startBatch();
        }
        m_currentBlendMode = mode;
    }
}

BlendMode Batch2D::getBlendMode() const {
    return m_currentBlendMode;
}

// ===== Statistics =====

Batch2DStats Batch2D::getStats() const {
    return m_stats;
}

void Batch2D::resetStats() {
    std::memset(&m_stats, 0, sizeof(Batch2DStats));
}
