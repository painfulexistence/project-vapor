#pragma once
#include "graphics.hpp"
#include <functional>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>

// Forward declarations
class Renderer_Metal;

// Constants for 2D batch rendering
namespace Batch2DConstants {
    static constexpr Uint32 MaxQuads = 20000;
    static constexpr Uint32 MaxVertices = MaxQuads * 4;
    static constexpr Uint32 MaxIndices = MaxQuads * 6;
    static constexpr Uint32 MaxTextureSlots = 16;
}

// 2D Batch Renderer interface
// Manages batched 2D rendering with support for quads, lines, circles, and other shapes
class Batch2D {
public:
    Batch2D();
    ~Batch2D();

    // Initialize the batch renderer (called by Renderer_Metal)
    void init();

    // Shutdown and release resources
    void shutdown();

    // Begin a new scene with the given view-projection matrix
    void beginScene(const glm::mat4& viewProj, BlendMode blendMode = BlendMode::Alpha);

    // End the current scene and flush remaining batches
    void endScene();

    // Flush the current batch (called automatically when batch is full)
    void flush();

    // ===== Quad Drawing =====

    // Draw a colored quad
    void drawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
    void drawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);

    // Draw a textured quad
    void drawQuad(const glm::vec2& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f));
    void drawQuad(const glm::vec3& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f));

    // Draw a rotated quad
    void drawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color);
    void drawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color);
    void drawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f));
    void drawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f));

    // Draw a quad with a transform matrix
    void drawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
    void drawQuad(const glm::mat4& transform, TextureHandle texture, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);
    void drawQuad(const glm::mat4& transform, TextureHandle texture, const glm::vec2* texCoords, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);

    // ===== Line Drawing =====

    void drawLine(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness = 1.0f);
    void drawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness = 1.0f);

    // ===== Shape Drawing =====

    // Circle (outline)
    void drawCircle(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32);
    void drawCircle(const glm::vec3& center, float radius, const glm::vec4& color, int segments = 32);

    // Circle (filled)
    void drawCircleFilled(const glm::vec2& center, float radius, const glm::vec4& color, int segments = 32);
    void drawCircleFilled(const glm::vec3& center, float radius, const glm::vec4& color, int segments = 32);

    // Triangle (outline)
    void drawTriangle(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color);
    void drawTriangle(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec4& color);

    // Triangle (filled)
    void drawTriangleFilled(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color);
    void drawTriangleFilled(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec4& color);

    // Rectangle (outline)
    void drawRect(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness = 1.0f);
    void drawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, float thickness = 1.0f);

    // Polygon (outline)
    void drawPolygon(const std::vector<glm::vec2>& vertices, const glm::vec4& color, float thickness = 1.0f);
    void drawPolygon(const std::vector<glm::vec3>& vertices, const glm::vec4& color, float thickness = 1.0f);

    // Polygon (filled) - assumes convex polygon
    void drawPolygonFilled(const std::vector<glm::vec2>& vertices, const glm::vec4& color);
    void drawPolygonFilled(const std::vector<glm::vec3>& vertices, const glm::vec4& color);

    // ===== Raw Geometry =====

    // Draw arbitrary indexed geometry
    void drawGeometry(
        const std::vector<Batch2DVertex>& vertices,
        const std::vector<Uint32>& indices,
        TextureHandle texture,
        const glm::mat4& transform = glm::mat4(1.0f)
    );

    // ===== Blend Mode =====

    void setBlendMode(BlendMode mode);
    BlendMode getBlendMode() const;

    // ===== Statistics =====

    Batch2DStats getStats() const;
    void resetStats();

    // ===== Accessors for Renderer =====

    const Batch2DVertex* getVertexData() const { return m_vertexBufferBase; }
    const Uint32* getIndexData() const { return m_indexBufferBase; }
    Uint32 getVertexCount() const { return static_cast<Uint32>(m_vertexBufferPtr - m_vertexBufferBase); }
    Uint32 getIndexCount() const { return m_indexCount; }
    const glm::mat4& getProjectionMatrix() const { return m_projectionMatrix; }
    BlendMode getCurrentBlendMode() const { return m_currentBlendMode; }

    // Get texture handles for binding
    const std::vector<TextureHandle>& getTextureSlots() const { return m_textureSlots; }
    Uint32 getTextureSlotCount() const { return m_textureSlotIndex; }

    // White texture handle (set by renderer)
    void setWhiteTexture(TextureHandle handle) { m_whiteTexture = handle; }
    TextureHandle getWhiteTexture() const { return m_whiteTexture; }

    // Check if there's data to flush
    bool hasData() const { return m_indexCount > 0; }

    // Called after flush to reset batch state
    void resetBatch();

private:
    void startBatch();
    void nextBatch();
    float findOrAddTexture(TextureHandle texture);

    // Vertex/Index data (CPU side)
    Batch2DVertex* m_vertexBufferBase = nullptr;
    Batch2DVertex* m_vertexBufferPtr = nullptr;
    Uint32* m_indexBufferBase = nullptr;
    Uint32* m_indexBufferPtr = nullptr;
    Uint32 m_indexCount = 0;

    // Texture slots
    std::vector<TextureHandle> m_textureSlots;
    Uint32 m_textureSlotIndex = 1; // 0 = white texture

    // White texture (1x1 white pixel)
    TextureHandle m_whiteTexture;

    // Current state
    glm::mat4 m_projectionMatrix = glm::mat4(1.0f);
    BlendMode m_currentBlendMode = BlendMode::Alpha;

    // Pre-computed quad vertex positions and UVs
    glm::vec4 m_quadVertexPositions[4];
    glm::vec2 m_quadTexCoords[4];

    // Statistics
    Batch2DStats m_stats;

    // Flush callback (set by Batch2DPass)
    friend class Batch2DPass;
    std::function<void()> m_flushCallback;
};
