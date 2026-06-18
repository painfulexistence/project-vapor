#include "renderer.hpp"
#include "rhi_vulkan.hpp"
#ifdef __APPLE__
#include "rhi_metal.hpp"
#endif
#include "helper.hpp"
#include "components.hpp"
#include <SDL3/SDL_video.h>
#include <fmt/core.h>
#include <map>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <memory>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#ifdef __APPLE__
#include "backends/imgui_impl_metal.h"
#endif
#include "backends/imgui_impl_vulkan.h"
#include <vulkan/vulkan.h>

#ifdef __APPLE__
// Metal headers for ImGui initialization
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#endif

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
    materialBufferDesc.size = sizeof(Vapor::MaterialData) * MAX_INSTANCES;  // Reserve space for max materials
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
    instanceDataBufferDesc.size = sizeof(Vapor::InstanceData) * MAX_INSTANCES;
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
#ifdef __APPLE__
            case GraphicsBackend::Metal:
                ImGui_ImplMetal_Shutdown();
                ImGui_ImplSDL3_Shutdown();
                break;
#endif
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

MeshId Renderer::registerMesh(const std::vector<Vapor::VertexData>& vertices,
                                    const std::vector<Uint32>& indices) {
    RenderMesh mesh;

    // Create vertex buffer
    if (!vertices.empty()) {
        BufferDesc vbDesc;
        vbDesc.size = vertices.size() * sizeof(Vapor::VertexData);
        vbDesc.usage = BufferUsage::Vertex;
        vbDesc.memoryUsage = MemoryUsage::GPU;
        mesh.vertexBuffer = rhi->createBuffer(vbDesc);
        rhi->updateBuffer(mesh.vertexBuffer, vertices.data(), 0, vbDesc.size);
    }

    // Create index buffer
    if (!indices.empty()) {
        BufferDesc ibDesc;
        ibDesc.size = indices.size() * sizeof(Uint32);
        ibDesc.usage = BufferUsage::Index;
        ibDesc.memoryUsage = MemoryUsage::GPU;
        mesh.indexBuffer = rhi->createBuffer(ibDesc);
        rhi->updateBuffer(mesh.indexBuffer, indices.data(), 0, ibDesc.size);
    }

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

    if (materialData.alphaMode == Vapor::AlphaMode::BLEND) {
        flags |= ALPHA_BLEND;
    }

    if (materialData.doubleSided) {
        flags |= DOUBLE_SIDED;
    }

    material.flags = static_cast<MaterialFlags>(flags);

    // Create parameter buffer
    BufferDesc paramBufferDesc;
    paramBufferDesc.size = sizeof(Vapor::MaterialData);
    paramBufferDesc.usage = BufferUsage::Uniform;
    paramBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    material.parameterBuffer = rhi->createBuffer(paramBufferDesc);

    // Upload initial parameters
    Vapor::MaterialData params;
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

    rhi->updateBuffer(material.parameterBuffer, &params, 0, sizeof(Vapor::MaterialData));

    MaterialId id = static_cast<MaterialId>(materials.size());
    materials.push_back(material);
    return id;
}

TextureId Renderer::registerTexture(const std::shared_ptr<Vapor::Image>& image) {
    return getOrCreateTexture(image);
}

// ============================================================================
// Frame Rendering
// ============================================================================

void Renderer::beginFrame(const CameraRenderData& camera) {
    // Process any pending screenshots from previous frames
    processPendingScreenshots();

    // Begin RHI frame (get drawable, create command buffer)
    rhi->beginFrame();

    // Call ImGui backend NewFrame (matching old renderer behavior)
    // This must be called before ImGui::NewFrame() in main.cpp
    ImGui_ImplSDL3_NewFrame();
    // We need to create a render pass descriptor with swapchain texture
#ifdef __APPLE__
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
#endif
    if (backend == GraphicsBackend::Vulkan) {
        ImGui_ImplVulkan_NewFrame();
    }

    currentCamera = camera;
    frameDrawables.clear();
    visibleDrawables.clear();
    directionalLights.clear();
    pointLights.clear();

    // Set up batch renderers for auto-flushing
    // 2D uses orthographic projection
    glm::mat4 orthoProj = glm::ortho(
        0.0f, static_cast<float>(rhi->getSwapchainWidth()),
        static_cast<float>(rhi->getSwapchainHeight()), 0.0f,
        -1.0f, 1.0f
    );
    batch2D.beginBatch(rhi.get(), orthoProj);

    // 3D uses camera's view-projection
    glm::mat4 viewProj = camera.proj * camera.view;
    batch3D.beginBatch(rhi.get(), viewProj);

    fmt::print("beginFrame: camera position=({}, {}, {}), frameDrawables cleared\n",
               camera.position.x, camera.position.y, camera.position.z);
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
#ifdef __APPLE__
            case GraphicsBackend::Metal: {
                void* cmdBuffer = rhi->getBackendCommandBuffer();
                if (cmdBuffer) {
                    // Create ImGui render pass (load existing content, don't clear)
                    RenderPassDesc imguiPassDesc;
                    imguiPassDesc.colorAttachments.push_back(TextureHandle{0});  // Use swapchain
                    imguiPassDesc.loadColor.push_back(true);  // Load (don't clear, render on top)
                    imguiPassDesc.depthAttachment = TextureHandle{0};  // Use default depth
                    imguiPassDesc.loadDepth = true;  // Load (don't clear)

                    fmt::print("About to begin ImGui render pass\n");
                    rhi->beginRenderPass(imguiPassDesc);
                    fmt::print("ImGui render pass begun\n");

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
#endif
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

    // Batch draws are now flushed at the end of mainRenderPass
    // Disable auto-flushing until next beginFrame
    batch2D.canAutoFlush = false;
    batch3D.canAutoFlush = false;

    // Process screenshot request (before ending frame so command buffer is still active)
    if (screenshotRequested) {
        Uint32 width, height;
        BufferHandle screenshotBuffer = rhi->copySwapchainToBuffer(width, height);

        if (screenshotBuffer.isValid()) {
            PendingScreenshot pending;
            pending.buffer = screenshotBuffer;
            pending.callback = screenshotCallback;
            pending.width = width;
            pending.height = height;
            pending.frameIndex = frameNumber;
            pendingScreenshots.push_back(pending);
        }

        screenshotRequested = false;
        screenshotCallback = nullptr;
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
    Frustum frustum = extractFrustum(currentCamera.proj * currentCamera.view);

    Uint32 culledCount = 0;
    for (Uint32 i = 0; i < frameDrawables.size(); ++i) {
        const Drawable& d = frameDrawables[i];
        if (frustum.isBoxVisible(d.aabbMin, d.aabbMax)) {
            visibleDrawables.push_back(i);
        } else {
            culledCount++;
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

    // Update material data buffer (array of all materials for shader binding 1)
    if (!materials.empty()) {
        std::vector<Vapor::MaterialData> materialDataArray;
        materialDataArray.reserve(materials.size());
        for (const auto& mat : materials) {
            Vapor::MaterialData data;
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
                          materialDataArray.size() * sizeof(Vapor::MaterialData));
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
    std::vector<Vapor::InstanceData> instanceData;
    instanceData.reserve(visibleDrawables.size());
    Uint32 nonIdentityCount = 0;
    Uint32 instanceID = 0;
    for (Uint32 drawableIdx : visibleDrawables) {
        const Drawable& drawable = frameDrawables[drawableIdx];
        const RenderMesh& mesh = meshes[drawable.mesh];

        Vapor::InstanceData instance;
        instance.model = drawable.transform;
        instance.color = drawable.color;
        instance.vertexOffset = 0;  // Each mesh has its own buffer
        instance.indexOffset = 0;   // Each mesh has its own buffer
        instance.vertexCount = mesh.vertexCount;
        instance.indexCount = mesh.indexCount;
        instance.materialID = drawable.material;
        instance.primitiveMode = Vapor::PrimitiveMode::TRIANGLES;
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
                          instanceData.size() * sizeof(Vapor::InstanceData));
    }
}

void Renderer::mainRenderPass() {
    fmt::print("mainRenderPass: visibleDrawables.size()={}, mainPipeline.isValid()={}\n",
               visibleDrawables.size(), mainPipeline.isValid());

    if (!mainPipeline.isValid()) {
        fmt::print("mainRenderPass: Invalid pipeline, skipping draw\n");
        return;
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
        fmt::print("About to begin geometry render pass\n");
        rhi->beginRenderPass(renderPassDesc);
        fmt::print("Geometry render pass begun\n");

    // Bind pipeline
    rhi->bindPipeline(mainPipeline);

    // Bind common buffers (same for all drawables).
    // IMPORTANT: vertex and fragment shaders have INDEPENDENT buffer index
    // namespaces (Metal). We must bind per-stage, otherwise a fragment binding
    // would clobber the vertex binding at the same index (e.g. fragment lights
    // at index 0 overwriting the vertex camera at index 0), corrupting the
    // vertex transform and producing scattered-line artifacts.
    // Vertex buffers:
    // Binding 0: CameraData
    rhi->setVertexBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    // Binding 1: MaterialData array (all materials)
    rhi->setVertexBuffer(1, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
    // Binding 2: InstanceData array
    // Note: We only update the buffer with visible drawables, so the size is visibleDrawables.size()
    rhi->setVertexBuffer(2, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * visibleDrawables.size());

    // Fragment buffers (separate index namespace from vertex bindings):
    // Fragment binding 0: DirectionalLights
    if (directionalLightBuffer.isValid() && !directionalLights.empty()) {
        rhi->setFragmentBuffer(0, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
    }
    // Fragment binding 1: PointLights
    if (pointLightBuffer.isValid() && !pointLights.empty()) {
        rhi->setFragmentBuffer(1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
    }
    // Fragment binding 2: Clusters (if we have cluster buffer - skip for now)
    // Fragment binding 3: CameraData (for fragment shader)
    rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));

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
            if (mesh.vertexBuffer.isValid()) {
                rhi->bindVertexBuffer(mesh.vertexBuffer, 3, 0);
            }

            // Set instance ID (binding 4 for Metal shader)
            rhi->setVertexBytes(&correctInstanceID, sizeof(Uint32), 4);

            // Draw
            if (mesh.indexBuffer.isValid()) {
                rhi->bindIndexBuffer(mesh.indexBuffer, 0);
                rhi->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
            } else if (mesh.vertexBuffer.isValid()) {
                rhi->draw(mesh.vertexCount, 1, 0, 0);
            }
        }
    }

    // Flush batch renders (2D/3D quads, lines) before ending the pass
    flush3D();
    flush2D();

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
    fmt::print("About to begin main/post-process render pass\n");
    rhi->beginRenderPass(renderPassDesc);
    fmt::print("Main/post-process render pass begun\n");

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
        vertShaderCode = readFile("shaders/TBN.vert.spv");
        fragShaderCode = readFile("shaders/PBRNormalMapped.frag.spv");
    } else if (backend == GraphicsBackend::Metal) {
        vertShaderCode = readFile("shaders/3d_pbr_normal_mapped.metal");
        fragShaderCode = readFile("shaders/3d_pbr_normal_mapped.metal");
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
    vertexLayout.stride = sizeof(Vapor::VertexData);
    vertexLayout.attributes = {
        {0, PixelFormat::RGBA32_FLOAT, offsetof(Vapor::VertexData, position)},  // Position (vec3)
        {1, PixelFormat::RGBA32_FLOAT, offsetof(Vapor::VertexData, uv)},        // UV (vec2)
        {2, PixelFormat::RGBA32_FLOAT, offsetof(Vapor::VertexData, normal)},    // Normal (vec3)
        {3, PixelFormat::RGBA32_FLOAT, offsetof(Vapor::VertexData, tangent)}   // Tangent (vec4)
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

TextureId Renderer::getOrCreateTexture(const std::shared_ptr<Vapor::Image>& image) {
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
#ifdef __APPLE__
        case GraphicsBackend::Metal:
            rhi = std::unique_ptr<RHI>(createRHIMetal());
            break;
#endif
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
#ifdef __APPLE__
        case GraphicsBackend::Metal: {
            ImGui_ImplSDL3_InitForMetal(window);
            void* device = rhi->getBackendDevice();
            if (device) {
                ImGui_ImplMetal_Init(static_cast<MTL::Device*>(device));
            }
            break;
        }
#endif
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

    // Create Renderer and transfer RHI ownership.
    // initialize() may throw if GPU resource setup fails (shader compilation,
    // render-target creation, unsupported formats). Honor createRenderer()'s
    // nullptr-on-failure contract instead of letting the exception escape, so
    // callers (and headless CI tests) can degrade gracefully.
    try {
        auto renderer = std::make_unique<Renderer>();
        renderer->initialize(std::move(rhi), backend);
        return renderer;
    } catch (const std::exception& e) {
        fmt::print(stderr, "createRenderer: initialization failed: {}\n", e.what());
        return nullptr;
    }
}

// ============================================================================
// Scene/ECS Integration
// ============================================================================

void Renderer::stage(std::shared_ptr<Scene> scene) {
    if (!scene) return;

    for (auto& mesh : scene->stagedMeshes) {
        if (!mesh) continue;
        
        // Register mesh if not already registered
        if (mesh->renderMeshId == UINT32_MAX) {
            mesh->renderMeshId = registerMesh(mesh->vertices, mesh->indices);
        }
        
        // Register material if not already registered
        if (mesh->material) {
            if (mesh->material->rendererMaterialId == UINT32_MAX) {
                MaterialDataInput matData;
                matData.baseColorFactor = mesh->material->baseColorFactor;
                matData.normalScale = mesh->material->normalScale;
                matData.metallicFactor = mesh->material->metallicFactor;
                matData.roughnessFactor = mesh->material->roughnessFactor;
                matData.occlusionStrength = mesh->material->occlusionStrength;
                matData.emissiveFactor = mesh->material->emissiveFactor;
                matData.emissiveStrength = mesh->material->emissiveStrength;
                matData.subsurface = mesh->material->subsurface;
                matData.specular = mesh->material->specular;
                matData.specularTint = mesh->material->specularTint;
                matData.anisotropic = mesh->material->anisotropic;
                matData.sheen = mesh->material->sheen;
                matData.sheenTint = mesh->material->sheenTint;
                matData.clearcoat = mesh->material->clearcoat;
                matData.clearcoatGloss = mesh->material->clearcoatGloss;
                matData.alphaMode = mesh->material->alphaMode;
                matData.alphaCutoff = mesh->material->alphaCutoff;
                matData.doubleSided = mesh->material->doubleSided;
                
                matData.albedoMap = mesh->material->albedoMap;
                matData.normalMap = mesh->material->normalMap;
                matData.metallicMap = mesh->material->metallicMap;
                matData.roughnessMap = mesh->material->roughnessMap;
                matData.emissiveMap = mesh->material->emissiveMap;
                matData.occlusionMap = mesh->material->occlusionMap;
                
                mesh->material->rendererMaterialId = registerMaterial(matData);
            }
            mesh->renderMaterialId = mesh->material->rendererMaterialId;
        } else {
            mesh->renderMaterialId = INVALID_MATERIAL_ID;
        }
    }
    
    fmt::print("Scene staged with {} meshes\n", scene->stagedMeshes.size());
}

void Renderer::draw(std::shared_ptr<Scene> scene, Camera& camera) {
    if (!scene) return;

    currentCamera.proj = camera.getProjMatrix();
    currentCamera.view = camera.getViewMatrix();
    currentCamera.invProj = glm::inverse(currentCamera.proj);
    currentCamera.invView = glm::inverse(currentCamera.view);
    currentCamera.nearPlane = camera.near();
    currentCamera.farPlane = camera.far();
    currentCamera.position = camera.getEye();

    // Collect drawables from scene
    collectDrawables(scene);

    // TODO: Collect lights from scene
    // scene->collectLights(directionalLights, pointLights);

    // Render
    render();
}

void Renderer::draw(entt::registry& registry, std::shared_ptr<Scene> scene, Camera& camera) {
    if (!scene) return;

    currentCamera.proj = camera.getProjMatrix();
    currentCamera.view = camera.getViewMatrix();
    currentCamera.invProj = glm::inverse(currentCamera.proj);
    currentCamera.invView = glm::inverse(currentCamera.view);
    currentCamera.nearPlane = camera.near();
    currentCamera.farPlane = camera.far();
    currentCamera.position = camera.getEye();

    // Collect drawables from ECS
    collectDrawables(registry, scene);

    // TODO: Collect lights from ECS
    // collectLights(registry);

    // Render
    render();
}

void Renderer::collectDrawables(std::shared_ptr<Scene> scene) {
    // Only used for backwards compatibility or manual staging
    // Usually collectDrawables(registry, scene) is used for ECS
}

void Renderer::collectDrawables(entt::registry& registry, std::shared_ptr<Scene> scene) {
    // Collect renderables from ECS
    auto view = registry.view<Vapor::TransformComponent, Vapor::MeshRendererComponent>();

    for (auto entity : view) {
        auto& transform = view.get<Vapor::TransformComponent>(entity);
        auto& meshRenderer = view.get<Vapor::MeshRendererComponent>(entity);

        if (!meshRenderer.visible) continue;

        for (auto& mesh : meshRenderer.meshes) {
            if (!mesh || mesh->renderMeshId == UINT32_MAX) continue;

            // Create drawable from mesh
            Drawable drawable;
            drawable.mesh = mesh->renderMeshId;
            drawable.material = mesh->renderMaterialId;
            drawable.transform = transform.worldTransform;
            
            // Transform AABB to world space
            glm::vec3 minAABB = mesh->localAABBMin;
            glm::vec3 maxAABB = mesh->localAABBMax;
            
            glm::vec3 corners[8] = {
                glm::vec3(minAABB.x, minAABB.y, minAABB.z),
                glm::vec3(maxAABB.x, minAABB.y, minAABB.z),
                glm::vec3(minAABB.x, maxAABB.y, minAABB.z),
                glm::vec3(maxAABB.x, maxAABB.y, minAABB.z),
                glm::vec3(minAABB.x, minAABB.y, maxAABB.z),
                glm::vec3(maxAABB.x, minAABB.y, maxAABB.z),
                glm::vec3(minAABB.x, maxAABB.y, maxAABB.z),
                glm::vec3(maxAABB.x, maxAABB.y, maxAABB.z)
            };
            
            glm::vec3 worldMin(std::numeric_limits<float>::max());
            glm::vec3 worldMax(std::numeric_limits<float>::lowest());
            
            for (int i = 0; i < 8; i++) {
                glm::vec4 worldPos = transform.worldTransform * glm::vec4(corners[i], 1.0f);
                glm::vec3 p = glm::vec3(worldPos) / worldPos.w;
                worldMin = glm::min(worldMin, p);
                worldMax = glm::max(worldMax, p);
            }
            
            drawable.aabbMin = worldMin;
            drawable.aabbMax = worldMax;

            submitDrawable(drawable);
        }
    }

    // TODO: Collect sprites
    auto spriteView = registry.view<Vapor::TransformComponent, Vapor::SpriteComponent>();
    for (auto entity : spriteView) {
        auto& transform = spriteView.get<Vapor::TransformComponent>(entity);
        auto& sprite = spriteView.get<Vapor::SpriteComponent>(entity);

        if (!sprite.visible) continue;

        // Use batch rendering for sprites
        glm::vec2 position(transform.position.x, transform.position.y);
        glm::vec4 color = sprite.tint;

        // TODO: Get texture from atlas
        // TextureHandle tex = getAtlasTexture(sprite.atlas);
        // drawQuad2D(position, sprite.size, tex, color);

        // For now, just draw colored quads
        drawQuad2D(position, sprite.size, color);
    }
}

// ============================================================================
// Screenshot API
// ============================================================================

void Renderer::readPixelsAsync(ScreenshotCallback callback) {
    screenshotCallback = callback;
    screenshotRequested = true;
}

void Renderer::processPendingScreenshots() {
    // Wait for GPU to finish (ensures screenshot buffer is ready)
    // In a production implementation, we'd use per-frame fences
    // For now, simple wait ensures correctness
    if (!pendingScreenshots.empty()) {
        rhi->waitIdle();
    }

    for (auto it = pendingScreenshots.begin(); it != pendingScreenshots.end();) {
        PendingScreenshot& pending = *it;

        // Map buffer and read pixels
        void* data = rhi->mapBuffer(pending.buffer);
        if (data) {
            GpuImageData imageData;
            imageData.width = pending.width;
            imageData.height = pending.height;
            imageData.channelCount = 4; // RGBA/BGRA
            size_t dataSize = pending.width * pending.height * 4;
            imageData.data.resize(dataSize);
            std::memcpy(imageData.data.data(), data, dataSize);

            rhi->unmapBuffer(pending.buffer);

            // Call callback
            if (pending.callback) {
                pending.callback(imageData);
            }
        }

        // Cleanup
        rhi->destroyBuffer(pending.buffer);
        it = pendingScreenshots.erase(it);
    }
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
    batch2D.init(rhi.get(), backend, false, textures[defaultWhiteTexture].handle);

    // Initialize batch3D
    batch3D.init(rhi.get(), backend, true, textures[defaultWhiteTexture].handle);
}

void Renderer::shutdownBatchRendering() {
    batch2D.shutdown(rhi.get());
    batch3D.shutdown(rhi.get());
}

void Renderer::flush2D() {
    if (batch2D.quadCount > 0) {
        // TODO: Get view-projection matrix for 2D (orthographic)
        glm::mat4 viewProj = glm::orthoZO(
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
        glm::mat4 viewProj = currentCamera.proj * currentCamera.view;
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
    // Implement line as a rotated quad
    glm::vec2 dir = p1 - p0;
    float length = glm::length(dir);
    if (length < 0.0001f) return; // Degenerate line

    dir /= length; // Normalize
    glm::vec2 perp(-dir.y, dir.x); // Perpendicular vector

    // Calculate half-thickness offset
    glm::vec2 offset = perp * (thickness * 0.5f);

    // Create quad corners
    glm::vec2 corner0 = p0 - offset;
    glm::vec2 corner1 = p0 + offset;
    glm::vec2 corner2 = p1 + offset;
    glm::vec2 corner3 = p1 - offset;

    // Build transform matrix for the line quad
    glm::vec2 center = (p0 + p1) * 0.5f;
    float angle = std::atan2(dir.y, dir.x);

    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
    transform = glm::rotate(transform, angle, glm::vec3(0, 0, 1));
    transform = glm::scale(transform, glm::vec3(length, thickness, 1.0f));

    drawQuad2D(transform, color);
}

void Renderer::drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness) {
    // Similar to 2D but in 3D space
    glm::vec3 dir = p1 - p0;
    float length = glm::length(dir);
    if (length < 0.0001f) return;

    dir /= length;

    // Find perpendicular vector (use cross product with up vector)
    glm::vec3 up = glm::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 right = glm::normalize(glm::cross(dir, up));

    // Calculate quad corners with thickness
    glm::vec3 offset = right * (thickness * 0.5f);

    glm::vec3 center = (p0 + p1) * 0.5f;

    // Build transform that orients the quad along the line
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), center);

    // Create rotation to align with line direction
    glm::vec3 forward = dir;
    glm::vec3 worldUp = glm::vec3(0, 1, 0);
    if (glm::abs(glm::dot(forward, worldUp)) > 0.99f) {
        worldUp = glm::vec3(0, 0, 1);
    }
    right = glm::normalize(glm::cross(worldUp, forward));
    up = glm::cross(forward, right);

    glm::mat4 rotation(1.0f);
    rotation[0] = glm::vec4(right, 0);
    rotation[1] = glm::vec4(up, 0);
    rotation[2] = glm::vec4(forward, 0);

    transform = transform * rotation;
    transform = glm::scale(transform, glm::vec3(thickness, thickness, length));

    drawQuad3D(transform, color);
}

// Shape drawing
void Renderer::drawRect2D(
    const glm::vec2& position,
    const glm::vec2& size,
    const glm::vec4& color,
    float thickness
) {
    // Draw 4 lines to form rectangle
    glm::vec2 halfSize = size * 0.5f;
    glm::vec2 topLeft = position - halfSize;
    glm::vec2 topRight = position + glm::vec2(halfSize.x, -halfSize.y);
    glm::vec2 bottomRight = position + halfSize;
    glm::vec2 bottomLeft = position + glm::vec2(-halfSize.x, halfSize.y);

    drawLine2D(topLeft, topRight, color, thickness);
    drawLine2D(topRight, bottomRight, color, thickness);
    drawLine2D(bottomRight, bottomLeft, color, thickness);
    drawLine2D(bottomLeft, topLeft, color, thickness);
}

void Renderer::drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    // Draw circle outline using line segments
    if (segments < 3) segments = 32;

    float angleStep = glm::two_pi<float>() / segments;

    for (int i = 0; i < segments; ++i) {
        float angle0 = i * angleStep;
        float angle1 = (i + 1) * angleStep;

        glm::vec2 p0 = center + glm::vec2(std::cos(angle0), std::sin(angle0)) * radius;
        glm::vec2 p1 = center + glm::vec2(std::cos(angle1), std::sin(angle1)) * radius;

        drawLine2D(p0, p1, color, 1.0f);
    }
}

void Renderer::drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    // Draw filled circle using triangle fan (rendered as quads)
    if (segments < 3) segments = 32;

    float angleStep = glm::two_pi<float>() / segments;

    // Draw as quads approximating triangles
    for (int i = 0; i < segments; ++i) {
        float angle0 = i * angleStep;
        float angle1 = (i + 1) * angleStep;

        glm::vec2 p0 = center;
        glm::vec2 p1 = center + glm::vec2(std::cos(angle0), std::sin(angle0)) * radius;
        glm::vec2 p2 = center + glm::vec2(std::cos(angle1), std::sin(angle1)) * radius;

        // Create a thin triangle as a degenerate quad
        // Calculate midpoint for smoother rendering
        glm::vec2 midPoint = (p1 + p2) * 0.5f;

        // Draw as a filled triangle by creating 3 vertices
        // We'll approximate this by drawing a very thin quad from center to edge
        drawTriangleFilled2D(p0, p1, p2, color);
    }
}

void Renderer::drawTriangle2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    // Draw triangle outline using 3 lines
    drawLine2D(p0, p1, color, 1.0f);
    drawLine2D(p1, p2, color, 1.0f);
    drawLine2D(p2, p0, color, 1.0f);
}

void Renderer::drawTriangleFilled2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color) {
    // Draw filled triangle using barycentric coordinates
    // We'll approximate by drawing a quad that covers the triangle
    // For a proper triangle, we need to add triangle support to the batch renderer
    // For now, draw three quads from center to each edge

    glm::vec2 center = (p0 + p1 + p2) / 3.0f;

    // Calculate small quads to approximate the triangle
    // This is a simplified approach - ideally we'd add proper triangle rendering

    // Create transformation matrix for a quad that covers the triangle area
    glm::vec2 min = glm::min(glm::min(p0, p1), p2);
    glm::vec2 max = glm::max(glm::max(p0, p1), p2);
    glm::vec2 size = max - min;
    glm::vec2 pos = (min + max) * 0.5f;

    // For now, just draw a quad that approximates the triangle
    // A proper implementation would tessellate or use a geometry shader
    drawQuad2D(pos, size, color);
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
    if (!fontManager) return;

    Font* fontData = fontManager->getFont(font);
    if (!fontData) return;

    TextureHandle fontTexture = fontManager->getFontTexture(font);
    if (!fontTexture.isValid()) {
        // Need to create texture from atlas data
        const FontManager::AtlasData* atlasData = fontManager->getAtlasData(font);
        if (atlasData && !atlasData->rgbaData.empty()) {
            TextureDesc texDesc;
            texDesc.width = atlasData->width;
            texDesc.height = atlasData->height;
            texDesc.format = PixelFormat::RGBA8_UNORM;
            texDesc.usage = TextureUsage::Sampled;
            fontTexture = rhi->createTexture(texDesc);
            rhi->updateTexture(fontTexture, atlasData->rgbaData.data(), atlasData->rgbaData.size());
            fontManager->setFontTextureHandle(font, fontTexture);
        } else {
            return; // No atlas data available
        }
    }

    // Draw each character as a textured quad
    float cursorX = position.x;
    float cursorY = position.y;

    for (char c : text) {
        if (c == '\n') {
            cursorX = position.x;
            cursorY += fontData->lineHeight * scale;
            continue;
        }

        const Glyph* glyph = fontManager->getGlyph(font, static_cast<int>(c));
        if (!glyph) continue;

        // Calculate glyph quad position and size
        float xPos = cursorX + glyph->xOffset * scale;
        float yPos = cursorY + glyph->yOffset * scale;
        float width = glyph->width * scale;
        float height = glyph->height * scale;

        // Build transform matrix for this glyph
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(xPos + width * 0.5f, yPos + height * 0.5f, 0.0f));
        transform = glm::scale(transform, glm::vec3(width, height, 1.0f));

        // UV coordinates from glyph
        glm::vec2 texCoords[4] = {
            glm::vec2(glyph->u0, glyph->v0),
            glm::vec2(glyph->u1, glyph->v0),
            glm::vec2(glyph->u1, glyph->v1),
            glm::vec2(glyph->u0, glyph->v1)
        };

        // Draw the glyph quad with texture coordinates
        batch2D.addQuad(transform, texCoords, color);

        // Advance cursor
        cursorX += glyph->advance * scale;
    }
}

void Renderer::drawText3D(
    FontHandle font,
    const std::string& text,
    const glm::vec3& worldPosition,
    float scale,
    const glm::vec4& color
) {
    if (!fontManager) return;

    Font* fontData = fontManager->getFont(font);
    if (!fontData) return;

    TextureHandle fontTexture = fontManager->getFontTexture(font);
    if (!fontTexture.isValid()) {
        // Need to create texture from atlas data
        const FontManager::AtlasData* atlasData = fontManager->getAtlasData(font);
        if (atlasData && !atlasData->rgbaData.empty()) {
            TextureDesc texDesc;
            texDesc.width = atlasData->width;
            texDesc.height = atlasData->height;
            texDesc.format = PixelFormat::RGBA8_UNORM;
            texDesc.usage = TextureUsage::Sampled;
            fontTexture = rhi->createTexture(texDesc);
            rhi->updateTexture(fontTexture, atlasData->rgbaData.data(), atlasData->rgbaData.size());
            fontManager->setFontTextureHandle(font, fontTexture);
        } else {
            return;
        }
    }

    // Calculate text width for centering
    float textWidth = 0.0f;
    for (char c : text) {
        const Glyph* glyph = fontManager->getGlyph(font, static_cast<int>(c));
        if (glyph) {
            textWidth += glyph->advance * scale;
        }
    }

    // Create billboard matrix (faces camera)
    // Extract camera right and up vectors from view matrix
    glm::vec3 cameraRight = glm::vec3(currentCamera.view[0][0], currentCamera.view[1][0], currentCamera.view[2][0]);
    glm::vec3 cameraUp = glm::vec3(currentCamera.view[0][1], currentCamera.view[1][1], currentCamera.view[2][1]);

    // Draw each character as a billboard
    float cursorX = -textWidth * 0.5f; // Center the text
    float cursorY = 0.0f;

    for (char c : text) {
        if (c == '\n') {
            cursorX = -textWidth * 0.5f;
            cursorY -= fontData->lineHeight * scale;
            continue;
        }

        const Glyph* glyph = fontManager->getGlyph(font, static_cast<int>(c));
        if (!glyph) continue;

        // Calculate glyph position in billboard space
        float xOffset = glyph->xOffset * scale;
        float yOffset = glyph->yOffset * scale;
        float width = glyph->width * scale;
        float height = glyph->height * scale;

        // Calculate world position for this glyph
        glm::vec3 glyphCenter = worldPosition
            + cameraRight * (cursorX + xOffset + width * 0.5f)
            + cameraUp * (cursorY + yOffset + height * 0.5f);

        // Build billboard transform
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), glyphCenter);
        // Add right and up vectors to create billboard orientation
        glm::mat4 billboardRotation(1.0f);
        billboardRotation[0] = glm::vec4(cameraRight, 0.0f);
        billboardRotation[1] = glm::vec4(cameraUp, 0.0f);
        billboardRotation[2] = glm::vec4(glm::cross(cameraRight, cameraUp), 0.0f);
        transform = transform * billboardRotation;
        transform = glm::scale(transform, glm::vec3(width, height, 1.0f));

        // UV coordinates from glyph
        glm::vec2 texCoords[4] = {
            glm::vec2(glyph->u0, glyph->v0),
            glm::vec2(glyph->u1, glyph->v0),
            glm::vec2(glyph->u1, glyph->v1),
            glm::vec2(glyph->u0, glyph->v1)
        };

        // Draw the glyph quad with texture coordinates
        batch3D.addQuad(transform, texCoords, color);

        // Advance cursor
        cursorX += glyph->advance * scale;
    }
}

glm::vec2 Renderer::measureText(FontHandle font, const std::string& text, float scale) {
    if (fontManager) {
        return fontManager->measureText(font, text, scale);
    }
    return glm::vec2(0.0f);
}

float Renderer::getFontLineHeight(FontHandle font, float scale) {
    if (fontManager) {
        Font* fontData = fontManager->getFont(font);
        if (fontData) {
            return fontData->lineHeight * scale;
        }
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
    if (target.id >= renderTextures.size()) {
        return; // Invalid handle
    }

    auto& resource = renderTextures[target.id];

    // Save current camera state
    CameraRenderData previousCamera = currentCamera;

    // Set up camera for this render
    CameraRenderData rtCamera;
    rtCamera.proj = camera.getProjMatrix();
    rtCamera.view = camera.getViewMatrix();
    rtCamera.invProj = glm::inverse(rtCamera.proj);
    rtCamera.invView = glm::inverse(rtCamera.view);
    rtCamera.nearPlane = camera.near();
    rtCamera.farPlane = camera.far();
    rtCamera.position = camera.getEye();
    currentCamera = rtCamera;

    // Begin render pass with render texture as target
    RenderPassDesc passDesc;
    passDesc.colorAttachments.push_back(resource.colorTexture);
    passDesc.clearColors.push_back(clearColor);
    passDesc.loadColor.push_back(false); // Clear, don't load

    if (resource.hasDepth && resource.depthTexture.isValid()) {
        passDesc.depthAttachment = resource.depthTexture;
        passDesc.clearDepth = 1.0f;
        passDesc.loadDepth = false; // Clear depth
    }
    fmt::print("About to begin render pass passDesc, depthAttachment.id={}\n", passDesc.depthAttachment.id);
    rhi->beginRenderPass(passDesc);
    fmt::print("Render pass passDesc begun\n");

    // Collect drawables from scene
    frameDrawables.clear();
    visibleDrawables.clear();
    collectDrawables(scene);

    // Perform rendering
    if (!visibleDrawables.empty()) {
        performCulling();
        sortDrawables();
        updateBuffers();

        // Bind pipeline and render
        rhi->bindPipeline(mainPipeline);

        for (uint32_t drawableIdx : visibleDrawables) {
            const Drawable& drawable = frameDrawables[drawableIdx];
            const RenderMesh& mesh = meshes[drawable.mesh];

            // Bind material
            bindMaterial(drawable.material);

            // Bind instance data (transform, color, etc.)
            uint32_t instanceID = drawableToInstanceID[drawableIdx];
            rhi->setVertexBytes(&instanceID, sizeof(uint32_t), 1);

            // Bind mesh buffers
            rhi->bindVertexBuffer(mesh.vertexBuffer, 0, 0);
            rhi->bindIndexBuffer(mesh.indexBuffer, 0);

            // Draw
            rhi->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
        }
    }

    // Flush any batched draws
    flush2D();
    flush3D();

    rhi->endRenderPass();

    // Restore previous camera state
    currentCamera = previousCamera;
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

TextureHandle Renderer::createTexture(const std::shared_ptr<Vapor::Image>& img) {
    TextureDesc desc;
    desc.width = img->width;
    desc.height = img->height;
    desc.format = PixelFormat::RGBA8_UNORM;
    desc.usage = TextureUsage::Sampled;
    desc.mipLevels = 1;

    TextureHandle handle = rhi->createTexture(desc);

    // Upload data
    size_t dataSize = img->width * img->height * img->channelCount;
    rhi->updateTexture(handle, img->byteArray.data(), dataSize);

    return handle;
}

void Renderer::updateTexture(TextureHandle handle, const std::shared_ptr<Vapor::Image>& img) {
    if (!handle.isValid() || !img) return;

    // Re-upload pixel data in place (no GPU reallocation). Caller guarantees the
    // dimensions/channel count match the original createTexture() call.
    size_t dataSize = img->width * img->height * img->channelCount;
    rhi->updateTexture(handle, img->byteArray.data(), dataSize);
}

// ============================================================================
// BatchRenderer Implementation
// ============================================================================

void Renderer::BatchRenderer::init(RHI* rhi, GraphicsBackend backend, bool is3D, TextureHandle defaultTex) {
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

    // Load and create shaders
    std::string vertShaderCode;
    std::string fragShaderCode;

    if (backend == GraphicsBackend::Vulkan) {
        // Load SPIR-V shaders
        vertShaderCode = readFile("shaders/Batch2D.vert.spv");
        fragShaderCode = readFile("shaders/Batch2D.frag.spv");
    } else if (backend == GraphicsBackend::Metal) {
        // Load Metal shader library
        vertShaderCode = readFile("shaders/2d_batch.metal");
        fragShaderCode = vertShaderCode;  // Same file for Metal
    }

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        fmt::print("Warning: Failed to load batch2D shaders\n");
        return;
    }

    // Create vertex shader
    ShaderDesc vertShaderDesc;
    vertShaderDesc.stage = ShaderStage::Vertex;
    vertShaderDesc.code = vertShaderCode.data();
    vertShaderDesc.codeSize = vertShaderCode.size();
    vertShaderDesc.entryPoint = (backend == GraphicsBackend::Metal) ? "batch2d_vertex" : "main";
    vertexShader = rhi->createShader(vertShaderDesc);

    // Create fragment shader
    ShaderDesc fragShaderDesc;
    fragShaderDesc.stage = ShaderStage::Fragment;
    fragShaderDesc.code = fragShaderCode.data();
    fragShaderDesc.codeSize = fragShaderCode.size();
    fragShaderDesc.entryPoint = (backend == GraphicsBackend::Metal) ? "batch2d_fragment" : "main";
    fragmentShader = rhi->createShader(fragShaderDesc);

    // Create pipeline
    PipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.fragmentShader = fragmentShader;

    // Vertex layout (matches Vertex2D struct)
    pipelineDesc.vertexLayout.stride = sizeof(Vertex2D);
    pipelineDesc.vertexLayout.attributes = {
        {0, PixelFormat::RGBA32_FLOAT, offsetof(Vertex2D, position)},   // vec3 position (using RGBA32 for vec3)
        {1, PixelFormat::RGBA32_FLOAT, offsetof(Vertex2D, color)},      // vec4 color
        {2, PixelFormat::RGBA32_FLOAT, offsetof(Vertex2D, texCoord)},   // vec2 texCoord (using RGBA32, only xy used)
        {3, PixelFormat::R32_FLOAT, offsetof(Vertex2D, texIndex)},      // float texIndex
        {4, PixelFormat::R32_FLOAT, offsetof(Vertex2D, entityID)},      // int entityID (as float)
    };

    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.blendMode = BlendMode::AlphaBlend;
    pipelineDesc.depthTest = is3D;  // Enable depth test for 3D, disable for 2D
    pipelineDesc.depthWrite = is3D;
    pipelineDesc.cullMode = CullMode::None;  // No culling for 2D quads

    pipeline = rhi->createPipeline(pipelineDesc);

    fmt::print("BatchRenderer initialized ({} mode)\n", is3D ? "3D" : "2D");
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

void Renderer::BatchRenderer::beginBatch(RHI* rhi, const glm::mat4& viewProj) {
    currentRHI = rhi;
    currentViewProj = viewProj;
    canAutoFlush = true;
}

void Renderer::BatchRenderer::flush(RHI* rhi, const glm::mat4& viewProj) {
    if (quadCount == 0) return;

    // Upload vertex data
    rhi->updateBuffer(vertexBuffer, vertices.data(), 0, sizeof(Vertex2D) * vertices.size());

    // Bind pipeline
    rhi->bindPipeline(pipeline);

    // Set projection matrix uniform (set 0, binding 0)
    rhi->setVertexBytes(&viewProj, sizeof(glm::mat4), 0);

    // Bind vertex and index buffers
    rhi->bindVertexBuffer(vertexBuffer, 0, 0);
    rhi->bindIndexBuffer(indexBuffer, 0);

    // Bind white texture for now (texture array binding can be added later)
    // rhi->setTexture(1, 0, whiteTexture, defaultSampler);

    // Draw indexed
    uint32_t indexCount = quadCount * 6;  // 6 indices per quad
    rhi->drawIndexed(indexCount, 1, 0, 0, 0);

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
        if (canAutoFlush && currentRHI) {
            flush(currentRHI, currentViewProj);
        } else {
            return; // Can't flush, skip this quad
        }
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
        // Auto-flush when full
        if (canAutoFlush && currentRHI) {
            flush(currentRHI, currentViewProj);
        } else {
            return; // Can't flush, skip this quad
        }
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
    if (quadCount >= MaxQuads) {
        // Auto-flush when full
        if (canAutoFlush && currentRHI) {
            flush(currentRHI, currentViewProj);
        } else {
            return; // Can't flush, skip this quad
        }
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
        v.color = tint;
        v.texCoord = texCoords ? texCoords[i] : glm::vec2((i & 1), (i >> 1));
        v.texIndex = 0.0f; // TODO: Support multiple textures
        v.entityID = entityID;
        vertices.push_back(v);
    }

    quadCount++;
}
