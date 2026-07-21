#pragma once
// GPU-layout structs: everything that is uploaded verbatim to a GPU buffer.
// All structs are alignas(16) to satisfy Metal/Vulkan UBO alignment rules.
#include "graphics_handles.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace Vapor {

// TODO: static_assert(sizeof) + offsetof on the load-bearing fields to lock
// these GPU upload layouts against silent drift.

enum class PrimitiveMode {
    POINTS,
    LINES,
    LINE_STRIP,
    TRIANGLES,
    TRIANGLE_STRIP,
};

struct alignas(16) MaterialData {
    glm::vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    glm::vec3 emissiveFactor;
    // MASK-mode alpha cutoff in emissiveFactor's alignment tail (was _pad1, so
    // the layout is unchanged); the Metal MaterialData twin exposes the same
    // slot via packed_float3 + alphaCutoff. 0 = no cutoff (OPAQUE/BLEND).
    float alphaCutoff;
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float prototypeUVMode;
    float uvScale;
    float iblEnabled; // 1.0 = use IBL, 0.0 = ambient approximation
    // Keep byte-identical with the shader twins (3d_common.metal / RHIMain.frag
    // / PrePass.frag / ShadowDepth.frag): all are stride 112. A missing field
    // here silently shifts every materials[i>0] read.
    float transmission;
    // Surface shader model the Main pass dispatches on: 0 = Standard PBR,
    // 1 = Terrain (splat), 2 = Grass. Occupies the alignment tail after
    // transmission, so the std430 stride stays 112 — shader twins that don't
    // read it are unaffected.
    float shaderModel;
};
static_assert(sizeof(MaterialData) == 112, "GPU material upload layout: stride must stay 112 (shader twins assume it)");

struct alignas(16) DirectionalLight {
    glm::vec3 direction;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity;
};

struct alignas(16) PointLight {
    glm::vec3 position;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity = 1.0f;
    float radius = 0.5f;
};

// Rectangular area light driven by an optional video texture.
// right and up must be orthonormal; halfWidth/halfHeight are in world units.
struct alignas(16) RectLight {
    glm::vec3 position;
    float halfWidth;
    glm::vec3 right;           // normalized right axis
    float halfHeight;
    glm::vec3 up;              // normalized up axis
    float intensity;
    glm::vec3 color;
    uint32_t useVideoTexture;  // 0 = solid color, 1 = sample rectLightVideo texture
};

struct alignas(16) FrameData {
    Uint32 frameNumber;
    float time;
    float deltaTime;
};

struct alignas(16) CameraData {
    glm::mat4 proj;
    glm::mat4 view;
    glm::mat4 invProj;
    glm::mat4 invView;
    float near;
    float far;
    glm::vec3 position;
    float _pad1;
    glm::vec4 frustumPlanes[6];
};

struct alignas(16) InstanceData {
    glm::mat4 model;
    glm::vec4 color;
    Uint32 vertexOffset;
    Uint32 indexOffset;
    Uint32 vertexCount;
    Uint32 indexCount;
    Uint32 materialID;
    PrimitiveMode primitiveMode;
    // Always-populated merged-buffer offsets for RT hit shading (the RT kernels
    // fetch UV + normals at the hit regardless of draw mode). Occupy the old
    // _pad1[2] slot, so the stride is unchanged.
    Uint32 rtVertexOffset;
    Uint32 rtIndexOffset;
    glm::vec3 AABBMin;
    float _pad2;
    glm::vec3 AABBMax;
    float _pad3;
    glm::vec4 boundingSphere;
};

// Indirect draw arguments produced by the GPU cull compute pass.
// The binary layout matches VkDrawIndexedIndirectCommand and
// MTLDrawIndexedPrimitivesIndirectArguments exactly (5x 4-byte fields, 20 bytes,
// no padding), so a single compute-written buffer feeds both backends.
// NOTE: intentionally NOT alignas(16) — the tight 20-byte stride is what the
// indirect-draw APIs expect (Vulkan stride arg / Metal per-command offset).
struct DrawCommand {
    Uint32 indexCount;
    Uint32 instanceCount; // 0 = culled (GPU no-op), 1 = visible
    Uint32 firstIndex;    // base index into the merged index buffer
    Sint32 vertexOffset;  // base vertex into the merged vertex buffer
    Uint32 firstInstance; // = instance index, so the vertex shader can look up InstanceData
};
static_assert(sizeof(DrawCommand) == 20, "DrawCommand must match the GPU indirect-args layout");

// One grass blade instance (Grass pass): world base position + per-blade
// randomization, uploaded verbatim into the grass instance pool. Cell builders
// (TerrainWorld::buildGrassCell) produce these on worker threads.
struct alignas(16) GrassBladeGpu {
    glm::vec4 positionAndHeight;  // xyz = world base position, w = blade height (m)
    glm::vec4 params;             // x = sway phase (rad), y = facing angle (rad),
                                  // z = tint jitter 0..1, w = half width (m)
};
static_assert(sizeof(GrassBladeGpu) == 32, "grass instance pool layout: 32 bytes per blade");

// Per-frame grass uniforms (renderer-filled; the shader twins mirror this).
struct alignas(16) GrassParamsGpu {
    glm::mat4 viewProj;
    glm::vec4 cameraPosTime;  // xyz = camera position, w = time (s)
    glm::vec4 wind;           // xy = direction, z = strength (m at tip), w = speed
    glm::vec4 rootColor;      // rgb = root color, w = fade start distance (m)
    glm::vec4 tipColor;       // rgb = tip color, w = fade end distance (m)
    glm::vec4 sun;            // xyz = direction TOWARD the sun, w = intensity
    glm::vec4 sunColor;       // rgb = sun color, w = unused
};

// Mesh-shader terrain parameters (Terrain.task / Terrain.mesh set0 b3).
struct alignas(16) TerrainMeshParamsGpu {
    glm::vec4 worldParams;  // x worldSize, y tileSize, z heightScale, w noiseFrequency
    glm::ivec4 gridParams;  // x tilesAxis, y noiseOctaves, z materialId, w seed
    glm::ivec4 lodParams;   // xyz lod ring radii (tiles), w unused
};

struct alignas(16) Cluster {
    glm::vec4 min;
    glm::vec4 max;
    Uint32 lightCount;
    Uint32 lightIndices[256];
};

struct alignas(16) LightCullData {
    glm::vec2 screenSize;
    glm::vec2 _pad1;
    glm::uvec3 gridSize;
    Uint32 lightCount;
};

struct alignas(16) IBLCaptureData {
    glm::mat4 viewProj;
    Uint32 faceIndex;
    float roughness;
    float _pad[2];
};

} // namespace Vapor
