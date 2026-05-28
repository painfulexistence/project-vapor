#include "renderer.hpp"
#include "rhi_vulkan.hpp"
#include "rhi_metal.hpp"
#include "helper.hpp"
#include <SDL3/SDL_video.h>
#include <map>
#include <algorithm>
#include <cstring>
#include <memory>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_metal.h"
#include "backends/imgui_impl_vulkan.h"
#include <vulkan/vulkan.h>

// Metal headers for ImGui initialization
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>

// ============================================================================
// Initialization
// ============================================================================

void Renderer::initialize(std::unique_ptr<RHI> rhiPtr, GraphicsBackend backendType) {
    rhi = std::move(rhiPtr);
    backend = backendType;

    // Create uniform buffers
    BufferDesc cameraBufferDesc;
    cameraBufferDesc.size = sizeof(CameraRenderData);
    cameraBufferDesc.usage = BufferUsage::Uniform;
    cameraBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    cameraUniformBuffer = rhi->createBuffer(cameraBufferDesc);

    // Material data buffer - stores array of all materials (used by shader at binding 1)
    BufferDesc materialBufferDesc;
    materialBufferDesc.size = sizeof(MaterialData) * MAX_INSTANCES;  // Reserve space for max materials
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

    BufferDesc instanceDataBufferDesc;
    instanceDataBufferDesc.size = sizeof(InstanceData) * MAX_INSTANCES;
    instanceDataBufferDesc.usage = BufferUsage::Uniform;
    instanceDataBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    instanceDataBuffer = rhi->createBuffer(instanceDataBufferDesc);

    // Create default resources
    createDefaultResources();

    // Create render targets
    createRenderTargets();

    // Create render pipeline
    createRenderPipeline();

    // Initialize batch rendering
    initBatchRendering();

    // Initialize post-processing
    initPostProcessing();

    // Reserve space for per-frame data
    frameDrawables.reserve(MAX_INSTANCES);
    visibleDrawables.reserve(MAX_INSTANCES);
    directionalLights.reserve(maxDirectionalLights);
    pointLights.reserve(maxPointLights);
}

void Renderer::shutdown() {
    if (rhi) {
        // Shutdown ImGui backend
        switch (backend) {
            case GraphicsBackend::Metal:
                ImGui_ImplMetal_Shutdown();
                ImGui_ImplSDL3_Shutdown();
                break;
            case GraphicsBackend::Vulkan:
                ImGui_ImplVulkan_Shutdown();
                ImGui_ImplSDL3_Shutdown();
                break;
            default:
                break;
        }

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

        // Shutdown batch rendering
        shutdownBatchRendering();

        // Shutdown post-processing
        shutdownPostProcessing();

        // Destroy render textures
        for (auto& rt : renderTextures) {
            if (rt.colorTexture.isValid()) {
                rhi->destroyTexture(rt.colorTexture);
            }
            if (rt.depthTexture.isValid()) {
                rhi->destroyTexture(rt.depthTexture);
            }
        }
    }

    meshes.clear();
    materials.clear();
    textures.clear();
    textureCache.clear();
    renderTextures.clear();
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
    // Begin RHI frame (get drawable, create command buffer)
    rhi->beginFrame();

    // Call ImGui backend NewFrame (matching old renderer behavior)
    // This must be called before ImGui::NewFrame() in main.cpp
    // We need to create a render pass descriptor with swapchain texture
    if (backend == GraphicsBackend::Metal) {
        RHI_Metal* metalRHI = dynamic_cast<RHI_Metal*>(rhi.get());
        if (metalRHI) {
            CA::MetalDrawable* drawable = metalRHI->getCurrentDrawable();
            if (drawable) {
                // Create render pass descriptor for ImGui
                MTL::RenderPassDescriptor* imguiPassDesc = MTL::RenderPassDescriptor::alloc()->init();
                if (imguiPassDesc) {
                    auto colorAttachment = imguiPassDesc->colorAttachments()->object(0);
                    colorAttachment->setTexture(drawable->texture());
                    colorAttachment->setLoadAction(MTL::LoadActionLoad);
                    colorAttachment->setStoreAction(MTL::StoreActionStore);

                    // Call ImGui_ImplMetal_NewFrame with the descriptor
                    ImGui_ImplMetal_NewFrame(imguiPassDesc);

                    imguiPassDesc->release();
                }
            }
        }
    }

    currentCamera = camera;
    frameDrawables.clear();
    visibleDrawables.clear();
    directionalLights.clear();
    pointLights.clear();

    fmt::print("beginFrame: camera position=({}, {}, {}), frameDrawables cleared\n",
               camera.position.x, camera.position.y, camera.position.z);
}

void Renderer::submitDrawable(const Drawable& drawable) {
    frameDrawables.push_back(drawable);
    fmt::print("submitDrawable: mesh={}, material={}, transform=[{}, {}, {}]\n",
               drawable.mesh, drawable.material,
               drawable.transform[3][0], drawable.transform[3][1], drawable.transform[3][2]);
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

    // Multi-pass rendering (matching old renderer order)
    if (backend == GraphicsBackend::Metal) {
        buildAccelerationStructures();
        prePass();
        normalResolvePass();
        tileCullingPass();
        raytraceShadowPass();
        raytraceAOPass();
    }

    // If post-process pipeline exists, render to colorRT then post-process to swapchain
    // Otherwise, render directly to swapchain
    if (postProcessPipeline.isValid() && colorRT.isValid()) {
        mainRenderPass();  // Render to colorRT
        postProcessPass();  // Render from colorRT to swapchain (fullscreen pass)
    } else {
        // Fallback: render directly to swapchain
        mainRenderPass();  // This will render to swapchain if render targets don't exist
    }
}

void Renderer::endFrame() {
    fmt::print("endFrame(): ImGui::GetDrawData()={}, CmdListsCount={}\n",
               (void*)ImGui::GetDrawData(),
               ImGui::GetDrawData() ? ImGui::GetDrawData()->CmdListsCount : 0);

    // Render ImGui (matching old renderer behavior)
    // Note: ImGui::NewFrame() and ImGui::Render() should be called by user code
    // We only handle the backend rendering here

    if (ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount > 0) {
        fmt::print("Rendering ImGui with {} command lists\n", ImGui::GetDrawData()->CmdListsCount);
        // Render ImGui using backend-specific implementation
        // Matching old renderer: create render pass, then render ImGui
        switch (backend) {
            case GraphicsBackend::Metal: {
                void* cmdBuffer = rhi->getBackendCommandBuffer();
                if (cmdBuffer) {
                    // Create ImGui render pass (load existing content, don't clear)
                    RenderPassDesc imguiPassDesc;
                    imguiPassDesc.colorAttachments.push_back(TextureHandle{0});  // Use swapchain
                    imguiPassDesc.loadColor.push_back(true);  // Load (don't clear, render on top)
                    imguiPassDesc.depthAttachment = TextureHandle{0};  // Use default depth
                    imguiPassDesc.loadDepth = true;  // Load (don't clear)

                    rhi->beginRenderPass(imguiPassDesc);

                    // Get the current render encoder from RHI_Metal
                    RHI_Metal* metalRHI = dynamic_cast<RHI_Metal*>(rhi.get());
                    if (metalRHI) {
                        MTL::RenderCommandEncoder* encoder = metalRHI->getCurrentRenderEncoder();
                        if (encoder) {
                            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),
                                static_cast<MTL::CommandBuffer*>(cmdBuffer),
                                encoder);
                        }
                    }

                    rhi->endRenderPass();
                }
                break;
            }
            case GraphicsBackend::Vulkan: {
                // Vulkan ImGui rendering
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                    static_cast<VkCommandBuffer>(rhi->getBackendCommandBuffer()));
                break;
            }
            default:
                break;
        }
    }

    // End RHI frame (present drawable, commit command buffer)
    rhi->endFrame();

    // Update frame state
    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
    frameNumber++;
}

// ============================================================================
// Internal Rendering Steps
// ============================================================================

void Renderer::performCulling() {
    Frustum frustum = extractFrustum(currentCamera.viewProj);

    fmt::print("performCulling: frameDrawables.size()={}\n", frameDrawables.size());

    Uint32 culledCount = 0;
    for (Uint32 i = 0; i < frameDrawables.size(); ++i) {
        const Drawable& d = frameDrawables[i];
        if (frustum.isBoxVisible(d.aabbMin, d.aabbMax)) {
            visibleDrawables.push_back(i);
        } else {
            culledCount++;
            // Debug: print first few culled drawables
            if (culledCount <= 3) {
                glm::vec3 pos = glm::vec3(d.transform[3]);
                fmt::print("Culled drawable {}: mesh={}, transform=[{}, {}, {}], AABB=[({}, {}, {}), ({}, {}, {})]\n",
                           i, d.mesh, pos.x, pos.y, pos.z,
                           d.aabbMin.x, d.aabbMin.y, d.aabbMin.z,
                           d.aabbMax.x, d.aabbMax.y, d.aabbMax.z);
            }
        }
    }

    fmt::print("performCulling: visibleDrawables.size()={}, culled={}\n", visibleDrawables.size(), culledCount);
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

    // Update material data buffer (array of all materials for shader binding 1)
    if (!materials.empty()) {
        std::vector<MaterialData> materialDataArray;
        materialDataArray.reserve(materials.size());
        for (const auto& mat : materials) {
            MaterialData data;
            data.baseColorFactor = mat.baseColorFactor;
            data.normalScale = mat.normalScale;
            data.metallicFactor = mat.metallicFactor;
            data.roughnessFactor = mat.roughnessFactor;
            data.occlusionStrength = mat.occlusionStrength;
            data.emissiveFactor = mat.emissiveFactor;
            data.emissiveStrength = mat.emissiveStrength;
            data.subsurface = mat.subsurface;
            data.specular = mat.specular;
            data.specularTint = mat.specularTint;
            data.anisotropic = mat.anisotropic;
            data.sheen = mat.sheen;
            data.sheenTint = mat.sheenTint;
            data.clearcoat = mat.clearcoat;
            data.clearcoatGloss = mat.clearcoatGloss;
            materialDataArray.push_back(data);
        }
        rhi->updateBuffer(materialUniformBuffer, materialDataArray.data(), 0,
                          materialDataArray.size() * sizeof(MaterialData));
    }

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

    // Update instance data
    // Create a map from drawable index to instance ID for correct indexing
    drawableToInstanceID.clear();
    std::vector<InstanceData> instanceData;
    instanceData.reserve(visibleDrawables.size());
    Uint32 nonIdentityCount = 0;
    Uint32 instanceID = 0;
    for (Uint32 drawableIdx : visibleDrawables) {
        const Drawable& drawable = frameDrawables[drawableIdx];
        const RenderMesh& mesh = meshes[drawable.mesh];

        InstanceData instance;
        instance.model = drawable.transform;
        instance.color = drawable.color;
        instance.vertexOffset = 0;  // Each mesh has its own buffer
        instance.indexOffset = 0;   // Each mesh has its own buffer
        instance.vertexCount = mesh.vertexCount;
        instance.indexCount = mesh.indexCount;
        instance.materialID = drawable.material;
        instance.primitiveMode = PrimitiveMode::TRIANGLES;
        instance.AABBMin = drawable.aabbMin;
        instance.AABBMax = drawable.aabbMax;
        instance.boundingSphere = glm::vec4(
            (drawable.aabbMin + drawable.aabbMax) * 0.5f,
            glm::length(glm::vec3(drawable.aabbMax - drawable.aabbMin)) * 0.5f
        );
        instanceData.push_back(instance);
        drawableToInstanceID[drawableIdx] = instanceID;
        instanceID++;

        // Debug: count non-identity transforms
        glm::vec3 pos = glm::vec3(drawable.transform[3]);
        if (glm::length(pos) > 0.001f) {
            nonIdentityCount++;
        }
    }
    fmt::print("updateBuffers: {} visible drawables, {} with non-identity transform\n",
               visibleDrawables.size(), nonIdentityCount);

    if (!instanceData.empty()) {
        rhi->updateBuffer(instanceDataBuffer, instanceData.data(), 0,
                          instanceData.size() * sizeof(InstanceData));
    }
}

void Renderer::mainRenderPass() {
    fmt::print("mainRenderPass: visibleDrawables.size()={}, mainPipeline.isValid()={}\n",
               visibleDrawables.size(), mainPipeline.isValid());

    if (visibleDrawables.empty() || !mainPipeline.isValid()) {
        if (visibleDrawables.empty()) {
            fmt::print("mainRenderPass: No visible drawables, skipping draw\n");
        }
        if (!mainPipeline.isValid()) {
            fmt::print("mainRenderPass: Invalid pipeline, skipping draw\n");
        }
        return;  // Nothing to draw or no pipeline
    }

    // Check if colorRT is valid (should be created in createRenderTargets)
    // If render targets don't exist, fallback to rendering directly to swapchain
    bool useRenderTargets = colorRT_MSAA.isValid() && colorRT.isValid() && depthStencilRT_MSAA.isValid();

    if (!useRenderTargets) {
        fmt::print("mainRenderPass: Render targets not valid, rendering directly to swapchain\n");
    }

    // Get swapchain dimensions for render pass
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();

    // Create render pass descriptor
    RenderPassDesc renderPassDesc;
    // If post-process pipeline exists, render to colorRT; otherwise render directly to swapchain
    bool usePostProcess = postProcessPipeline.isValid() && colorRT.isValid();
    if (useRenderTargets && usePostProcess) {
        // Render to colorRT (MSAA with resolve) for post-processing
        renderPassDesc.colorAttachments.push_back(colorRT_MSAA);  // Render to MSAA color RT
        renderPassDesc.resolveAttachments.push_back(colorRT);  // Resolve to non-MSAA color RT
        renderPassDesc.depthAttachment = depthStencilRT_MSAA;  // Use MSAA depth RT
    } else {
        // Fallback: render directly to swapchain
        renderPassDesc.colorAttachments.push_back(TextureHandle{0});  // Use swapchain (handle 0 is special)
        renderPassDesc.depthAttachment = swapchainDepthBuffer;  // Use swapchain depth buffer
    }
    renderPassDesc.clearColors.push_back(glm::vec4(0.2f, 0.2f, 0.3f, 1.0f));
    renderPassDesc.loadColor.push_back(false);  // Clear
    renderPassDesc.clearDepth = 1.0f;
    renderPassDesc.loadDepth = false;  // Clear (first frame) or Load (subsequent frames)

    // Begin render pass
    rhi->beginRenderPass(renderPassDesc);

    // Bind pipeline
    rhi->bindPipeline(mainPipeline);

    // Bind common buffers (same for all drawables)
    // Vertex buffers (matching old renderer):
    // Binding 0: CameraData
    rhi->setUniformBuffer(0, 0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    // Binding 1: MaterialData array (all materials)
    rhi->setUniformBuffer(0, 1, materialUniformBuffer, 0, sizeof(MaterialData) * MAX_INSTANCES);
    // Binding 2: InstanceData array
    // Note: We only update the buffer with visible drawables, so the size is visibleDrawables.size()
    rhi->setUniformBuffer(0, 2, instanceDataBuffer, 0, sizeof(InstanceData) * visibleDrawables.size());

    // Fragment buffers (matching old renderer - these are separate from vertex bindings):
    // Fragment binding 0: DirectionalLights
    if (directionalLightBuffer.isValid() && !directionalLights.empty()) {
        rhi->setUniformBuffer(0, 0, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
    }
    // Fragment binding 1: PointLights
    if (pointLightBuffer.isValid() && !pointLights.empty()) {
        rhi->setUniformBuffer(0, 1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
    }
    // Fragment binding 2: Clusters (if we have cluster buffer - skip for now)
    // Fragment binding 3: CameraData (for fragment shader)
    rhi->setUniformBuffer(0, 3, cameraUniformBuffer, 0, sizeof(CameraRenderData));

    // Fragment bytes (matching old renderer):
    glm::vec2 screenSize(static_cast<float>(width), static_cast<float>(height));
    rhi->setFragmentBytes(&screenSize, sizeof(glm::vec2), 4);
    glm::uvec3 gridSize(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
    rhi->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
    float time = 0.0f;  // TODO: Get actual time
    rhi->setFragmentBytes(&time, sizeof(float), 6);

    // Group drawables by material to reduce state changes
    std::map<MaterialId, std::vector<Uint32>> materialBatches;
    for (Uint32 drawableIdx : visibleDrawables) {
        const Drawable& drawable = frameDrawables[drawableIdx];
        materialBatches[drawable.material].push_back(drawableIdx);
    }

    // Draw by material batches (matching old renderer behavior)
    Uint32 instanceID = 0;
    for (const auto& [materialId, drawableIndices] : materialBatches) {
        // Bind material textures (if material is valid)
        if (materialId < materials.size()) {
            bindMaterial(materialId);
        }

        // Draw all drawables with this material
        for (Uint32 drawableIdx : drawableIndices) {
            const Drawable& drawable = frameDrawables[drawableIdx];
            const RenderMesh& mesh = meshes[drawable.mesh];

            // Get the correct instance ID for this drawable
            auto it = drawableToInstanceID.find(drawableIdx);
            if (it == drawableToInstanceID.end()) {
                fmt::print("Warning: drawable {} not found in instance ID map\n", drawableIdx);
                continue;
            }
            Uint32 correctInstanceID = it->second;

            // Debug: print first few drawables' transform and instance ID
            if (correctInstanceID < 5) {
                glm::vec3 pos = glm::vec3(drawable.transform[3]);
                fmt::print("Drawing drawable {}: instanceID={}, mesh={}, transform=[{}, {}, {}]\n",
                           drawableIdx, correctInstanceID, drawable.mesh, pos.x, pos.y, pos.z);
            }

            // Bind vertex buffer (binding 3 for Metal shader)
            rhi->bindVertexBuffer(mesh.vertexBuffer, 3, 0);

            // Bind index buffer
            rhi->bindIndexBuffer(mesh.indexBuffer, 0);

            // Set instance ID (binding 4 for Metal shader)
            rhi->setVertexBytes(&correctInstanceID, sizeof(Uint32), 4);

            // Draw indexed
            rhi->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
        }
    }

    // End render pass
    rhi->endRenderPass();
}

void Renderer::prePass() {
    // TODO: Implement pre-pass (depth + normal) for Metal
    // For now, this is a placeholder
}

void Renderer::buildAccelerationStructures() {
    // TODO: Implement acceleration structure building for Metal
    // For now, this is a placeholder
}

void Renderer::normalResolvePass() {
    // TODO: Implement normal resolve pass for Metal
    // For now, this is a placeholder
}

void Renderer::tileCullingPass() {
    // TODO: Implement tile culling pass
    // For now, this is a placeholder
}

void Renderer::raytraceShadowPass() {
    // TODO: Implement raytrace shadow pass for Metal
    // For now, this is a placeholder
}

void Renderer::raytraceAOPass() {
    // TODO: Implement raytrace AO pass for Metal
    // For now, this is a placeholder
}

void Renderer::postProcessPass() {
    // Post-process pass: render from colorRT to swapchain (fullscreen triangle)
    if (!postProcessPipeline.isValid() || !colorRT.isValid()) {
        // If no post-process pipeline, just skip (or could do a simple copy)
        fmt::print("postProcessPass: Skipping (pipeline valid={}, colorRT valid={})\n",
                   postProcessPipeline.isValid(), colorRT.isValid());
        return;
    }

    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();

    // Create render pass descriptor for swapchain
    RenderPassDesc renderPassDesc;
    renderPassDesc.colorAttachments.push_back(TextureHandle{0});  // Use swapchain (handle 0 is special)
    renderPassDesc.clearColors.push_back(glm::vec4(0.2f, 0.2f, 0.3f, 1.0f));
    renderPassDesc.loadColor.push_back(false);  // Clear

    // Begin render pass
    rhi->beginRenderPass(renderPassDesc);

    // Bind post-process pipeline
    rhi->bindPipeline(postProcessPipeline);

    // Bind colorRT, aoRT, normalRT as fragment textures (matching old renderer)
    // Fragment binding 0: colorRT
    if (colorRT.isValid() && defaultSampler.isValid()) {
        rhi->setTexture(0, 0, colorRT, defaultSampler);
    }
    // Fragment binding 1: aoRT (if available)
    if (aoRT.isValid() && defaultSampler.isValid()) {
        rhi->setTexture(0, 1, aoRT, defaultSampler);
    }
    // Fragment binding 2: normalRT (if available)
    if (normalRT.isValid() && defaultSampler.isValid()) {
        rhi->setTexture(0, 2, normalRT, defaultSampler);
    }

    // Draw fullscreen triangle (3 vertices, 1 instance)
    rhi->draw(3, 1, 0, 0);

    // End render pass
    rhi->endRenderPass();
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

void Renderer::createRenderTargets() {
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();
    constexpr Uint32 MSAA_SAMPLE_COUNT = 4;

    // Create depth/stencil RT (MSAA and resolved)
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::Depth32Float;
        desc.usage = TextureUsage::RenderTarget;
        desc.sampleCount = MSAA_SAMPLE_COUNT;
        depthStencilRT_MSAA = rhi->createTexture(desc);

        desc.sampleCount = 1;
        desc.usage = TextureUsage::RenderTarget;  // Can be sampled later if needed
        depthStencilRT = rhi->createTexture(desc);
    }

    // Create color RT (MSAA and resolved, HDR format)
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::RGBA16_FLOAT;  // HDR format
        desc.usage = TextureUsage::RenderTarget;
        desc.sampleCount = MSAA_SAMPLE_COUNT;
        colorRT_MSAA = rhi->createTexture(desc);

        desc.sampleCount = 1;
        desc.usage = TextureUsage::RenderTarget;  // Can be sampled in post-process
        colorRT = rhi->createTexture(desc);
    }

    // Create normal RT (MSAA and resolved)
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::RGBA16_FLOAT;  // HDR format
        desc.usage = TextureUsage::RenderTarget;
        desc.sampleCount = MSAA_SAMPLE_COUNT;
        normalRT_MSAA = rhi->createTexture(desc);

        desc.sampleCount = 1;
        desc.usage = TextureUsage::Storage;  // For compute shaders
        normalRT = rhi->createTexture(desc);
    }

    // Create shadow RT
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::RGBA8_UNORM;
        desc.usage = TextureUsage::Storage;  // For compute shaders
        desc.mipLevels = static_cast<Uint32>(std::floor(std::log2(std::max(width, height))) + 1);
        shadowRT = rhi->createTexture(desc);
    }

    // Create AO RT
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::R16_FLOAT;  // Single channel float
        desc.usage = TextureUsage::Storage;  // For compute shaders
        aoRT = rhi->createTexture(desc);
    }

    // Create default depth buffer for swapchain rendering (when not using render targets)
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::Depth32Float;
        desc.usage = TextureUsage::RenderTarget;
        desc.sampleCount = 1;  // No MSAA for swapchain depth
        swapchainDepthBuffer = rhi->createTexture(desc);
    }

    // Verify all render targets were created successfully
    if (!colorRT_MSAA.isValid() || !colorRT.isValid() || !depthStencilRT_MSAA.isValid() ||
        !depthStencilRT.isValid() || !normalRT_MSAA.isValid() || !normalRT.isValid() ||
        !swapchainDepthBuffer.isValid()) {
        throw std::runtime_error("Failed to create render targets");
    }

    fmt::print("createRenderTargets: Created render targets ({}x{})\n", width, height);
}

void Renderer::createRenderPipeline() {
    std::string vertShaderCode;
    std::string fragShaderCode;

    if (backend == GraphicsBackend::Vulkan) {
        vertShaderCode = readFile("assets/shaders/TBN.vert.spv");
        fragShaderCode = readFile("assets/shaders/PBRNormalMapped.frag.spv");
    } else if (backend == GraphicsBackend::Metal) {
        vertShaderCode = readFile("assets/shaders/3d_pbr_normal_mapped.metal");
        fragShaderCode = readFile("assets/shaders/3d_pbr_normal_mapped.metal");
    } else {
        return;  // Unknown backend
    }

    // Create shaders
    ShaderDesc vertShaderDesc;
    vertShaderDesc.stage = ShaderStage::Vertex;
    vertShaderDesc.code = vertShaderCode.data();
    vertShaderDesc.codeSize = vertShaderCode.size();
    vertShaderDesc.entryPoint = (backend == GraphicsBackend::Metal) ? "vertexMain" : "main";
    vertexShader = rhi->createShader(vertShaderDesc);

    ShaderDesc fragShaderDesc;
    fragShaderDesc.stage = ShaderStage::Fragment;
    fragShaderDesc.code = fragShaderCode.data();
    fragShaderDesc.codeSize = fragShaderCode.size();
    fragShaderDesc.entryPoint = (backend == GraphicsBackend::Metal) ? "fragmentMain" : "main";
    fragmentShader = rhi->createShader(fragShaderDesc);

    // Create vertex layout
    VertexLayout vertexLayout;
    vertexLayout.stride = sizeof(VertexData);
    vertexLayout.attributes = {
        {0, PixelFormat::RGBA32_FLOAT, offsetof(VertexData, position)},  // Position (vec3)
        {1, PixelFormat::RGBA32_FLOAT, offsetof(VertexData, uv)},        // UV (vec2)
        {2, PixelFormat::RGBA32_FLOAT, offsetof(VertexData, normal)},    // Normal (vec3)
        {3, PixelFormat::RGBA32_FLOAT, offsetof(VertexData, tangent)}   // Tangent (vec4)
    };

    // Create pipeline
    PipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.fragmentShader = fragmentShader;
    pipelineDesc.vertexLayout = vertexLayout;
    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.blendMode = BlendMode::Opaque;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompareOp = CompareOp::Less;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.frontFaceCounterClockwise = true;
    pipelineDesc.sampleCount = 1;

    mainPipeline = rhi->createPipeline(pipelineDesc);
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

    // Bind textures (matching shader bindings: texture(0) = albedo, texture(1) = normal, etc.)
    if (material.albedoTexture < textures.size()) {
        const RenderTexture& tex = textures[material.albedoTexture];
        rhi->setTexture(0, 0, tex.handle, tex.sampler);  // texture(0) in shader
    }

    if (material.normalTexture < textures.size()) {
        const RenderTexture& tex = textures[material.normalTexture];
        rhi->setTexture(0, 1, tex.handle, tex.sampler);  // texture(1) in shader
    }

    if (material.metallicTexture < textures.size()) {
        const RenderTexture& tex = textures[material.metallicTexture];
        rhi->setTexture(0, 2, tex.handle, tex.sampler);  // texture(2) in shader
    }

    if (material.roughnessTexture < textures.size()) {
        const RenderTexture& tex = textures[material.roughnessTexture];
        rhi->setTexture(0, 3, tex.handle, tex.sampler);  // texture(3) in shader
    }

    if (material.occlusionTexture < textures.size()) {
        const RenderTexture& tex = textures[material.occlusionTexture];
        rhi->setTexture(0, 4, tex.handle, tex.sampler);  // texture(4) in shader
    }

    if (material.emissiveTexture < textures.size()) {
        const RenderTexture& tex = textures[material.emissiveTexture];
        rhi->setTexture(0, 5, tex.handle, tex.sampler);  // texture(5) in shader
    }

    // texture(7) is shadow RT - we'll bind it separately if needed
}

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<Renderer> createRenderer(GraphicsBackend backend, SDL_Window* window) {
    std::unique_ptr<RHI> rhi;

    switch (backend) {
        case GraphicsBackend::Metal:
            rhi = std::unique_ptr<RHI>(createRHIMetal());
            break;
        case GraphicsBackend::Vulkan:
            rhi = std::unique_ptr<RHI>(createRHIVulkan());
            break;
        default:
            return nullptr;
    }

    if (!rhi) {
        return nullptr;
    }

    // Initialize RHI with window
    if (!rhi->initialize(window)) {
        return nullptr;
    }

    // Initialize ImGui backend based on graphics backend
    switch (backend) {
        case GraphicsBackend::Metal: {
            ImGui_ImplSDL3_InitForMetal(window);
            void* device = rhi->getBackendDevice();
            if (device) {
                ImGui_ImplMetal_Init(static_cast<MTL::Device*>(device));
            }
            break;
        }
        case GraphicsBackend::Vulkan: {
            ImGui_ImplSDL3_InitForVulkan(window);
            void* instance = rhi->getBackendInstance();
            void* physicalDevice = rhi->getBackendPhysicalDevice();
            void* device = rhi->getBackendDevice();
            void* queue = rhi->getBackendQueue();

            if (instance && physicalDevice && device && queue) {
                // Get swapchain image count from RHI
                // For now, use a reasonable default (2-3 images)
                Uint32 imageCount = 2;

                ImGui_ImplVulkan_InitInfo initInfo = {};
                initInfo.Instance = static_cast<VkInstance>(instance);
                initInfo.PhysicalDevice = static_cast<VkPhysicalDevice>(physicalDevice);
                initInfo.Device = static_cast<VkDevice>(device);
                initInfo.QueueFamily = 0; // Graphics queue family is typically 0
                initInfo.Queue = static_cast<VkQueue>(queue);
                initInfo.MinImageCount = imageCount;
                initInfo.ImageCount = imageCount;
                initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                initInfo.PipelineCache = VK_NULL_HANDLE;
                initInfo.DescriptorPoolSize = 1000;
                initInfo.UseDynamicRendering = true;
                initInfo.Allocator = nullptr;
                initInfo.CheckVkResultFn = nullptr;
                ImGui_ImplVulkan_Init(&initInfo);
            }
            break;
        }
        default:
            break;
    }

    // Create Renderer and transfer RHI ownership
    auto renderer = std::make_unique<Renderer>();
    renderer->initialize(std::move(rhi), backend);

    return renderer;
}

// ============================================================================
// Scene/ECS Integration
// ============================================================================

void Renderer::stage(std::shared_ptr<Scene> scene) {
    // TODO: Implement scene staging
    // This should extract and upload all meshes, materials, textures from the scene
}

void Renderer::draw(std::shared_ptr<Scene> scene, Camera& camera) {
    // TODO: Implement scene drawing
    // Collect drawables from scene, then use existing render() method
}

void Renderer::draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera) {
    // TODO: Implement ECS drawing
    // Collect drawables from ECS registry, then use existing render() method
}

void Renderer::collectDrawables(std::shared_ptr<Scene> scene) {
    // TODO: Traverse scene graph and collect renderables
}

void Renderer::collectDrawables(entt::registry& registry, std::shared_ptr<Scene> scene) {
    // TODO: Query ECS for entities with renderable components
}

// ============================================================================
// Screenshot API
// ============================================================================

void Renderer::readPixelsAsync(ScreenshotCallback callback) {
    screenshotCallback = callback;
    screenshotRequested = true;
}

// ============================================================================
// UI Integration
// ============================================================================

bool Renderer::initUI() {
    // TODO: Initialize RmlUI rendering
    return false;
}

std::shared_ptr<Vapor::DebugDraw> Renderer::getDebugDraw() {
    return debugDraw;
}

void Renderer::setImGuiCallback(std::function<void()> callback) {
    imGuiCallback = std::move(callback);
}

// ============================================================================
// Batch Rendering Implementation
// ============================================================================

void Renderer::initBatchRendering() {
    // Initialize batch2D
    batch2D.init(rhi.get(), false, defaultWhiteTexture);

    // Initialize batch3D
    batch3D.init(rhi.get(), true, defaultWhiteTexture);
}

void Renderer::shutdownBatchRendering() {
    batch2D.shutdown(rhi.get());
    batch3D.shutdown(rhi.get());
}

void Renderer::flush2D() {
    if (batch2D.quadCount > 0) {
        // TODO: Get view-projection matrix for 2D (orthographic)
        glm::mat4 viewProj = glm::ortho(
            0.0f, static_cast<float>(rhi->getSwapchainWidth()),
            static_cast<float>(rhi->getSwapchainHeight()), 0.0f,
            -1.0f, 1.0f
        );
        batch2D.flush(rhi.get(), viewProj);
    }
}

void Renderer::flush3D() {
    if (batch3D.quadCount > 0) {
        // Use current camera's view-projection
        glm::mat4 viewProj = currentCamera.projection * currentCamera.view;
        batch3D.flush(rhi.get(), viewProj);
    }
}

// 2D Quad drawing implementations
void Renderer::drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) {
    batch2D.addQuad(glm::vec3(position, 0.0f), size, color);
}

void Renderer::drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    batch2D.addQuad(position, size, color);
}

void Renderer::drawQuad2D(
    const glm::vec2& position,
    const glm::vec2& size,
    TextureHandle texture,
    const glm::vec4& tintColor
) {
    // TODO: Support textured quads
    drawQuad2D(position, size, tintColor);
}

void Renderer::drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    batch2D.addQuad(transform, color, entityID);
}

void Renderer::drawQuad2D(
    const glm::mat4& transform,
    TextureHandle texture,
    const glm::vec2* texCoords,
    const glm::vec4& tintColor,
    int entityID
) {
    // TODO: Support textured quads with custom tex coords
    batch2D.addQuad(transform, tintColor, entityID);
}

// 3D Quad drawing implementations
void Renderer::drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    batch3D.addQuad(position, size, color);
}

void Renderer::drawQuad3D(
    const glm::vec3& position,
    const glm::vec2& size,
    TextureHandle texture,
    const glm::vec4& tintColor
) {
    // TODO: Support textured quads
    drawQuad3D(position, size, tintColor);
}

void Renderer::drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    batch3D.addQuad(transform, color, entityID);
}

void Renderer::drawQuad3D(
    const glm::mat4& transform,
    TextureHandle texture,
    const glm::vec2* texCoords,
    const glm::vec4& tintColor,
    int entityID
) {
    // TODO: Support textured quads with custom tex coords
    batch3D.addQuad(transform, tintColor, entityID);
}

// Rotated quad
void Renderer::drawRotatedQuad2D(
    const glm::vec2& position,
    const glm::vec2& size,
    float rotation,
    const glm::vec4& color
) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f));
    transform = glm::rotate(transform, rotation, glm::vec3(0, 0, 1));
    transform = glm::scale(transform, glm::vec3(size, 1.0f));
    batch2D.addQuad(transform, color);
}

void Renderer::drawRotatedQuad2D(
    const glm::vec2& position,
    const glm::vec2& size,
    float rotation,
    TextureHandle texture,
    const glm::vec4& tintColor
) {
    // TODO: Support textured rotated quads
    drawRotatedQuad2D(position, size, rotation, tintColor);
}

// Line drawing
void Renderer::drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness) {
    // TODO: Implement line rendering (can be done with thin quads)
}

void Renderer::drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness) {
    // TODO: Implement 3D line rendering
}

// Shape drawing
void Renderer::drawRect2D(
    const glm::vec2& position,
    const glm::vec2& size,
    const glm::vec4& color,
    float thickness
) {
    // Draw 4 lines to form rectangle
    // TODO: Implement using drawLine2D
}

void Renderer::drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    // TODO: Draw circle outline using line segments
}

void Renderer::drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    // TODO: Draw filled circle using triangle fan or quads
}

void Renderer::drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    // TODO: Draw triangle outline
}

void Renderer::drawTriangleFilled2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    // TODO: Draw filled triangle
}

// Batch stats
Batch2DStats Renderer::getBatch2DStats() const {
    return batch2DStats;
}

void Renderer::resetBatch2DStats() {
    batch2DStats = {};
    batch2D.drawCalls = 0;
    batch2D.totalQuads = 0;
}

// ============================================================================
// Font Rendering
// ============================================================================

FontHandle Renderer::loadFont(const std::string& path, float baseSize) {
    if (!fontManager) {
        fontManager = std::make_unique<FontManager>();
    }
    return fontManager->loadFont(path, baseSize);
}

void Renderer::unloadFont(FontHandle handle) {
    if (fontManager) {
        fontManager->unloadFont(handle);
    }
}

void Renderer::drawText2D(
    FontHandle font,
    const std::string& text,
    const glm::vec2& position,
    float scale,
    const glm::vec4& color
) {
    // TODO: Use fontManager to get glyph quads, then batch render them
}

void Renderer::drawText3D(
    FontHandle font,
    const std::string& text,
    const glm::vec3& worldPosition,
    float scale,
    const glm::vec4& color
) {
    // TODO: Billboard text in 3D space
}

glm::vec2 Renderer::measureText(FontHandle font, const std::string& text, float scale) {
    if (fontManager) {
        return fontManager->measureText(font, text, scale);
    }
    return glm::vec2(0.0f);
}

float Renderer::getFontLineHeight(FontHandle font, float scale) {
    if (fontManager) {
        return fontManager->getLineHeight(font, scale);
    }
    return 0.0f;
}

// ============================================================================
// Render-to-Texture
// ============================================================================

RenderTextureHandle Renderer::createRenderTexture(const RenderTextureDesc& desc) {
    RenderTextureResource resource;
    resource.width = desc.width;
    resource.height = desc.height;
    resource.format = desc.format;
    resource.isHDR = desc.isHDR;
    resource.hasDepth = desc.hasDepth;

    // Create color texture
    TextureDesc colorDesc;
    colorDesc.width = desc.width;
    colorDesc.height = desc.height;
    colorDesc.format = desc.format;
    colorDesc.usage = TextureUsage::RenderTarget;
    colorDesc.sampleCount = desc.sampleCount;
    resource.colorTexture = rhi->createTexture(colorDesc);

    // Create depth texture if needed
    if (desc.hasDepth) {
        TextureDesc depthDesc;
        depthDesc.width = desc.width;
        depthDesc.height = desc.height;
        depthDesc.format = PixelFormat::Depth32Float;
        depthDesc.usage = TextureUsage::DepthStencil;
        depthDesc.sampleCount = desc.sampleCount;
        resource.depthTexture = rhi->createTexture(depthDesc);
    }

    // Add to storage
    uint32_t id = static_cast<uint32_t>(renderTextures.size());
    renderTextures.push_back(resource);

    return RenderTextureHandle{id};
}

void Renderer::destroyRenderTexture(RenderTextureHandle handle) {
    if (handle.id < renderTextures.size()) {
        auto& resource = renderTextures[handle.id];
        if (resource.colorTexture.isValid()) {
            rhi->destroyTexture(resource.colorTexture);
        }
        if (resource.depthTexture.isValid()) {
            rhi->destroyTexture(resource.depthTexture);
        }
    }
}

TextureHandle Renderer::getRenderTextureAsTexture(RenderTextureHandle handle) {
    if (handle.id < renderTextures.size()) {
        return renderTextures[handle.id].colorTexture;
    }
    return TextureHandle{};
}

void Renderer::renderToTexture(
    RenderTextureHandle target,
    std::shared_ptr<Scene> scene,
    Camera& camera,
    const glm::vec4& clearColor
) {
    // TODO: Implement render-to-texture
    // 1. Begin render pass with target textures
    // 2. Draw scene
    // 3. End render pass
}

glm::uvec2 Renderer::getRenderTextureSize(RenderTextureHandle handle) {
    if (handle.id < renderTextures.size()) {
        auto& resource = renderTextures[handle.id];
        return glm::uvec2(resource.width, resource.height);
    }
    return glm::uvec2(0);
}

Uint64 Renderer::registerRenderTextureForUI(RenderTextureHandle handle) {
    // TODO: Register with RmlUI
    return 0;
}

// ============================================================================
// Post-Processing
// ============================================================================

void Renderer::initPostProcessing() {
    // TODO: Create compute pipelines for post-processing effects
}

void Renderer::shutdownPostProcessing() {
    // TODO: Destroy post-processing resources
}

void Renderer::applyBloom(RenderTextureHandle target, float threshold, float strength) {
    // TODO: Implement bloom effect
    // 1. Downsample with threshold
    // 2. Blur
    // 3. Upsample and combine
}

void Renderer::applyToneMapping(RenderTextureHandle target, float exposure) {
    // TODO: Implement tone mapping (ACES, Reinhard, etc.)
}

void Renderer::applyVignette(RenderTextureHandle target, float strength, float radius) {
    // TODO: Implement vignette effect
}

// ============================================================================
// Texture Creation (for sprites)
// ============================================================================

TextureHandle Renderer::createTexture(const std::shared_ptr<Image>& img) {
    TextureDesc desc;
    desc.width = img->width;
    desc.height = img->height;
    desc.format = PixelFormat::RGBA8_UNORM;
    desc.usage = TextureUsage::Sampled;
    desc.mipLevels = 1;

    TextureHandle handle = rhi->createTexture(desc);

    // Upload data
    size_t dataSize = img->width * img->height * img->channels;
    rhi->updateTexture(handle, img->data.data(), dataSize);

    return handle;
}

// ============================================================================
// BatchRenderer Implementation
// ============================================================================

void Renderer::BatchRenderer::init(RHI* rhi, bool is3D, TextureHandle defaultTex) {
    whiteTexture = defaultTex;

    // Create vertex buffer
    BufferDesc vbDesc;
    vbDesc.size = sizeof(Vertex2D) * MaxVertices;
    vbDesc.usage = BufferUsage::Vertex;
    vbDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    vertexBuffer = rhi->createBuffer(vbDesc);

    // Create index buffer with quad indices (0,1,2, 2,3,0 pattern)
    std::vector<uint32_t> quadIndices;
    quadIndices.reserve(MaxIndices);
    for (uint32_t i = 0; i < MaxQuads; i++) {
        uint32_t offset = i * 4;
        quadIndices.push_back(offset + 0);
        quadIndices.push_back(offset + 1);
        quadIndices.push_back(offset + 2);
        quadIndices.push_back(offset + 2);
        quadIndices.push_back(offset + 3);
        quadIndices.push_back(offset + 0);
    }

    BufferDesc ibDesc;
    ibDesc.size = sizeof(uint32_t) * MaxIndices;
    ibDesc.usage = BufferUsage::Index;
    ibDesc.memoryUsage = MemoryUsage::GPU;
    indexBuffer = rhi->createBuffer(ibDesc);
    rhi->updateBuffer(indexBuffer, quadIndices.data(), 0, ibDesc.size);

    // Reserve vertex storage
    vertices.reserve(MaxVertices);
    indices.reserve(MaxIndices);

    // TODO: Create batch rendering shaders and pipeline
}

void Renderer::BatchRenderer::shutdown(RHI* rhi) {
    if (vertexBuffer.isValid()) {
        rhi->destroyBuffer(vertexBuffer);
    }
    if (indexBuffer.isValid()) {
        rhi->destroyBuffer(indexBuffer);
    }
    if (pipeline.isValid()) {
        rhi->destroyPipeline(pipeline);
    }
    if (vertexShader.isValid()) {
        rhi->destroyShader(vertexShader);
    }
    if (fragmentShader.isValid()) {
        rhi->destroyShader(fragmentShader);
    }
}

void Renderer::BatchRenderer::flush(RHI* rhi, const glm::mat4& viewProj) {
    if (quadCount == 0) return;

    // Upload vertex data
    rhi->updateBuffer(vertexBuffer, vertices.data(), 0, sizeof(Vertex2D) * vertices.size());

    // TODO: Bind pipeline, set uniforms, draw
    // rhi->bindPipeline(pipeline);
    // rhi->setVertexBytes(&viewProj, sizeof(glm::mat4), 0);
    // rhi->bindVertexBuffer(vertexBuffer);
    // rhi->bindIndexBuffer(indexBuffer);
    // rhi->drawIndexed(quadCount * 6);

    drawCalls++;
    totalQuads += quadCount;

    // Reset for next batch
    reset();
}

void Renderer::BatchRenderer::reset() {
    vertices.clear();
    indices.clear();
    quadCount = 0;
}

void Renderer::BatchRenderer::addQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, int entityID) {
    if (quadCount >= MaxQuads) {
        // Auto-flush when full
        // TODO: flush needs viewProj which we don't have here
        // For now, just skip
        return;
    }

    // Create quad vertices (centered)
    glm::vec2 halfSize = size * 0.5f;

    Vertex2D v0, v1, v2, v3;
    v0.position = position + glm::vec3(-halfSize.x, -halfSize.y, 0.0f);
    v1.position = position + glm::vec3( halfSize.x, -halfSize.y, 0.0f);
    v2.position = position + glm::vec3( halfSize.x,  halfSize.y, 0.0f);
    v3.position = position + glm::vec3(-halfSize.x,  halfSize.y, 0.0f);

    v0.color = v1.color = v2.color = v3.color = color;

    v0.texCoord = glm::vec2(0, 0);
    v1.texCoord = glm::vec2(1, 0);
    v2.texCoord = glm::vec2(1, 1);
    v3.texCoord = glm::vec2(0, 1);

    v0.texIndex = v1.texIndex = v2.texIndex = v3.texIndex = 0.0f;
    v0.entityID = v1.entityID = v2.entityID = v3.entityID = entityID;

    vertices.push_back(v0);
    vertices.push_back(v1);
    vertices.push_back(v2);
    vertices.push_back(v3);

    quadCount++;
}

void Renderer::BatchRenderer::addQuad(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    if (quadCount >= MaxQuads) {
        return;
    }

    // Extract quad corners from transform matrix
    glm::vec4 positions[4] = {
        transform * glm::vec4(-0.5f, -0.5f, 0.0f, 1.0f),
        transform * glm::vec4( 0.5f, -0.5f, 0.0f, 1.0f),
        transform * glm::vec4( 0.5f,  0.5f, 0.0f, 1.0f),
        transform * glm::vec4(-0.5f,  0.5f, 0.0f, 1.0f),
    };

    for (int i = 0; i < 4; i++) {
        Vertex2D v;
        v.position = glm::vec3(positions[i]) / positions[i].w;
        v.color = color;
        v.texCoord = (i == 0) ? glm::vec2(0, 0) :
                     (i == 1) ? glm::vec2(1, 0) :
                     (i == 2) ? glm::vec2(1, 1) : glm::vec2(0, 1);
        v.texIndex = 0.0f;
        v.entityID = entityID;
        vertices.push_back(v);
    }

    quadCount++;
}

void Renderer::BatchRenderer::addQuad(
    const glm::mat4& transform,
    const glm::vec2* texCoords,
    const glm::vec4& tint,
    int entityID
) {
    // TODO: Implement textured quad with custom tex coords
    addQuad(transform, tint, entityID);
}
