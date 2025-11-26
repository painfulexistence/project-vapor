#include "renderer.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>

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

    // Create frame data buffer
    BufferDesc frameDataBufferDesc;
    frameDataBufferDesc.size = sizeof(FrameData);
    frameDataBufferDesc.usage = BufferUsage::Uniform;
    frameDataBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    frameDataBuffer = rhi->createBuffer(frameDataBufferDesc);

    // Create instance data buffer
    BufferDesc instanceBufferDesc;
    instanceBufferDesc.size = sizeof(InstanceData) * MAX_INSTANCES;
    instanceBufferDesc.usage = BufferUsage::Uniform;
    instanceBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    instanceDataBuffer = rhi->createBuffer(instanceBufferDesc);

    // Create cluster buffer
    Uint32 numClusters = clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;
    BufferDesc clusterBufferDesc;
    clusterBufferDesc.size = sizeof(Cluster) * numClusters;
    clusterBufferDesc.usage = BufferUsage::Storage;
    clusterBufferDesc.memoryUsage = MemoryUsage::GPU;
    clusterBuffer = rhi->createBuffer(clusterBufferDesc);

    // Create default resources
    createDefaultResources();

    // Create render targets
    createRenderTargets();

    // Create pipelines (TODO: implement when we have shader loading)
    // createRenderPipeline();
    // createComputePipelines();

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
        if (frameDataBuffer.isValid()) {
            rhi->destroyBuffer(frameDataBuffer);
        }
        if (instanceDataBuffer.isValid()) {
            rhi->destroyBuffer(instanceDataBuffer);
        }
        if (clusterBuffer.isValid()) {
            rhi->destroyBuffer(clusterBuffer);
        }

        // Destroy render targets
        if (colorRT_MSAA.isValid()) {
            rhi->destroyTexture(colorRT_MSAA);
        }
        if (colorRT.isValid()) {
            rhi->destroyTexture(colorRT);
        }
        if (depthStencilRT_MSAA.isValid()) {
            rhi->destroyTexture(depthStencilRT_MSAA);
        }
        if (depthStencilRT.isValid()) {
            rhi->destroyTexture(depthStencilRT);
        }
        if (normalRT_MSAA.isValid()) {
            rhi->destroyTexture(normalRT_MSAA);
        }
        if (normalRT.isValid()) {
            rhi->destroyTexture(normalRT);
        }
        if (shadowRT.isValid()) {
            rhi->destroyTexture(shadowRT);
        }
        if (aoRT.isValid()) {
            rhi->destroyTexture(aoRT);
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
        if (prePassVertexShader.isValid()) {
            rhi->destroyShader(prePassVertexShader);
        }
        if (prePassFragmentShader.isValid()) {
            rhi->destroyShader(prePassFragmentShader);
        }
        if (postProcessVertexShader.isValid()) {
            rhi->destroyShader(postProcessVertexShader);
        }
        if (postProcessFragmentShader.isValid()) {
            rhi->destroyShader(postProcessFragmentShader);
        }

        // Destroy sampler
        if (defaultSampler.isValid()) {
            rhi->destroySampler(defaultSampler);
        }

        // Destroy pipelines
        if (mainPipeline.isValid()) {
            rhi->destroyPipeline(mainPipeline);
        }
        if (prePassPipeline.isValid()) {
            rhi->destroyPipeline(prePassPipeline);
        }
        if (postProcessPipeline.isValid()) {
            rhi->destroyPipeline(postProcessPipeline);
        }

        // Destroy compute pipelines
        if (buildClustersPipeline.isValid()) {
            rhi->destroyComputePipeline(buildClustersPipeline);
        }
        if (cullLightsPipeline.isValid()) {
            rhi->destroyComputePipeline(cullLightsPipeline);
        }
        if (tileCullingPipeline.isValid()) {
            rhi->destroyComputePipeline(tileCullingPipeline);
        }
        if (normalResolvePipeline.isValid()) {
            rhi->destroyComputePipeline(normalResolvePipeline);
        }
        if (raytraceShadowPipeline.isValid()) {
            rhi->destroyComputePipeline(raytraceShadowPipeline);
        }
        if (raytraceAOPipeline.isValid()) {
            rhi->destroyComputePipeline(raytraceAOPipeline);
        }

        // Destroy acceleration structures
        for (auto& blas : BLASs) {
            if (blas.isValid()) {
                rhi->destroyAccelerationStructure(blas);
            }
        }
        if (TLAS.isValid()) {
            rhi->destroyAccelerationStructure(TLAS);
        }
    }

    meshes.clear();
    materials.clear();
    textures.clear();
    textureCache.clear();
    BLASs.clear();
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
    // Update frame state
    updateFrameData();

    // Perform culling and sorting
    performCulling();
    sortDrawables();
    updateBuffers();

    // Choose render path
    switch (currentRenderPath) {
    case RenderPath::Forward:
        // Simple forward rendering (no advanced features)
        executeDrawCalls();
        break;

    case RenderPath::Deferred:
        // TODO: Implement deferred rendering
        executeDrawCalls();
        break;

    case RenderPath::Clustered:
        // Clustered forward rendering with ray tracing
        // 1. Pre-pass: Render depth and normals
        prePass();

        // 2. Normal resolve (MSAA to single sample for compute shaders)
        normalResolvePass();

        // 3. Build light clusters
        clusterBuildPass();

        // 4. Cull lights against clusters
        lightCullingPass();

        // 5. Ray trace shadows (if TLAS is available)
        if (TLAS.isValid()) {
            raytraceShadowPass();
        }

        // 6. Ray trace ambient occlusion (if TLAS is available)
        if (TLAS.isValid()) {
            raytraceAOPass();
        }

        // 7. Main rendering pass with clustered lighting
        mainRenderPass();

        // 8. Post-processing (tone mapping, etc.)
        postProcessPass();
        break;
    }

    // Update stats
    drawCount++;
    currentInstanceCount = static_cast<Uint32>(visibleDrawables.size());
    culledInstanceCount = static_cast<Uint32>(frameDrawables.size() - visibleDrawables.size());
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

// ============================================================================
// Render Path Management
// ============================================================================

void Renderer::setRenderPath(RenderPath path) {
    currentRenderPath = path;
}

// ============================================================================
// Render Target Creation
// ============================================================================

void Renderer::createRenderTargets() {
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();

    // Create color render targets (HDR format)
    TextureDesc colorRTDesc;
    colorRTDesc.width = width;
    colorRTDesc.height = height;
    colorRTDesc.format = PixelFormat::RGBA16_FLOAT;
    colorRTDesc.usage = TextureUsage::RenderTarget;
    colorRTDesc.sampleCount = MSAA_SAMPLE_COUNT;
    colorRT_MSAA = rhi->createTexture(colorRTDesc);

    colorRTDesc.sampleCount = 1;
    colorRTDesc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
    colorRT = rhi->createTexture(colorRTDesc);

    // Create depth-stencil render targets
    TextureDesc depthRTDesc;
    depthRTDesc.width = width;
    depthRTDesc.height = height;
    depthRTDesc.format = PixelFormat::DEPTH32_FLOAT;
    depthRTDesc.usage = TextureUsage::DepthStencil;
    depthRTDesc.sampleCount = MSAA_SAMPLE_COUNT;
    depthStencilRT_MSAA = rhi->createTexture(depthRTDesc);

    depthRTDesc.sampleCount = 1;
    depthRTDesc.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
    depthStencilRT = rhi->createTexture(depthRTDesc);

    // Create normal render targets (for ray tracing)
    TextureDesc normalRTDesc;
    normalRTDesc.width = width;
    normalRTDesc.height = height;
    normalRTDesc.format = PixelFormat::RGBA16_FLOAT;
    normalRTDesc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
    normalRTDesc.sampleCount = MSAA_SAMPLE_COUNT;
    normalRT_MSAA = rhi->createTexture(normalRTDesc);

    normalRTDesc.sampleCount = 1;
    normalRTDesc.usage = TextureUsage::Sampled | TextureUsage::Storage;
    normalRT = rhi->createTexture(normalRTDesc);

    // Create shadow render target (for ray traced shadows)
    TextureDesc shadowRTDesc;
    shadowRTDesc.width = width;
    shadowRTDesc.height = height;
    shadowRTDesc.format = PixelFormat::RGBA8_UNORM;
    shadowRTDesc.usage = TextureUsage::Sampled | TextureUsage::Storage;
    shadowRTDesc.mipLevels = static_cast<Uint32>(std::floor(std::log2(std::max(width, height))) + 1);
    shadowRT = rhi->createTexture(shadowRTDesc);

    // Create ambient occlusion render target
    TextureDesc aoRTDesc;
    aoRTDesc.width = width;
    aoRTDesc.height = height;
    aoRTDesc.format = PixelFormat::R16_FLOAT;
    aoRTDesc.usage = TextureUsage::Sampled | TextureUsage::Storage;
    aoRT = rhi->createTexture(aoRTDesc);
}

// ============================================================================
// Multi-Pass Rendering Methods
// ============================================================================

void Renderer::buildAccelerationStructures() {
    // TODO: Build BLAS for each mesh (done once during initialization)
    // TODO: Build TLAS for all instances (done each frame if geometry changes)

    // This would be implemented when we integrate with the scene loading system
    // For now, this is a placeholder
}

void Renderer::updateFrameData() {
    // Update frame data (frame number, time, delta time)
    FrameData frameData;
    frameData.frameNumber = frameNumber;
    frameData.time = time;
    frameData.deltaTime = deltaTime;
    rhi->updateBuffer(frameDataBuffer, &frameData, 0, sizeof(FrameData));

    // Update instance data
    // TODO: Convert frameDrawables to InstanceData and upload
    // This would be done when we have proper drawable submission
}

void Renderer::prePass() {
    // Pre-pass: Render depth and normals only
    // This pass is used for:
    // 1. Early Z-culling in the main pass
    // 2. Providing depth/normal data for ray tracing

    RenderPassDesc prePassDesc;
    prePassDesc.colorAttachments.push_back({
        normalRT_MSAA,
        LoadOp::Clear,
        StoreOp::Store,
        {0.0f, 0.0f, 0.0f, 1.0f}
    });
    prePassDesc.depthAttachment = {
        depthStencilRT_MSAA,
        LoadOp::Clear,
        StoreOp::Store,
        {1.0f, 0}
    };
    prePassDesc.depthResolveAttachment = {
        depthStencilRT,
        LoadOp::DontCare,
        StoreOp::Store,
        {1.0f, 0}
    };

    rhi->beginRenderPass(prePassDesc);

    if (prePassPipeline.isValid()) {
        rhi->bindPipeline(prePassPipeline);

        // TODO: Draw all opaque geometry
        // This would iterate through visibleDrawables and draw them
    }

    rhi->endRenderPass();
}

void Renderer::normalResolvePass() {
    // Compute pass: Resolve MSAA normals
    // This is needed because Metal doesn't support automatic MSAA resolve for render targets

    if (!normalResolvePipeline.isValid()) {
        return;
    }

    rhi->beginComputePass();
    rhi->bindComputePipeline(normalResolvePipeline);

    // Bind MSAA normal texture as input
    rhi->setComputeTexture(0, normalRT_MSAA);

    // Bind resolved normal texture as output
    rhi->setComputeTexture(1, normalRT);

    // Dispatch one thread per pixel
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();
    rhi->dispatch(width, height, 1);

    rhi->endComputePass();
}

void Renderer::clusterBuildPass() {
    // Compute pass: Build light clusters
    // This divides the screen into a 3D grid and computes the frustum for each cluster

    if (!buildClustersPipeline.isValid()) {
        return;
    }

    rhi->beginComputePass();
    rhi->bindComputePipeline(buildClustersPipeline);

    // Bind cluster buffer
    rhi->setComputeBuffer(0, clusterBuffer);

    // Bind camera data
    rhi->setComputeBuffer(1, cameraUniformBuffer);

    // Dispatch one thread per cluster
    rhi->dispatch(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);

    rhi->endComputePass();
}

void Renderer::lightCullingPass() {
    // Compute pass: Cull lights against clusters
    // For each cluster, determines which lights affect it

    if (!tileCullingPipeline.isValid()) {
        return;
    }

    rhi->beginComputePass();
    rhi->bindComputePipeline(tileCullingPipeline);

    // Bind cluster buffer (read/write)
    rhi->setComputeBuffer(0, clusterBuffer);

    // Bind light buffers
    rhi->setComputeBuffer(1, pointLightBuffer);
    rhi->setComputeBuffer(2, cameraUniformBuffer);

    // Dispatch one thread per cluster (2D grid for tiled culling)
    rhi->dispatch(clusterGridSizeX, clusterGridSizeY, 1);

    rhi->endComputePass();
}

void Renderer::raytraceShadowPass() {
    // Compute pass: Ray trace hard shadows
    // Uses acceleration structures to trace shadow rays from each pixel

    if (!raytraceShadowPipeline.isValid() || !TLAS.isValid()) {
        return;
    }

    rhi->beginComputePass();
    rhi->bindComputePipeline(raytraceShadowPipeline);

    // Bind input textures
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);

    // Bind output texture
    rhi->setComputeTexture(2, shadowRT);

    // Bind buffers
    rhi->setComputeBuffer(0, cameraUniformBuffer);
    rhi->setComputeBuffer(1, directionalLightBuffer);
    rhi->setComputeBuffer(2, pointLightBuffer);

    // Bind acceleration structure
    rhi->setAccelerationStructure(4, TLAS);

    // Dispatch one thread per pixel
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();
    rhi->dispatch(width, height, 1);

    rhi->endComputePass();
}

void Renderer::raytraceAOPass() {
    // Compute pass: Ray trace ambient occlusion
    // Shoots random rays in the hemisphere to compute occlusion

    if (!raytraceAOPipeline.isValid() || !TLAS.isValid()) {
        return;
    }

    rhi->beginComputePass();
    rhi->bindComputePipeline(raytraceAOPipeline);

    // Bind input textures
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);

    // Bind output texture
    rhi->setComputeTexture(2, aoRT);

    // Bind buffers
    rhi->setComputeBuffer(0, frameDataBuffer);
    rhi->setComputeBuffer(1, cameraUniformBuffer);

    // Bind acceleration structure
    rhi->setAccelerationStructure(2, TLAS);

    // Dispatch one thread per pixel
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();
    rhi->dispatch(width, height, 1);

    rhi->endComputePass();
}

void Renderer::mainRenderPass() {
    // Main rendering pass: Draw all visible geometry
    // Uses clustered lighting and pre-computed shadows/AO

    RenderPassDesc renderPassDesc;
    renderPassDesc.colorAttachments.push_back({
        colorRT_MSAA,
        LoadOp::Clear,
        StoreOp::Store,
        {clearColor.r, clearColor.g, clearColor.b, clearColor.a}
    });
    renderPassDesc.colorResolveAttachments.push_back({
        colorRT,
        LoadOp::DontCare,
        StoreOp::Store,
        {0.0f, 0.0f, 0.0f, 0.0f}
    });
    renderPassDesc.depthAttachment = {
        depthStencilRT_MSAA,
        LoadOp::Load,  // Load from pre-pass
        StoreOp::Store,
        {1.0f, 0}
    };

    rhi->beginRenderPass(renderPassDesc);

    if (mainPipeline.isValid()) {
        rhi->bindPipeline(mainPipeline);

        // Bind camera uniform
        rhi->setUniformBuffer(0, 0, cameraUniformBuffer);

        // Bind light buffers
        rhi->setUniformBuffer(0, 1, directionalLightBuffer);
        rhi->setUniformBuffer(0, 2, pointLightBuffer);

        // Bind cluster buffer
        rhi->setStorageBuffer(0, 3, clusterBuffer);

        // TODO: Bind shadow and AO textures
        // TODO: Draw all visible drawables
        // This would iterate through visibleDrawables, bind materials, and draw
    }

    rhi->endRenderPass();
}

void Renderer::postProcessPass() {
    // Post-processing pass: Tone mapping, gamma correction, etc.
    // Renders a fullscreen triangle

    if (!postProcessPipeline.isValid()) {
        return;
    }

    RenderPassDesc postPassDesc;
    postPassDesc.colorAttachments.push_back({
        TextureHandle{0},  // This would be the swapchain image
        LoadOp::Clear,
        StoreOp::Store,
        {clearColor.r, clearColor.g, clearColor.b, clearColor.a}
    });

    rhi->beginRenderPass(postPassDesc);
    rhi->bindPipeline(postProcessPipeline);

    // Bind input textures (HDR color, AO, normals for debug)
    rhi->setTexture(0, 0, colorRT, defaultSampler);
    rhi->setTexture(0, 1, aoRT, defaultSampler);
    rhi->setTexture(0, 2, normalRT, defaultSampler);

    // Draw fullscreen triangle
    rhi->draw(3, 1, 0, 0);

    rhi->endRenderPass();
}
