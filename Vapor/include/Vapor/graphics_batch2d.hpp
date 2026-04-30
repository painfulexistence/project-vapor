#pragma once
#include "graphics_handles.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Blend modes for 2D batch rendering
enum class BlendMode {
    None,
    Alpha,
    Additive,
    Multiply,
    Screen,
    Premultiplied
};

struct alignas(16) Batch2DVertex {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec4 color    = glm::vec4(1.0f);
    glm::vec2 uv       = glm::vec2(0.0f);
    float texIndex     = 0.0f;
    float entityID     = -1.0f;
    float _pad;
};

struct alignas(16) Batch2DUniforms {
    glm::mat4 projectionMatrix;
};

struct Batch2DStats {
    Uint32 drawCalls    = 0;
    Uint32 quadCount    = 0;
    Uint32 lineCount    = 0;
    Uint32 triangleCount = 0;
    Uint32 circleCount  = 0;
    Uint32 vertexCount  = 0;
    Uint32 indexCount   = 0;
};
