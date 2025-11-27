#pragma once
#include <SDL3/SDL_stdinc.h>
#include <glm/glm.hpp>
#include <vector>
#include "rhi.hpp"
#include "graphics.hpp"

// ============================================================================
// Renderer Layer Data Structures
//
// This layer sits between the Application (Scene) and RHI.
// It manages rendering resources and implements high-level rendering logic.
// ============================================================================

// ============================================================================
// Renderer Resource IDs
// ============================================================================

using MeshId = Uint32;
using MaterialId = Uint32;
using TextureId = Uint32;

constexpr Uint32 INVALID_MESH_ID = UINT32_MAX;
constexpr Uint32 INVALID_MATERIAL_ID = UINT32_MAX;
constexpr Uint32 INVALID_TEXTURE_ID = UINT32_MAX;

// ============================================================================
// Drawable - Represents a single renderable object
// ============================================================================

struct Drawable {
    glm::mat4 transform;
    MeshId mesh = INVALID_MESH_ID;
    MaterialId material = INVALID_MATERIAL_ID;
    glm::vec3 aabbMin;
    glm::vec3 aabbMax;
    glm::vec4 color = glm::vec4(1.0f);
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
};

// ============================================================================
// RenderMesh - Renderer's internal mesh representation
// ============================================================================

struct RenderMesh {
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;
    Uint32 indexCount = 0;
    Uint32 vertexCount = 0;
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
};

// ============================================================================
// RenderTexture - Renderer's internal texture representation
// ============================================================================

struct RenderTexture {
    TextureHandle handle;
    SamplerHandle sampler;
    Uint32 width = 0;
    Uint32 height = 0;
    PixelFormat format = PixelFormat::RGBA8_UNORM;
};

// ============================================================================
// Material Flags
// ============================================================================

enum MaterialFlags : Uint32 {
    HAS_ALBEDO_TEXTURE = 1 << 0,
    HAS_NORMAL_MAP = 1 << 1,
    HAS_METALLIC_MAP = 1 << 2,
    HAS_ROUGHNESS_MAP = 1 << 3,
    HAS_OCCLUSION_MAP = 1 << 4,
    HAS_EMISSIVE_MAP = 1 << 5,
    ALPHA_BLEND = 1 << 6,
    DOUBLE_SIDED = 1 << 7,
};

// ============================================================================
// RenderMaterial - Renderer's internal material representation
// ============================================================================

struct RenderMaterial {
    // Material parameters (CPU-side cache)
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float normalScale = 1.0f;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    float emissiveStrength = 1.0f;
    float subsurface = 0.0f;
    float specular = 0.5f;
    float specularTint = 0.0f;
    float anisotropic = 0.0f;
    float sheen = 0.0f;
    float sheenTint = 0.5f;
    float clearcoat = 0.0f;
    float clearcoatGloss = 1.0f;

    // Texture references (IDs, not handles)
    TextureId albedoTexture = INVALID_TEXTURE_ID;
    TextureId normalTexture = INVALID_TEXTURE_ID;
    TextureId metallicTexture = INVALID_TEXTURE_ID;
    TextureId roughnessTexture = INVALID_TEXTURE_ID;
    TextureId occlusionTexture = INVALID_TEXTURE_ID;
    TextureId emissiveTexture = INVALID_TEXTURE_ID;

    // GPU resources
    BufferHandle parameterBuffer;
    DescriptorSetHandle descriptorSet;
    PipelineHandle pipeline;

    // Metadata
    MaterialFlags flags = static_cast<MaterialFlags>(0);
    AlphaMode alphaMode = AlphaMode::OPAQUE;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

// ============================================================================
// Camera Data for Renderer
// ============================================================================

struct CameraRenderData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewProj;
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::vec3 position;
    float nearPlane;
    float farPlane;
    glm::vec4 frustumPlanes[6];
};

// ============================================================================
// Light Data for Renderer
// ============================================================================

struct DirectionalLightData {
    glm::vec3 direction;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity;
    float _pad3[3];
};

struct PointLightData {
    glm::vec3 position;
    float _pad1;
    glm::vec3 color;
    float _pad2;
    float intensity;
    float radius;
    float _pad3[2];
};

// ============================================================================
// Frustum for Culling
// ============================================================================

struct Frustum {
    glm::vec4 planes[6]; // left, right, bottom, top, near, far

    bool isBoxVisible(const glm::vec3& aabbMin, const glm::vec3& aabbMax) const {
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& plane = planes[i];
            glm::vec3 positive = aabbMin;

            if (plane.x >= 0) positive.x = aabbMax.x;
            if (plane.y >= 0) positive.y = aabbMax.y;
            if (plane.z >= 0) positive.z = aabbMax.z;

            if (glm::dot(glm::vec3(plane), positive) + plane.w < 0) {
                return false;
            }
        }
        return true;
    }
};

// ============================================================================
// Material Data Input (from Application Layer)
// ============================================================================

struct MaterialDataInput {
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float normalScale = 1.0f;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    float emissiveStrength = 1.0f;
    float subsurface = 0.0f;
    float specular = 0.5f;
    float specularTint = 0.0f;
    float anisotropic = 0.0f;
    float sheen = 0.0f;
    float sheenTint = 0.5f;
    float clearcoat = 0.0f;
    float clearcoatGloss = 1.0f;

    // Texture data (from Application's Image objects)
    std::shared_ptr<Image> albedoMap;
    std::shared_ptr<Image> normalMap;
    std::shared_ptr<Image> metallicMap;
    std::shared_ptr<Image> roughnessMap;
    std::shared_ptr<Image> occlusionMap;
    std::shared_ptr<Image> emissiveMap;

    AlphaMode alphaMode = AlphaMode::OPAQUE;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};
