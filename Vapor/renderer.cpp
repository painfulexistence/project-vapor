#include "renderer.hpp"
#include <algorithm>
#include <cstring>

// ============================================================================
// Initialization
// ============================================================================

void Renderer::initialize(RHI* rhiPtr) {
    rhi = rhiPtr;

    // Create uniform buffers
    BufferDesc cameraBufferDesc;
    cameraBufferDesc.size = sizeof(CameraRenderData);
    cameraBufferDesc.usage = BufferUsage::Uniform;
    cameraBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    cameraUniformBuffer = rhi->createBuffer(cameraBufferDesc);

    BufferDesc materialBufferDesc;
    materialBufferDesc.size = sizeof(MaterialData) * MAX_INSTANCES;
    materialBufferDesc.usage = BufferUsage::Uniform;
    materialBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    materialUniformBuffer = rhi->createBuffer(materialBufferDesc);

    BufferDesc dirLightBufferDesc;
    dirLightBufferDesc.size = sizeof(DirectionalLightData) * maxDirectionalLights;
    dirLightBufferDesc.usage = BufferUsage::Uniform;
    dirLightBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    directionalLightBuffer = rhi->createBuffer(dirLightBufferDesc);

    BufferDesc pointLightBufferDesc;
    pointLightBufferDesc.size = sizeof(PointLightData) * maxPointLights;
    pointLightBufferDesc.usage = BufferUsage::Uniform;
    pointLightBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    pointLightBuffer = rhi->createBuffer(pointLightBufferDesc);

    // Create default resources
    createDefaultResources();

    // Reserve space for per-frame data
    frameDrawables.reserve(MAX_INSTANCES);
    visibleDrawables.reserve(MAX_INSTANCES);
    directionalLights.reserve(maxDirectionalLights);
    pointLights.reserve(maxPointLights);
}

void Renderer::shutdown() {
    if (rhi) {
        // Destroy buffers
        if (cameraUniformBuffer.isValid()) {
            rhi->destroyBuffer(cameraUniformBuffer);
        }
        if (materialUniformBuffer.isValid()) {
            rhi->destroyBuffer(materialUniformBuffer);
        }
        if (directionalLightBuffer.isValid()) {
            rhi->destroyBuffer(directionalLightBuffer);
        }
        if (pointLightBuffer.isValid()) {
            rhi->destroyBuffer(pointLightBuffer);
        }

        // Destroy meshes
        for (auto& mesh : meshes) {
            if (mesh.vertexBuffer.isValid()) {
                rhi->destroyBuffer(mesh.vertexBuffer);
            }
            if (mesh.indexBuffer.isValid()) {
                rhi->destroyBuffer(mesh.indexBuffer);
            }
        }

        // Destroy materials
        for (auto& material : materials) {
            if (material.parameterBuffer.isValid()) {
                rhi->destroyBuffer(material.parameterBuffer);
            }
            if (material.pipeline.isValid()) {
                rhi->destroyPipeline(material.pipeline);
            }
        }

        // Destroy textures
        for (auto& texture : textures) {
            if (texture.handle.isValid()) {
                rhi->destroyTexture(texture.handle);
            }
            if (texture.sampler.isValid()) {
                rhi->destroySampler(texture.sampler);
            }
        }

        // Destroy shaders
        if (vertexShader.isValid()) {
            rhi->destroyShader(vertexShader);
        }
        if (fragmentShader.isValid()) {
            rhi->destroyShader(fragmentShader);
        }

        // Destroy sampler
        if (defaultSampler.isValid()) {
            rhi->destroySampler(defaultSampler);
        }

        // Destroy pipeline
        if (mainPipeline.isValid()) {
            rhi->destroyPipeline(mainPipeline);
        }
    }

    meshes.clear();
    materials.clear();
    textures.clear();
    textureCache.clear();
}

// ============================================================================
// Resource Registration
// ============================================================================

MeshId Renderer::registerMesh(const std::vector<VertexData>& vertices,
                                    const std::vector<Uint32>& indices) {
    RenderMesh mesh;

    // Create vertex buffer
    BufferDesc vbDesc;
    vbDesc.size = vertices.size() * sizeof(VertexData);
    vbDesc.usage = BufferUsage::Vertex;
    vbDesc.memoryUsage = MemoryUsage::GPU;
    mesh.vertexBuffer = rhi->createBuffer(vbDesc);
    rhi->updateBuffer(mesh.vertexBuffer, vertices.data(), 0, vbDesc.size);

    // Create index buffer
    BufferDesc ibDesc;
    ibDesc.size = indices.size() * sizeof(Uint32);
    ibDesc.usage = BufferUsage::Index;
    ibDesc.memoryUsage = MemoryUsage::GPU;
    mesh.indexBuffer = rhi->createBuffer(ibDesc);
    rhi->updateBuffer(mesh.indexBuffer, indices.data(), 0, ibDesc.size);

    mesh.indexCount = static_cast<Uint32>(indices.size());
    mesh.vertexCount = static_cast<Uint32>(vertices.size());

    MeshId id = static_cast<MeshId>(meshes.size());
    meshes.push_back(mesh);
    return id;
}

MaterialId Renderer::registerMaterial(const MaterialDataInput& materialData) {
    RenderMaterial material;

    // Copy material parameters
    material.baseColorFactor = materialData.baseColorFactor;
    material.normalScale = materialData.normalScale;
    material.metallicFactor = materialData.metallicFactor;
    material.roughnessFactor = materialData.roughnessFactor;
    material.occlusionStrength = materialData.occlusionStrength;
    material.emissiveFactor = materialData.emissiveFactor;
    material.emissiveStrength = materialData.emissiveStrength;
    material.subsurface = materialData.subsurface;
    material.specular = materialData.specular;
    material.specularTint = materialData.specularTint;
    material.anisotropic = materialData.anisotropic;
    material.sheen = materialData.sheen;
    material.sheenTint = materialData.sheenTint;
    material.clearcoat = materialData.clearcoat;
    material.clearcoatGloss = materialData.clearcoatGloss;
    material.alphaMode = materialData.alphaMode;
    material.alphaCutoff = materialData.alphaCutoff;
    material.doubleSided = materialData.doubleSided;

    // Register textures
    Uint32 flags = 0;
    if (materialData.albedoMap) {
        material.albedoTexture = getOrCreateTexture(materialData.albedoMap);
        flags |= HAS_ALBEDO_TEXTURE;
    } else {
        material.albedoTexture = defaultWhiteTexture;
    }

    if (materialData.normalMap) {
        material.normalTexture = getOrCreateTexture(materialData.normalMap);
        flags |= HAS_NORMAL_MAP;
    } else {
        material.normalTexture = defaultNormalTexture;
    }

    if (materialData.metallicMap) {
        material.metallicTexture = getOrCreateTexture(materialData.metallicMap);
        flags |= HAS_METALLIC_MAP;
    } else {
        material.metallicTexture = defaultWhiteTexture;
    }

    if (materialData.roughnessMap) {
        material.roughnessTexture = getOrCreateTexture(materialData.roughnessMap);
        flags |= HAS_ROUGHNESS_MAP;
    } else {
        material.roughnessTexture = defaultWhiteTexture;
    }

    if (materialData.occlusionMap) {
        material.occlusionTexture = getOrCreateTexture(materialData.occlusionMap);
        flags |= HAS_OCCLUSION_MAP;
    } else {
        material.occlusionTexture = defaultWhiteTexture;
    }

    if (materialData.emissiveMap) {
        material.emissiveTexture = getOrCreateTexture(materialData.emissiveMap);
        flags |= HAS_EMISSIVE_MAP;
    } else {
        material.emissiveTexture = defaultBlackTexture;
    }

    if (materialData.alphaMode == AlphaMode::BLEND) {
        flags |= ALPHA_BLEND;
    }

    if (materialData.doubleSided) {
        flags |= DOUBLE_SIDED;
    }

    material.flags = static_cast<MaterialFlags>(flags);

    // Create parameter buffer
    BufferDesc paramBufferDesc;
    paramBufferDesc.size = sizeof(MaterialData);
    paramBufferDesc.usage = BufferUsage::Uniform;
    paramBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    material.parameterBuffer = rhi->createBuffer(paramBufferDesc);

    // Upload initial parameters
    MaterialData params;
    params.baseColorFactor = material.baseColorFactor;
    params.normalScale = material.normalScale;
    params.metallicFactor = material.metallicFactor;
    params.roughnessFactor = material.roughnessFactor;
    params.occlusionStrength = material.occlusionStrength;
    params.emissiveFactor = material.emissiveFactor;
    params.emissiveStrength = material.emissiveStrength;
    params.subsurface = material.subsurface;
    params.specular = material.specular;
    params.specularTint = material.specularTint;
    params.anisotropic = material.anisotropic;
    params.sheen = material.sheen;
    params.sheenTint = material.sheenTint;
    params.clearcoat = material.clearcoat;
    params.clearcoatGloss = material.clearcoatGloss;

    rhi->updateBuffer(material.parameterBuffer, &params, 0, sizeof(MaterialData));

    MaterialId id = static_cast<MaterialId>(materials.size());
    materials.push_back(material);
    return id;
}

TextureId Renderer::registerTexture(const std::shared_ptr<Image>& image) {
    return getOrCreateTexture(image);
}

// ============================================================================
// Frame Rendering
// ============================================================================

void Renderer::beginFrame(const CameraRenderData& camera) {
    currentCamera = camera;
    frameDrawables.clear();
    visibleDrawables.clear();
    directionalLights.clear();
    pointLights.clear();
}

void Renderer::submitDrawable(const Drawable& drawable) {
    frameDrawables.push_back(drawable);
}

void Renderer::submitDirectionalLight(const DirectionalLightData& light) {
    if (directionalLights.size() < maxDirectionalLights) {
        directionalLights.push_back(light);
    }
}

void Renderer::submitPointLight(const PointLightData& light) {
    if (pointLights.size() < maxPointLights) {
        pointLights.push_back(light);
    }
}

void Renderer::render() {
    performCulling();
    sortDrawables();
    updateBuffers();
    executeDrawCalls();
}

void Renderer::endFrame() {
    // Nothing to do here for now
}

// ============================================================================
// Internal Rendering Steps
// ============================================================================

void Renderer::performCulling() {
    Frustum frustum = extractFrustum(currentCamera.viewProj);

    for (Uint32 i = 0; i < frameDrawables.size(); ++i) {
        const Drawable& d = frameDrawables[i];
        if (frustum.isBoxVisible(d.aabbMin, d.aabbMax)) {
            visibleDrawables.push_back(i);
        }
    }
}

void Renderer::sortDrawables() {
    // Sort by material to reduce state changes
    std::sort(visibleDrawables.begin(), visibleDrawables.end(),
        [this](Uint32 a, Uint32 b) {
            return frameDrawables[a].material < frameDrawables[b].material;
        });
}

void Renderer::updateBuffers() {
    // Update camera uniform buffer
    rhi->updateBuffer(cameraUniformBuffer, &currentCamera, 0, sizeof(CameraRenderData));

    // Update directional lights
    if (!directionalLights.empty()) {
        rhi->updateBuffer(directionalLightBuffer, directionalLights.data(), 0,
                          directionalLights.size() * sizeof(DirectionalLightData));
    }

    // Update point lights
    if (!pointLights.empty()) {
        rhi->updateBuffer(pointLightBuffer, pointLights.data(), 0,
                          pointLights.size() * sizeof(PointLightData));
    }
}

void Renderer::executeDrawCalls() {
    // For now, this is a placeholder that will be implemented
    // once we have the RHI_Vulkan implementation
    // TODO: Implement actual rendering
}

void Renderer::createDefaultResources() {
    // Create default sampler
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressModeU = AddressMode::Repeat;
    samplerDesc.addressModeV = AddressMode::Repeat;
    samplerDesc.addressModeW = AddressMode::Repeat;
    defaultSampler = rhi->createSampler(samplerDesc);

    // Create default white texture (1x1 white pixel)
    {
        TextureDesc texDesc;
        texDesc.width = 1;
        texDesc.height = 1;
        texDesc.format = PixelFormat::RGBA8_UNORM;
        texDesc.usage = TextureUsage::Sampled;
        TextureHandle texHandle = rhi->createTexture(texDesc);

        Uint32 whitePixel = 0xFFFFFFFF;
        rhi->updateTexture(texHandle, &whitePixel, sizeof(Uint32));

        RenderTexture tex;
        tex.handle = texHandle;
        tex.sampler = defaultSampler;
        tex.width = 1;
        tex.height = 1;
        tex.format = PixelFormat::RGBA8_UNORM;

        defaultWhiteTexture = static_cast<TextureId>(textures.size());
        textures.push_back(tex);
    }

    // Create default normal texture (1x1 normal map pointing up: 0.5, 0.5, 1.0)
    {
        TextureDesc texDesc;
        texDesc.width = 1;
        texDesc.height = 1;
        texDesc.format = PixelFormat::RGBA8_UNORM;
        texDesc.usage = TextureUsage::Sampled;
        TextureHandle texHandle = rhi->createTexture(texDesc);

        Uint32 normalPixel = 0xFFFF8080;  // (0.5, 0.5, 1.0, 1.0) in RGBA8
        rhi->updateTexture(texHandle, &normalPixel, sizeof(Uint32));

        RenderTexture tex;
        tex.handle = texHandle;
        tex.sampler = defaultSampler;
        tex.width = 1;
        tex.height = 1;
        tex.format = PixelFormat::RGBA8_UNORM;

        defaultNormalTexture = static_cast<TextureId>(textures.size());
        textures.push_back(tex);
    }

    // Create default black texture (1x1 black pixel)
    {
        TextureDesc texDesc;
        texDesc.width = 1;
        texDesc.height = 1;
        texDesc.format = PixelFormat::RGBA8_UNORM;
        texDesc.usage = TextureUsage::Sampled;
        TextureHandle texHandle = rhi->createTexture(texDesc);

        Uint32 blackPixel = 0xFF000000;
        rhi->updateTexture(texHandle, &blackPixel, sizeof(Uint32));

        RenderTexture tex;
        tex.handle = texHandle;
        tex.sampler = defaultSampler;
        tex.width = 1;
        tex.height = 1;
        tex.format = PixelFormat::RGBA8_UNORM;

        defaultBlackTexture = static_cast<TextureId>(textures.size());
        textures.push_back(tex);
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

Frustum Renderer::extractFrustum(const glm::mat4& viewProj) {
    Frustum frustum;

    // Left
    frustum.planes[0] = glm::vec4(
        viewProj[0][3] + viewProj[0][0],
        viewProj[1][3] + viewProj[1][0],
        viewProj[2][3] + viewProj[2][0],
        viewProj[3][3] + viewProj[3][0]
    );

    // Right
    frustum.planes[1] = glm::vec4(
        viewProj[0][3] - viewProj[0][0],
        viewProj[1][3] - viewProj[1][0],
        viewProj[2][3] - viewProj[2][0],
        viewProj[3][3] - viewProj[3][0]
    );

    // Bottom
    frustum.planes[2] = glm::vec4(
        viewProj[0][3] + viewProj[0][1],
        viewProj[1][3] + viewProj[1][1],
        viewProj[2][3] + viewProj[2][1],
        viewProj[3][3] + viewProj[3][1]
    );

    // Top
    frustum.planes[3] = glm::vec4(
        viewProj[0][3] - viewProj[0][1],
        viewProj[1][3] - viewProj[1][1],
        viewProj[2][3] - viewProj[2][1],
        viewProj[3][3] - viewProj[3][1]
    );

    // Near
    frustum.planes[4] = glm::vec4(
        viewProj[0][3] + viewProj[0][2],
        viewProj[1][3] + viewProj[1][2],
        viewProj[2][3] + viewProj[2][2],
        viewProj[3][3] + viewProj[3][2]
    );

    // Far
    frustum.planes[5] = glm::vec4(
        viewProj[0][3] - viewProj[0][2],
        viewProj[1][3] - viewProj[1][2],
        viewProj[2][3] - viewProj[2][2],
        viewProj[3][3] - viewProj[3][2]
    );

    // Normalize planes
    for (int i = 0; i < 6; ++i) {
        float length = glm::length(glm::vec3(frustum.planes[i]));
        frustum.planes[i] /= length;
    }

    return frustum;
}

TextureId Renderer::getOrCreateTexture(const std::shared_ptr<Image>& image) {
    if (!image || image->uri.empty()) {
        return defaultWhiteTexture;
    }

    // Check cache
    auto it = textureCache.find(image->uri);
    if (it != textureCache.end()) {
        return it->second;
    }

    // Create new texture
    TextureDesc texDesc;
    texDesc.width = image->width;
    texDesc.height = image->height;
    texDesc.format = PixelFormat::RGBA8_UNORM;
    texDesc.usage = TextureUsage::Sampled;
    TextureHandle texHandle = rhi->createTexture(texDesc);

    if (!image->byteArray.empty()) {
        rhi->updateTexture(texHandle, image->byteArray.data(), image->byteArray.size());
    }

    RenderTexture tex;
    tex.handle = texHandle;
    tex.sampler = defaultSampler;
    tex.width = image->width;
    tex.height = image->height;
    tex.format = PixelFormat::RGBA8_UNORM;

    TextureId id = static_cast<TextureId>(textures.size());
    textures.push_back(tex);
    textureCache[image->uri] = id;

    return id;
}

void Renderer::bindMaterial(MaterialId materialId) {
    if (materialId >= materials.size()) {
        return;
    }

    const RenderMaterial& material = materials[materialId];

    // Bind material parameter buffer
    rhi->setUniformBuffer(1, 0, material.parameterBuffer);

    // Bind textures
    if (material.albedoTexture < textures.size()) {
        const RenderTexture& tex = textures[material.albedoTexture];
        rhi->setTexture(1, 1, tex.handle, tex.sampler);
    }

    if (material.normalTexture < textures.size()) {
        const RenderTexture& tex = textures[material.normalTexture];
        rhi->setTexture(1, 2, tex.handle, tex.sampler);
    }

    if (material.metallicTexture < textures.size()) {
        const RenderTexture& tex = textures[material.metallicTexture];
        rhi->setTexture(1, 3, tex.handle, tex.sampler);
    }

    if (material.roughnessTexture < textures.size()) {
        const RenderTexture& tex = textures[material.roughnessTexture];
        rhi->setTexture(1, 4, tex.handle, tex.sampler);
    }

    if (material.occlusionTexture < textures.size()) {
        const RenderTexture& tex = textures[material.occlusionTexture];
        rhi->setTexture(1, 5, tex.handle, tex.sampler);
    }

    if (material.emissiveTexture < textures.size()) {
        const RenderTexture& tex = textures[material.emissiveTexture];
        rhi->setTexture(1, 6, tex.handle, tex.sampler);
    }
}
