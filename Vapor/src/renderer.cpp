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
// Metal headers for ImGui initialization (declarations only). The metal-cpp
// implementation is emitted by exactly ONE translation unit — renderer_metal.cpp.
// Defining *_PRIVATE_IMPLEMENTATION here too made three TUs emit it → duplicate
// symbols at link. Keep only the include for declarations.
#include <Metal/Metal.hpp>
#endif

// ============================================================================
// Initialization
// ============================================================================

void Renderer::initialize(std::unique_ptr<RHI> rhiPtr, GraphicsBackend backendType) {
    rhi = std::move(rhiPtr);
    backend = backendType;

    // Snapshot backend capabilities; the render graph uses them to skip
    // passes the backend can't run (e.g. raytracing passes on Vulkan).
    capabilities = rhi->getCapabilities();
    setupDefaultRenderGraph();

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

    // Shader-contract buffers (clusters / rect lights / PSSM), neutral-filled.
    // See the "Full PBR shader contract" note in renderer.hpp.
    {
        BufferDesc clusterDesc;
        clusterDesc.size = sizeof(Vapor::Cluster) * clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;
        clusterDesc.usage = BufferUsage::Storage;
        clusterDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        clusterBuffer = rhi->createBuffer(clusterDesc);

        BufferDesc rectDesc;
        rectDesc.size = sizeof(Vapor::RectLight) * maxRectLights;
        rectDesc.usage = BufferUsage::Storage;
        rectDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        rectLightBuffer = rhi->createBuffer(rectDesc);
        std::vector<Vapor::RectLight> zeroRects(maxRectLights, Vapor::RectLight{});
        rhi->updateBuffer(rectLightBuffer, zeroRects.data(), 0, rectDesc.size);

        BufferDesc pssmDesc;
        pssmDesc.size = sizeof(PSSMRenderData);
        pssmDesc.usage = BufferUsage::Uniform;
        pssmDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        pssmDataBuffer = rhi->createBuffer(pssmDesc);
        PSSMRenderData neutralPSSM;
        rhi->updateBuffer(pssmDataBuffer, &neutralPSSM, 0, sizeof(neutralPSSM));

        BufferDesc atmoDesc;
        atmoDesc.size = sizeof(AtmosphereRenderData);
        atmoDesc.usage = BufferUsage::Uniform;
        atmoDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        atmosphereDataBuffer = rhi->createBuffer(atmoDesc);
        AtmosphereRenderData atmoDefaults;
        rhi->updateBuffer(atmosphereDataBuffer, &atmoDefaults, 0, sizeof(atmoDefaults));
    }

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
        // GPU may still be executing the last frame; ImGui backend shutdown
        // and resource destruction below require it to be finished.
        rhi->waitIdle();

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
        if (shadowSampler.isValid()) {
            rhi->destroySampler(shadowSampler);
        }
        if (clampSampler.isValid()) {
            rhi->destroySampler(clampSampler);
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
    rectLights.clear();

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

    // Execute the frame's passes. Which passes run is decided by the graph:
    // disabled passes and passes whose capability requirements the backend
    // doesn't meet (PassFlags vs RHICapabilities) are skipped — no backend
    // checks here.
    renderGraph.execute(*this, capabilities);

    // Frame stats for the Engine window (read next frame, before the clear)
    lastFrameStats.totalDrawables = static_cast<Uint32>(frameDrawables.size());
    lastFrameStats.visibleDrawables = static_cast<Uint32>(visibleDrawables.size());
    lastFrameStats.directionalLights = static_cast<Uint32>(directionalLights.size());
    lastFrameStats.pointLights = static_cast<Uint32>(pointLights.size());
}

// The engine's default frame composition. Gameplay code is free to modify
// this through getRenderGraph(): append custom CallbackPass lambdas, remove
// or reorder built-ins, or clear() and rebuild the frame from scratch.
void Renderer::setupDefaultRenderGraph() {
    renderGraph.clear();

    // Geometry prep + lighting passes (raytraced passes only run on backends
    // that report RHICapabilities::raytracing — i.e. Metal today).
    renderGraph.addPass("BuildAccelStructures",
        [](Renderer& r) { r.buildAccelerationStructures(); }, PassFlags::RequiresRaytracing);
    renderGraph.addPass("PrePass",
        [](Renderer& r) { r.prePass(); });
    renderGraph.addPass("NormalResolve",
        [](Renderer& r) { r.normalResolvePass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("TileCulling",
        [](Renderer& r) { r.tileCullingPass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("RaytraceShadow",
        [](Renderer& r) { r.raytraceShadowPass(); }, PassFlags::RequiresRaytracing);
    renderGraph.addPass("RaytraceAO",
        [](Renderer& r) { r.raytraceAOPass(); }, PassFlags::RequiresRaytracing);

    // Main geometry pass: renders to colorRT when a post-process pipeline
    // Directional shadow depth (single cascade) before the main lighting pass.
    renderGraph.addPass("Shadow",
        [](Renderer& r) { r.shadowPass(); });

    // exists, directly to the swapchain otherwise (decided inside the pass).
    renderGraph.addPass("Main",
        [](Renderer& r) { r.mainRenderPass(); });

    // Sky/atmosphere fills the background (depth == far) before bloom, so the
    // bright sky and sun disk feed the bloom pyramid.
    renderGraph.addPass("SkyAtmosphere",
        [](Renderer& r) { r.skyAtmospherePass(); });

    // Pyramid bloom: brightness extract -> downsample chain -> tent-filter
    // upsample chain (accumulates into pyramid[0]); composited in PostProcess.
    // No-ops until the bloom pipelines/targets exist.
    renderGraph.addPass("BloomBrightness",
        [](Renderer& r) { r.bloomBrightnessPass(); });
    renderGraph.addPass("BloomDownsample",
        [](Renderer& r) { r.bloomDownsamplePass(); });
    renderGraph.addPass("BloomUpsample",
        [](Renderer& r) { r.bloomUpsamplePass(); });

    // Fullscreen post-process to swapchain; no-op until a post-process
    // pipeline is created.
    renderGraph.addPass("PostProcess",
        [](Renderer& r) { r.postProcessPass(); });
}

void Renderer::endFrame() {
    // Render ImGui (matching old renderer behavior)
    // Note: ImGui::NewFrame() and ImGui::Render() should be called by user code
    // We only handle the backend rendering here

    if (ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount > 0) {
        // Render ImGui using backend-specific implementation
        // Matching old renderer: create render pass, then render ImGui
        switch (backend) {
#ifdef __APPLE__
            case GraphicsBackend::Metal: {
                void* cmdBuffer = rhi->getBackendCommandBuffer();
                if (cmdBuffer) {
                    // Create ImGui render pass (load existing content, don't clear)
                    RenderPassDesc imguiPassDesc;
                    imguiPassDesc.name = "ImGui";
                    imguiPassDesc.colorAttachments.push_back(TextureHandle{0});  // Use swapchain
                    imguiPassDesc.loadColor.push_back(true);  // Load (don't clear, render on top)
                    // No depth attachment for the UI pass

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
#endif
            case GraphicsBackend::Vulkan: {
                // Vulkan ImGui rendering (skip if the frame was skipped).
                // Must happen inside a render pass (dynamic rendering).
                void* cmdBuffer = rhi->getBackendCommandBuffer();
                if (cmdBuffer) {
                    RenderPassDesc imguiPassDesc;
                    imguiPassDesc.name = "ImGui";
                    imguiPassDesc.colorAttachments.push_back(TextureHandle{0});  // swapchain
                    imguiPassDesc.loadColor.push_back(true);  // draw on top
                    rhi->beginRenderPass(imguiPassDesc);
                    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                        static_cast<VkCommandBuffer>(cmdBuffer));
                    rhi->endRenderPass();
                }
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
    currentFrameInFlight = (currentFrameInFlight + 1) % rhi->getMaxFramesInFlight();
    frameNumber++;
}

// ============================================================================
// Internal Rendering Steps
// ============================================================================

void Renderer::performCulling() {
    Frustum frustum = extractFrustum(currentCamera.proj * currentCamera.view);

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
    }

    if (!instanceData.empty()) {
        rhi->updateBuffer(instanceDataBuffer, instanceData.data(), 0,
                          instanceData.size() * sizeof(Vapor::InstanceData));
    }

    // Clusters: until the TileCulling compute pass is ported, every cluster
    // lists every point light (correct, just unculled). Refilled only when
    // the light count changes — the buffer is ~6MB.
    Uint32 clusterLightCount = static_cast<Uint32>(std::min<size_t>(pointLights.size(), 256));
    if (clusterLightCount != lastClusterLightCount) {
        lastClusterLightCount = clusterLightCount;
        Vapor::Cluster tpl{};
        tpl.lightCount = clusterLightCount;
        for (Uint32 i = 0; i < clusterLightCount; i++) {
            tpl.lightIndices[i] = i;
        }
        std::vector<Vapor::Cluster> clusters(
            static_cast<size_t>(clusterGridSizeX) * clusterGridSizeY * clusterGridSizeZ, tpl);
        rhi->updateBuffer(clusterBuffer, clusters.data(), 0,
                          clusters.size() * sizeof(Vapor::Cluster));
    }

    if (!rectLights.empty()) {
        rhi->updateBuffer(rectLightBuffer, rectLights.data(), 0,
                          rectLights.size() * sizeof(Vapor::RectLight));
    }
}

void Renderer::mainRenderPass() {
    if (!mainPipeline.isValid()) {
        return;
    }

    // Get swapchain dimensions for render pass
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();

    // Create render pass descriptor
    RenderPassDesc renderPassDesc;
    renderPassDesc.name = "Main";
    // If post-process pipeline exists, render to colorRT; otherwise render directly to swapchain.
    // Note: single-sampled colorRT/depthStencilRT are used (not the MSAA
    // variants) because every pipeline is currently created with
    // sampleCount = 1; a pipeline/pass sample-count mismatch is a Metal
    // validation error. Re-enable the MSAA path together with pipeline
    // sampleCount once the MSAA pipeline variants exist.
    bool usePostProcess = postProcessPipeline.isValid() && colorRT.isValid() && depthStencilRT.isValid();
    if (usePostProcess) {
        renderPassDesc.colorAttachments.push_back(colorRT);
        renderPassDesc.depthAttachment = depthStencilRT;
    } else {
        // Fallback: render directly to swapchain
        renderPassDesc.colorAttachments.push_back(TextureHandle{0});  // Use swapchain (handle 0 is special)
        renderPassDesc.depthAttachment = swapchainDepthBuffer;  // Use swapchain depth buffer
    }
    renderPassDesc.clearColors.push_back(clearColor);  // editable in the Engine window
    renderPassDesc.loadColor.push_back(false);  // Clear
    renderPassDesc.clearDepth = static_cast<float>(clearDepth);
    renderPassDesc.loadDepth = false;  // Clear

    // Begin render pass
    rhi->beginRenderPass(renderPassDesc);

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

    // Fragment bindings — the FULL contract of 3d_pbr_normal_mapped.metal.
    // Every slot the shader declares must be bound (Metal reads several of
    // them unconditionally; an unbound buffer/texture is undefined behavior).
    // Slots whose passes aren't ported yet get neutral defaults.
    // The shader (fragment buffer table):
    //   0 dirLights  1 pointLights  2 clusters  3 camera
    //   4 screenSize 5 gridSize     6 time      7 rectLights
    //   8 rectLightCount  9 pssmData  10 gibsEnabled
    rhi->setFragmentBuffer(0, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
    rhi->setFragmentBuffer(1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
    rhi->setFragmentBuffer(2, clusterBuffer);
    rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));

    glm::vec2 screenSize(static_cast<float>(width), static_cast<float>(height));
    rhi->setFragmentBytes(&screenSize, sizeof(glm::vec2), 4);
    glm::uvec3 gridSize(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
    rhi->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
    float time = 0.0f;  // TODO: Get actual time
    rhi->setFragmentBytes(&time, sizeof(float), 6);

    rhi->setFragmentBuffer(7, rectLightBuffer);
    Uint32 rectLightCount = static_cast<Uint32>(rectLights.size());
    rhi->setFragmentBytes(&rectLightCount, sizeof(Uint32), 8);
    rhi->setFragmentBuffer(9, pssmDataBuffer, 0, sizeof(PSSMRenderData));
    Uint32 gibsEnabled = 0;  // GIBS not ported to the RHI renderer yet
    rhi->setFragmentBytes(&gibsEnabled, sizeof(Uint32), 10);

    // Light counts for the Vulkan shader (no cluster culling there yet).
    // Binding 11: free on Metal, and maps to push-constant offset 112 on
    // Vulkan — exactly where RHIMain.frag reads it.
    glm::uvec2 lightCounts(static_cast<Uint32>(directionalLights.size()),
                           static_cast<Uint32>(pointLights.size()));
    rhi->setFragmentBytes(&lightCounts, sizeof(glm::uvec2), 11);

    // Default textures for the shadow/AO/IBL slots (texture table 6-14):
    //   6 texAO  7 texShadow  8 irradiance  9 prefilter  10 brdfLUT
    //   11 rectLightVideo  12 pssmShadowMaps  13 texPointShadow  14 gibsGI
    // White = neutral shadow/AO (fully lit); black = zero IBL/GI contribution.
    TextureHandle whiteTex = textures[defaultWhiteTexture].handle;
    TextureHandle blackTex = textures[defaultBlackTexture].handle;
    rhi->setTexture(0, 6, whiteTex, defaultSampler);
    rhi->setTexture(0, 7, whiteTex, defaultSampler);
    rhi->setTexture(0, 8, defaultBlackCubemapTex, defaultSampler);
    rhi->setTexture(0, 9, defaultBlackCubemapTex, defaultSampler);
    rhi->setTexture(0, 10, blackTex, defaultSampler);
    rhi->setTexture(0, 11, whiteTex, defaultSampler);
    rhi->setTexture(0, 12, pssmShadowArrayTexture, defaultSampler);
    rhi->setTexture(0, 13, whiteTex, defaultSampler);
    rhi->setTexture(0, 14, blackTex, defaultSampler);

    // PSSM cascaded shadow (Vulkan binding budget is 8 slots/set, so the Metal
    // contract slots 9/12 above are no-ops here): cascade data at set1 b2 and
    // the 3-layer depth array at set2 b6. RHIMain.frag reads a sampler2DArray at
    // b6 and the PSSMBuf at b2; the neutral binding-6 texture is overridden.
    if (pssmDataBuffer.isValid()) rhi->setFragmentBuffer(2, pssmDataBuffer, 0, sizeof(PSSMRenderData));
    if (pssmShadowArrayTexture.isValid()) rhi->setTexture(0, 6, pssmShadowArrayTexture, shadowSampler);

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

// Directional shadow pass (PSSM, 3 cascades): split the camera frustum by a
// practical (log/uniform blend) scheme, fit a texel-snapped ortho box to each
// cascade's bounding sphere, and render scene depth into one layer of the
// shadow array per cascade. All three light-space matrices are uploaded once to
// pssmDataBuffer and the cascade being drawn is selected by a vertex push
// constant (a per-cascade buffer rewrite would race — host-visible updates are
// immediate while the GPU executes the whole frame later). On Vulkan there is
// no RT near-region, so the cascades cover the full [near, far] range.
void Renderer::shadowPass() {
    if (!shadowPipeline.isValid() || !pssmShadowArrayTexture.isValid() ||
        !pssmDataBuffer.isValid() || directionalLights.empty()) {
        return;
    }

    const float nearClip = currentCamera.nearPlane;
    const float farClip  = currentCamera.farPlane;
    // No ray-traced near cascade on Vulkan: cascade 0 starts at the near plane.
    const float rtEnd = nearClip;

    // Cascade split distances (view space). splits[0] = near end, splits[3] = far.
    float splits[4];
    splits[0] = rtEnd;
    const float lambda = 0.7f;  // 0 = uniform, 1 = logarithmic
    for (int i = 1; i <= 3; i++) {
        float p = float(i) / 3.0f;
        float logS = rtEnd * std::pow(farClip / glm::max(rtEnd, 0.1f), p);
        float uniS = rtEnd + (farClip - rtEnd) * p;
        splits[i] = lambda * logS + (1.0f - lambda) * uniS;
    }

    glm::vec3 lightDir = glm::normalize(directionalLights[0].direction);
    glm::vec3 up = (std::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) < 0.99f)
                 ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);

    glm::mat4 invVP = glm::inverse(currentCamera.proj * currentCamera.view);
    // View-space forward distance -> NDC z, using the actual (ZO) projection so
    // handedness and near/far are respected. RH proj has proj[2][3] == -1.
    const float zSign = (currentCamera.proj[2][3] < 0.0f) ? -1.0f : 1.0f;
    auto viewDepthToNDCz = [&](float d) -> float {
        glm::vec4 clip = currentCamera.proj * glm::vec4(0.0f, 0.0f, zSign * d, 1.0f);
        return clip.z / clip.w;
    };

    PSSMRenderData gpuData;
    gpuData.cascadeSplits = glm::vec4(splits[0], splits[1], splits[2], splits[3]);
    gpuData.blendRange = (farClip - rtEnd) * 0.05f;

    for (int ci = 0; ci < 3; ci++) {
        float splitNear = glm::clamp(splits[ci],     nearClip, farClip);
        float splitFar  = glm::clamp(splits[ci + 1], nearClip, farClip);
        float nearNDCz = viewDepthToNDCz(splitNear);
        float farNDCz  = viewDepthToNDCz(splitFar);

        // Sub-frustum corners at this cascade's exact z slice (world space).
        const glm::vec4 cascadeNDC[8] = {
            {-1,-1,nearNDCz,1},{1,-1,nearNDCz,1},{-1,1,nearNDCz,1},{1,1,nearNDCz,1},
            {-1,-1,farNDCz, 1},{1,-1,farNDCz, 1},{-1,1,farNDCz, 1},{1,1,farNDCz, 1},
        };
        glm::vec3 corners[8];
        glm::vec3 sphereCenter(0.0f);
        for (int i = 0; i < 8; i++) {
            glm::vec4 w = invVP * cascadeNDC[i];
            corners[i] = glm::vec3(w) / w.w;
            sphereCenter += corners[i];
        }
        sphereCenter /= 8.0f;
        float sphereRadius = 0.0f;
        for (auto& c : corners) sphereRadius = glm::max(sphereRadius, glm::length(c - sphereCenter));

        const float lightDist = sphereRadius * 2.0f + 1.0f;
        glm::mat4 lightView = glm::lookAt(sphereCenter - lightDir * lightDist, sphereCenter, up);

        // Snap the sphere center to the texel grid in light space (anti-shimmer).
        float texelSize = (2.0f * sphereRadius) / float(SHADOW_MAP_SIZE);
        glm::vec4 lsCenter = lightView * glm::vec4(sphereCenter, 1.0f);
        lsCenter.x = std::floor(lsCenter.x / texelSize) * texelSize;
        lsCenter.y = std::floor(lsCenter.y / texelSize) * texelSize;
        glm::vec3 snapped = glm::vec3(glm::inverse(lightView) * lsCenter);
        lightView = glm::lookAt(snapped - lightDir * lightDist, snapped, up);

        float minDist = std::numeric_limits<float>::max();
        float maxDist = -minDist;
        for (auto& c : corners) {
            float d = -(lightView * glm::vec4(c, 1.0f)).z;  // RH: -z is forward
            minDist = glm::min(minDist, d);
            maxDist = glm::max(maxDist, d);
        }
        minDist -= (maxDist - minDist);  // extend near to catch casters behind the cascade

        glm::mat4 lightProj = glm::orthoZO(-sphereRadius, sphereRadius, -sphereRadius, sphereRadius, minDist, maxDist);
        gpuData.lightSpaceMatrices[ci] = lightProj * lightView;
    }

    // Upload all cascade matrices once; the shadow VS indexes by cascadeIndex.
    rhi->updateBuffer(pssmDataBuffer, &gpuData, 0, sizeof(gpuData));

    // Render scene depth into each cascade layer (depth only).
    for (Uint32 ci = 0; ci < 3; ci++) {
        RenderPassDesc rp;
        rp.name = "ShadowCascade";
        rp.depthAttachment = pssmShadowArrayTexture;
        rp.depthArrayLayer = ci;
        rp.loadDepth = false;  // clear
        rp.clearDepth = 1.0f;
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(shadowPipeline);
        rhi->setVertexBuffer(0, pssmDataBuffer, 0, sizeof(PSSMRenderData));
        rhi->setVertexBuffer(2, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * std::max<size_t>(1, visibleDrawables.size()));
        rhi->setVertexBytes(&ci, sizeof(Uint32), 5);  // cascadeIndex -> push offset 16

        for (Uint32 drawableIdx : visibleDrawables) {
            const Drawable& drawable = frameDrawables[drawableIdx];
            const RenderMesh& mesh = meshes[drawable.mesh];
            auto it = drawableToInstanceID.find(drawableIdx);
            if (it == drawableToInstanceID.end()) continue;
            Uint32 iid = it->second;
            if (mesh.vertexBuffer.isValid()) rhi->bindVertexBuffer(mesh.vertexBuffer, 3, 0);
            rhi->setVertexBytes(&iid, sizeof(Uint32), 4);  // instanceID -> push offset 0
            if (mesh.indexBuffer.isValid()) {
                rhi->bindIndexBuffer(mesh.indexBuffer, 0);
                rhi->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
            } else if (mesh.vertexBuffer.isValid()) {
                rhi->draw(mesh.vertexCount, 1, 0, 0);
            }
        }
        rhi->endRenderPass();
    }
}

// Bloom brightness: soft-threshold extract from colorRT into the half-res
// bloomBrightness target.
void Renderer::bloomBrightnessPass() {
    if (!bloomBrightPipeline.isValid() || !colorRT.isValid() || !bloomBrightness.isValid()) return;
    RenderPassDesc rp;
    rp.name = "BloomBrightness";
    rp.colorAttachments.push_back(bloomBrightness);
    rp.clearColors.push_back(glm::vec4(0.0f));
    rp.loadColor.push_back(false);  // clear
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(bloomBrightPipeline);
    rhi->setTexture(0, 0, colorRT, clampSampler);
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();
}

// Bloom downsample: build the pyramid. brightness -> pyramid[0], then
// pyramid[i-1] -> pyramid[i], each a 3x3 gaussian at decreasing resolution.
void Renderer::bloomDownsamplePass() {
    if (!bloomDownsamplePipeline.isValid() || !bloomBrightness.isValid()) return;
    for (Uint32 i = 0; i < BLOOM_PYRAMID_LEVELS; i++) {
        if (!bloomPyramid[i].isValid()) return;
        TextureHandle src = (i == 0) ? bloomBrightness : bloomPyramid[i - 1];
        RenderPassDesc rp;
        rp.name = "BloomDownsample";
        rp.colorAttachments.push_back(bloomPyramid[i]);
        rp.clearColors.push_back(glm::vec4(0.0f));
        rp.loadColor.push_back(false);  // clear
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(bloomDownsamplePipeline);
        rhi->setTexture(0, 0, src, clampSampler);
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    }
}

// Bloom upsample: from the bottom of the pyramid up to pyramid[0], tent-filter
// the lower level and ADD it (additive blend) onto the current level, so
// pyramid[0] ends up holding the fully accumulated bloom.
void Renderer::bloomUpsamplePass() {
    if (!bloomUpsamplePipeline.isValid()) return;
    for (int i = static_cast<int>(BLOOM_PYRAMID_LEVELS) - 2; i >= 0; i--) {
        if (!bloomPyramid[i].isValid() || !bloomPyramid[i + 1].isValid()) continue;
        RenderPassDesc rp;
        rp.name = "BloomUpsample";
        rp.colorAttachments.push_back(bloomPyramid[i]);
        rp.loadColor.push_back(true);   // keep this level's downsampled content
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(bloomUpsamplePipeline);
        rhi->setTexture(0, 0, bloomPyramid[i + 1], clampSampler);
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    }
}

// Sky/atmosphere pass: physically-based Rayleigh/Mie scattering rendered into
// the HDR colorRT, depth-tested so it only fills background pixels (where the
// main pass left depth at the far plane). Runs after Main, before bloom, so the
// bright sky/sun participate in bloom.
void Renderer::skyAtmospherePass() {
    if (!atmospherePipeline.isValid() || !colorRT.isValid() || !depthStencilRT.isValid() ||
        !atmosphereDataBuffer.isValid()) {
        return;
    }
    RenderPassDesc rp;
    rp.name = "SkyAtmosphere";
    rp.colorAttachments.push_back(colorRT);
    rp.loadColor.push_back(true);      // preserve the rendered scene
    rp.depthAttachment = depthStencilRT;
    rp.loadDepth = true;               // test against the scene depth (no writes)
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(atmospherePipeline);
    rhi->setFragmentBuffer(0, atmosphereDataBuffer, 0, sizeof(AtmosphereRenderData));
    rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();
}

void Renderer::postProcessPass() {
    // Post-process pass: render from colorRT to swapchain (fullscreen triangle)
    if (!postProcessPipeline.isValid() || !colorRT.isValid()) {
        // If no post-process pipeline, just skip (or could do a simple copy)
        return;
    }

    // Create render pass descriptor for swapchain
    RenderPassDesc renderPassDesc;
    renderPassDesc.name = "PostProcess";
    renderPassDesc.colorAttachments.push_back(TextureHandle{0});  // Use swapchain (handle 0 is special)
    renderPassDesc.clearColors.push_back(glm::vec4(0.2f, 0.2f, 0.3f, 1.0f));
    renderPassDesc.loadColor.push_back(false);  // Clear

    // Begin render pass
    rhi->beginRenderPass(renderPassDesc);

    // Bind post-process pipeline
    rhi->bindPipeline(postProcessPipeline);

    // Fragment texture 0: HDR colorRT; texture 1: accumulated bloom (pyramid[0]).
    if (colorRT.isValid() && clampSampler.isValid()) {
        rhi->setTexture(0, 0, colorRT, clampSampler);
    }
    if (bloomPyramid[0].isValid() && clampSampler.isValid()) {
        rhi->setTexture(0, 1, bloomPyramid[0], clampSampler);
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

    // Shadow-map sampler: nearest + clamp. Manual PCF does its own 3x3 filtering,
    // so point sampling is both correct (no bilinear blending of depth values)
    // and the fast hardware path; clamp-to-edge avoids wrap artifacts at cascade
    // borders. (Linear depth sampling is emulated in-shader on MoltenVK — a
    // large per-pixel cost for the main pass's PCF loop.)
    SamplerDesc shadowSamplerDesc;
    shadowSamplerDesc.minFilter = FilterMode::Nearest;
    shadowSamplerDesc.magFilter = FilterMode::Nearest;
    shadowSamplerDesc.mipFilter = FilterMode::Nearest;
    shadowSamplerDesc.addressModeU = AddressMode::ClampToEdge;
    shadowSamplerDesc.addressModeV = AddressMode::ClampToEdge;
    shadowSamplerDesc.addressModeW = AddressMode::ClampToEdge;
    shadowSampler = rhi->createSampler(shadowSamplerDesc);

    // Linear + clamp sampler for fullscreen/bloom sampling (avoids edge wrap).
    SamplerDesc clampSamplerDesc;
    clampSamplerDesc.minFilter = FilterMode::Linear;
    clampSamplerDesc.magFilter = FilterMode::Linear;
    clampSamplerDesc.mipFilter = FilterMode::Linear;
    clampSamplerDesc.addressModeU = AddressMode::ClampToEdge;
    clampSamplerDesc.addressModeV = AddressMode::ClampToEdge;
    clampSamplerDesc.addressModeW = AddressMode::ClampToEdge;
    clampSampler = rhi->createSampler(clampSamplerDesc);

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

    // Neutral defaults for the PBR shader's shadow/IBL bindings (see the
    // "Full PBR shader contract" note in renderer.hpp)
    {
        // Black 1x1 cubemap: IBL irradiance/prefilter contribute nothing
        TextureDesc cubeDesc;
        cubeDesc.width = 1;
        cubeDesc.height = 1;
        cubeDesc.arrayLayers = 6;
        cubeDesc.isCube = true;
        cubeDesc.format = PixelFormat::RGBA8_UNORM;
        cubeDesc.usage = TextureUsage::Sampled;
        defaultBlackCubemapTex = rhi->createTexture(cubeDesc);
        Uint32 blackPixel = 0xFF000000;
        for (Uint32 face = 0; face < 6; face++) {
            rhi->updateTexture(defaultBlackCubemapTex, &blackPixel, sizeof(Uint32), 0, face);
        }

        // PSSM cascaded shadow map: a 3-layer depth array, each layer rendered
        // by the shadow pass and sampled as a sampler2DArray in the PBR shader.
        // Fixed-size (independent of the swapchain), so it lives here rather than
        // in createRenderTargets. DepthStencil = render target, Sampled = read.
        TextureDesc pssmDesc;
        pssmDesc.width = SHADOW_MAP_SIZE;
        pssmDesc.height = SHADOW_MAP_SIZE;
        pssmDesc.arrayLayers = 3;
        pssmDesc.format = PixelFormat::Depth32Float;
        pssmDesc.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
        pssmShadowArrayTexture = rhi->createTexture(pssmDesc);
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
        desc.usage = TextureUsage::DepthStencil;
        desc.sampleCount = MSAA_SAMPLE_COUNT;
        depthStencilRT_MSAA = rhi->createTexture(desc);

        desc.sampleCount = 1;
        desc.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;  // Sampled by later passes
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
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;  // Sampled in post-process
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
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled;  // Written by compute, sampled in post
        normalRT = rhi->createTexture(desc);
    }

    // Create shadow RT
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::RGBA8_UNORM;
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled;
        desc.mipLevels = static_cast<Uint32>(std::floor(std::log2(std::max(width, height))) + 1);
        shadowRT = rhi->createTexture(desc);
    }

    // Create AO RT
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::R16_FLOAT;  // Single channel float
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled;
        aoRT = rhi->createTexture(desc);
    }

    // Bloom pyramid targets (HDR). Brightness extract at half res, then a chain
    // of progressively-halved levels (matches the Metal backend's sizing:
    // pyramid[i] = swapchain / 2^(i+1)).
    {
        TextureDesc desc;
        desc.format = PixelFormat::RGBA16_FLOAT;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        desc.sampleCount = 1;

        desc.width = std::max(1u, width / 2);
        desc.height = std::max(1u, height / 2);
        bloomBrightness = rhi->createTexture(desc);

        for (Uint32 i = 0; i < BLOOM_PYRAMID_LEVELS; i++) {
            desc.width = std::max(1u, width >> (i + 1));
            desc.height = std::max(1u, height >> (i + 1));
            bloomPyramid[i] = rhi->createTexture(desc);
        }
    }

    // Create default depth buffer for swapchain rendering (when not using render targets)
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::Depth32Float;
        desc.usage = TextureUsage::DepthStencil;
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
        vertShaderCode = readFile("shaders/RHIMain.vert.spv");
        fragShaderCode = readFile("shaders/RHIMain.frag.spv");
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
        {0, PixelFormat::RGB32_FLOAT, offsetof(Vapor::VertexData, position)},  // Position (vec3)
        {1, PixelFormat::RG32_FLOAT, offsetof(Vapor::VertexData, uv)},          // UV (vec2)
        {2, PixelFormat::RGB32_FLOAT, offsetof(Vapor::VertexData, normal)},     // Normal (vec3)
        {3, PixelFormat::RGBA32_FLOAT, offsetof(Vapor::VertexData, tangent)}    // Tangent (vec4)
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
    // Main geometry renders into the HDR colorRT (RGBA16F); the PostProcess pass
    // tone-maps it to the swapchain. Bake the matching color format into the PSO.
    pipelineDesc.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
    pipelineDesc.hasDepthAttachment = true;
    pipelineDesc.depthAttachmentFormat = PixelFormat::Depth32Float;

    mainPipeline = rhi->createPipeline(pipelineDesc);

    // ------------------------------------------------------------------------
    // Post-process pipeline: fullscreen triangle sampling colorRT, ACES tone
    // map + sRGB encode to the swapchain. Vulkan only (the Metal backend uses
    // the native renderer). Activating this makes mainRenderPass() route to
    // colorRT and postProcessPass() composite to the swapchain.
    // ------------------------------------------------------------------------
    if (backend == GraphicsBackend::Vulkan) {
        std::string ppVertCode = readFile("shaders/FullScreen.vert.spv");
        std::string ppFragCode = readFile("shaders/PostProcess.frag.spv");
        if (!ppVertCode.empty() && !ppFragCode.empty()) {
            ShaderDesc ppVertDesc;
            ppVertDesc.stage = ShaderStage::Vertex;
            ppVertDesc.code = ppVertCode.data();
            ppVertDesc.codeSize = ppVertCode.size();
            ppVertDesc.entryPoint = "main";
            postProcessVertexShader = rhi->createShader(ppVertDesc);

            ShaderDesc ppFragDesc;
            ppFragDesc.stage = ShaderStage::Fragment;
            ppFragDesc.code = ppFragCode.data();
            ppFragDesc.codeSize = ppFragCode.size();
            ppFragDesc.entryPoint = "main";
            postProcessFragmentShader = rhi->createShader(ppFragDesc);

            PipelineDesc ppDesc;
            ppDesc.vertexShader = postProcessVertexShader;
            ppDesc.fragmentShader = postProcessFragmentShader;
            // Fullscreen triangle generates its own vertices from gl_VertexIndex;
            // no vertex buffer / attributes.
            ppDesc.vertexLayout.stride = 0;
            ppDesc.vertexLayout.attributes = {};
            ppDesc.topology = PrimitiveTopology::TriangleList;
            ppDesc.blendMode = BlendMode::Opaque;
            ppDesc.depthTest = false;
            ppDesc.depthWrite = false;
            ppDesc.cullMode = CullMode::None;
            ppDesc.sampleCount = 1;
            ppDesc.hasDepthAttachment = false;
            ppDesc.colorAttachmentFormats = { PixelFormat::Swapchain };
            postProcessPipeline = rhi->createPipeline(ppDesc);

            // Pyramid bloom pipelines: fullscreen (reusing FullScreen.vert),
            // rendering into RGBA16F pyramid levels. The upsample pass uses
            // additive blending to accumulate onto the level it targets.
            auto makeFullscreenFragPipeline = [&](const char* fragSpv, ShaderHandle& outShader,
                                                  BlendMode blend) -> PipelineHandle {
                std::string code = readFile(fragSpv);
                if (code.empty()) return {};
                ShaderDesc fd;
                fd.stage = ShaderStage::Fragment;
                fd.code = code.data();
                fd.codeSize = code.size();
                fd.entryPoint = "main";
                outShader = rhi->createShader(fd);
                PipelineDesc d;
                d.vertexShader = postProcessVertexShader;
                d.fragmentShader = outShader;
                d.vertexLayout.stride = 0;
                d.vertexLayout.attributes = {};
                d.topology = PrimitiveTopology::TriangleList;
                d.blendMode = blend;
                d.depthTest = false;
                d.depthWrite = false;
                d.cullMode = CullMode::None;
                d.sampleCount = 1;
                d.hasDepthAttachment = false;
                d.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
                return rhi->createPipeline(d);
            };
            bloomBrightPipeline      = makeFullscreenFragPipeline("shaders/BloomBright.frag.spv",     bloomBrightShader,     BlendMode::Opaque);
            bloomDownsamplePipeline  = makeFullscreenFragPipeline("shaders/BloomDownsample.frag.spv", bloomDownsampleShader, BlendMode::Opaque);
            bloomUpsamplePipeline    = makeFullscreenFragPipeline("shaders/BloomUpsample.frag.spv",   bloomUpsampleShader,   BlendMode::Additive);

            // Sky/atmosphere: fullscreen into the HDR colorRT, depth-tested
            // (LessOrEqual at z=1.0) so it only fills background pixels; no
            // depth write. Its own vertex shader (z=1.0), so not the lambda.
            std::string skyVertCode = readFile("shaders/Sky.vert.spv");
            std::string atmoFragCode = readFile("shaders/Atmosphere.frag.spv");
            if (!skyVertCode.empty() && !atmoFragCode.empty()) {
                ShaderDesc svd; svd.stage = ShaderStage::Vertex;   svd.code = skyVertCode.data();  svd.codeSize = skyVertCode.size();  svd.entryPoint = "main";
                atmosphereVertexShader = rhi->createShader(svd);
                ShaderDesc afd; afd.stage = ShaderStage::Fragment; afd.code = atmoFragCode.data(); afd.codeSize = atmoFragCode.size(); afd.entryPoint = "main";
                atmosphereFragmentShader = rhi->createShader(afd);

                PipelineDesc ad;
                ad.vertexShader = atmosphereVertexShader;
                ad.fragmentShader = atmosphereFragmentShader;
                ad.vertexLayout.stride = 0;
                ad.vertexLayout.attributes = {};
                ad.topology = PrimitiveTopology::TriangleList;
                ad.blendMode = BlendMode::Opaque;
                ad.depthTest = true;
                ad.depthWrite = false;
                ad.depthCompareOp = CompareOp::LessOrEqual;
                ad.cullMode = CullMode::None;
                ad.sampleCount = 1;
                ad.hasDepthAttachment = true;
                ad.depthAttachmentFormat = PixelFormat::Depth32Float;
                ad.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
                atmospherePipeline = rhi->createPipeline(ad);
            }
        }

        // Directional shadow depth pipeline: renders scene geometry into the
        // shadow map (depth only, no color attachment).
        std::string svCode = readFile("shaders/ShadowDepth.vert.spv");
        std::string sfCode = readFile("shaders/ShadowDepth.frag.spv");
        if (!svCode.empty() && !sfCode.empty()) {
            ShaderDesc vd; vd.stage = ShaderStage::Vertex;   vd.code = svCode.data(); vd.codeSize = svCode.size(); vd.entryPoint = "main";
            shadowVertexShader = rhi->createShader(vd);
            ShaderDesc fd; fd.stage = ShaderStage::Fragment; fd.code = sfCode.data(); fd.codeSize = sfCode.size(); fd.entryPoint = "main";
            shadowFragmentShader = rhi->createShader(fd);

            PipelineDesc sd;
            sd.vertexShader = shadowVertexShader;
            sd.fragmentShader = shadowFragmentShader;
            sd.vertexLayout.stride = sizeof(Vapor::VertexData);
            sd.vertexLayout.attributes = {
                {0, PixelFormat::RGB32_FLOAT, offsetof(Vapor::VertexData, position)},
                {1, PixelFormat::RG32_FLOAT,  offsetof(Vapor::VertexData, uv)},
                {2, PixelFormat::RGB32_FLOAT, offsetof(Vapor::VertexData, normal)},
                {3, PixelFormat::RGBA32_FLOAT, offsetof(Vapor::VertexData, tangent)},
            };
            sd.topology = PrimitiveTopology::TriangleList;
            sd.blendMode = BlendMode::Opaque;
            sd.depthTest = true;
            sd.depthWrite = true;
            sd.depthCompareOp = CompareOp::Less;
            // Cull front faces in the shadow pass to reduce peter-panning/acne.
            sd.cullMode = CullMode::Front;
            sd.frontFaceCounterClockwise = true;
            sd.sampleCount = 1;
            sd.hasDepthAttachment = true;
            sd.depthAttachmentFormat = PixelFormat::Depth32Float;
            sd.colorAttachmentFormats = {};  // depth-only, no color
            shadowPipeline = rhi->createPipeline(sd);
        }
    }
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

std::unique_ptr<IRenderer> createRenderer(GraphicsBackend backend, SDL_Window* window) {
#ifdef __APPLE__
    // Metal uses the full-feature native renderer (45 passes, RT/GIBS/water/…).
    if (backend == GraphicsBackend::Metal) {
        return createRendererMetal(window);
    }
#endif

    std::unique_ptr<RHI> rhi;

    switch (backend) {
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

                // Dynamic rendering: ImGui bakes the attachment format into
                // its pipeline, so it must match the swapchain format.
                static VkFormat imguiColorFormat;
                imguiColorFormat = (rhi->getSwapchainFormat() == PixelFormat::BGRA8_SRGB)
                    ? VK_FORMAT_B8G8R8A8_SRGB
                    : VK_FORMAT_B8G8R8A8_UNORM;
                VkPipelineRenderingCreateInfo renderingInfo = {};
                renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                renderingInfo.colorAttachmentCount = 1;
                renderingInfo.pColorAttachmentFormats = &imguiColorFormat;

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
                initInfo.PipelineRenderingCreateInfo = renderingInfo;
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
            if (!mesh->vertices.empty()) {
                mesh->renderMeshId = registerMesh(mesh->vertices, mesh->indices);
            } else if (mesh->vertexCount > 0 && mesh->indexCount > 0 &&
                       mesh->vertexOffset + mesh->vertexCount <= scene->vertices.size() &&
                       mesh->indexOffset + mesh->indexCount <= scene->indices.size()) {
                // Memory-optimized scenes keep geometry only in the scene-level
                // flat buffers (per-mesh arrays stay empty); slice this mesh's
                // range out. Index values in the flat buffer are mesh-local, so
                // no rebasing is needed.
                // TODO: share one scene-wide vertex/index buffer and draw with
                // firstIndex/vertexOffset instead of copying per mesh.
                std::vector<Vapor::VertexData> verts(
                    scene->vertices.begin() + mesh->vertexOffset,
                    scene->vertices.begin() + mesh->vertexOffset + mesh->vertexCount);
                std::vector<Uint32> inds(
                    scene->indices.begin() + mesh->indexOffset,
                    scene->indices.begin() + mesh->indexOffset + mesh->indexCount);
                mesh->renderMeshId = registerMesh(verts, inds);
            }
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

    submitSceneLights(scene);

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

    // Lights were gathered into the scene by the game's LightGatherSystem
    submitSceneLights(scene);

    // Render
    render();
}

void Renderer::submitSceneLights(const std::shared_ptr<Scene>& scene) {
    if (!scene) {
        return;
    }
    for (const auto& l : scene->directionalLights) {
        DirectionalLightData data{};
        data.direction = l.direction;
        data.color = l.color;
        data.intensity = l.intensity;
        submitDirectionalLight(data);
    }
    for (const auto& l : scene->pointLights) {
        PointLightData data{};
        data.position = l.position;
        data.color = l.color;
        data.intensity = l.intensity;
        data.radius = l.radius;
        submitPointLight(data);
    }
    for (const auto& l : scene->rectLights) {
        if (rectLights.size() >= maxRectLights) break;
        rectLights.push_back(l);
    }
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

void Renderer::invokeImGuiCallback() {
    // F1 toggles the engine ImGui overlay on/off.
    if (ImGui::IsKeyPressed(ImGuiKey_F1))
        m_imGuiVisible = !m_imGuiVisible;

    // Per-frame engine hook (recording capture + F2 hotkey). Runs whether or not
    // the overlay is visible so recording keeps working with the UI hidden.
    if (m_imGuiFrameCallback)
        m_imGuiFrameCallback();

    if (!m_imGuiVisible)
        return;

    if (imGuiCallback)
        imGuiCallback();

    ImGui::Begin("Engine");

    drawGraphicsImGui();
    drawRenderGraphImGui();
    drawGpuTimingsImGui();

    if (m_engineWindowCallback)
        m_engineWindowCallback();

    ImGui::End();
}

void* Renderer::getImGuiTextureID(TextureHandle handle) {
    if (!handle.isValid()) {
        return nullptr;
    }
#ifdef __APPLE__
    if (backend == GraphicsBackend::Metal) {
        // ImGui's Metal backend takes the MTLTexture pointer directly
        return rhi->getBackendTexture(handle);
    }
#endif
    if (backend == GraphicsBackend::Vulkan) {
        auto it = imguiTextureCache.find(handle.id);
        if (it != imguiTextureCache.end()) {
            return it->second;
        }
        void* view = rhi->getBackendTexture(handle);
        void* sampler = rhi->getBackendSampler(defaultSampler);
        if (!view || !sampler) {
            return nullptr;
        }
        VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
            (VkSampler)sampler, (VkImageView)view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        imguiTextureCache[handle.id] = (void*)ds;
        return (void*)ds;
    }
    return nullptr;
}

// The Graphics section of the Engine window — restored from the pre-RHI
// renderer: framerate, clear color, scene stats, render-target viewer and
// texture thumbnails. Sections tied to still-unported passes (shadow
// cascades, cloud/water/bloom tuning, GIBS) return with those ports.
void Renderer::drawGraphicsImGui() {
    if (!ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::Text("Average frame rate: %.3f ms/frame (%.1f FPS)",
                1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
    ImGui::ColorEdit3("Clear color", (float*)&clearColor);
    ImGui::Text("Drawables: %u / %u visible",
                lastFrameStats.visibleDrawables, lastFrameStats.totalDrawables);
    ImGui::Text("Scene lights: dir %u | point %u | rect %zu",
                lastFrameStats.directionalLights, lastFrameStats.pointLights, rectLights.size());
    ImGui::Text("Raytracing: %s | Compute: %s | GPU timestamps: %s",
                capabilities.raytracing ? "yes" : "no",
                capabilities.computeShaders ? "yes" : "no",
                capabilities.gpuTimestamps ? "yes" : "no");
    ImGui::Text("Frame: %u", frameNumber);

    // Render-target viewer
    if (ImGui::TreeNode("RTs")) {
        Uint32 w = rhi->getSwapchainWidth();
        Uint32 h = rhi->getSwapchainHeight();
        float aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        auto preview = [&](const char* label, TextureHandle tex) {
            if (!tex.isValid()) return;
            if (ImGui::TreeNode(label)) {
                ImGui::Text("%u x %u", w, h);
                if (void* id = getImGuiTextureID(tex)) {
                    ImGui::Image((ImTextureID)(intptr_t)id, ImVec2(320, 320 / aspect));
                } else {
                    ImGui::TextDisabled("(preview unavailable on this backend)");
                }
                ImGui::TreePop();
            }
        };
        preview("Color RT", colorRT);
        preview("Normal RT", normalRT);
        preview("Shadow RT", shadowRT);
        preview("AO RT", aoRT);
        ImGui::TreePop();
    }

    // Registered texture thumbnails (material maps etc.)
    if (ImGui::TreeNode("Textures")) {
        int shown = 0;
        for (size_t i = 0; i < textures.size() && shown < 64; i++) {
            const RenderTexture& tex = textures[i];
            if (!tex.handle.isValid()) continue;
            if (void* id = getImGuiTextureID(tex.handle)) {
                ImGui::Image((ImTextureID)(intptr_t)id, ImVec2(64, 64));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("#%zu  %u x %u", i, tex.width, tex.height);
                }
                shown++;
                if (shown % 8 != 0) ImGui::SameLine();
            }
        }
        ImGui::NewLine();
        ImGui::TreePop();
    }
}

void Renderer::drawRenderGraphImGui() {
    if (!ImGui::CollapsingHeader("Render Passes"))
        return;

    ImGui::Text("Backend features: raytracing=%s, compute=%s",
                capabilities.raytracing ? "yes" : "no",
                capabilities.computeShaders ? "yes" : "no");
    ImGui::Separator();

    for (const auto& pass : renderGraph.getPasses()) {
        if (!pass->isSupported(capabilities)) {
            ImGui::BeginDisabled();
            bool off = false;
            ImGui::Checkbox(pass->getName().c_str(), &off);
            ImGui::SameLine();
            ImGui::TextDisabled("(unsupported on this backend)");
            ImGui::EndDisabled();
        } else {
            ImGui::Checkbox(pass->getName().c_str(), &pass->enabled);
        }
    }
}

void Renderer::drawGpuTimingsImGui() {
    if (!ImGui::CollapsingHeader("GPU Pass Timings"))
        return;

    if (!rhi->isGpuTimingSupported()) {
        ImGui::TextDisabled("Not supported on this device");
        return;
    }

    bool enabled = rhi->isGpuTimingEnabled();
    if (ImGui::Checkbox("Enable##gpu_timing", &enabled))
        rhi->setGpuTimingEnabled(enabled);
    if (!enabled)
        return;

    auto timings = rhi->getGpuPassTimings();
    double totalMs = 0.0;
    double maxMs = 0.001;
    for (const auto& t : timings) {
        totalMs += t.gpuTimeMs;
        maxMs = std::max(maxMs, t.gpuTimeMs);
    }
    ImGui::Text("Total GPU: %.3f ms", totalMs);
    ImGui::Separator();
    if (ImGui::BeginTable("##gpu_pass_timings", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& t : timings) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(t.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", t.gpuTimeMs);
            ImGui::TableSetColumnIndex(2);
            ImGui::ProgressBar(static_cast<float>(t.gpuTimeMs / maxMs), ImVec2(-1.0f, 0.0f), "");
        }
        ImGui::EndTable();
    }
}

// ============================================================================
// Batch Rendering Implementation
// ============================================================================

void Renderer::initBatchRendering() {
    // Initialize batch2D
    batch2D.init(rhi.get(), backend, false, textures[defaultWhiteTexture].handle, defaultSampler);

    // Initialize batch3D
    batch3D.init(rhi.get(), backend, true, textures[defaultWhiteTexture].handle, defaultSampler);
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
    batch2D.setTexture(texture);
    batch2D.addQuad(glm::vec3(position, 0.0f), size, tintColor);
    batch2D.setTexture(TextureHandle{});
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
    // TODO: custom tex coords are not applied yet
    batch2D.setTexture(texture);
    batch2D.addQuad(transform, tintColor, entityID);
    batch2D.setTexture(TextureHandle{});
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
    batch3D.setTexture(texture);
    batch3D.addQuad(position, size, tintColor);
    batch3D.setTexture(TextureHandle{});
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
    // TODO: custom tex coords are not applied yet
    batch3D.setTexture(texture);
    batch3D.addQuad(transform, tintColor, entityID);
    batch3D.setTexture(TextureHandle{});
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
RHIBatch2DStats Renderer::getBatch2DStats() const {
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
    colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;  // Drawn as a texture afterwards
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
    passDesc.name = "RenderToTexture";
    passDesc.colorAttachments.push_back(resource.colorTexture);
    passDesc.clearColors.push_back(clearColor);
    passDesc.loadColor.push_back(false); // Clear, don't load

    if (resource.hasDepth && resource.depthTexture.isValid()) {
        passDesc.depthAttachment = resource.depthTexture;
        passDesc.clearDepth = 1.0f;
        passDesc.loadDepth = false; // Clear depth
    }
    rhi->beginRenderPass(passDesc);

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

void Renderer::BatchRenderer::init(RHI* rhi, GraphicsBackend backend, bool is3D, TextureHandle defaultTex, SamplerHandle samplerHandle) {
    whiteTexture = defaultTex;
    sampler = samplerHandle;

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
        vertShaderCode = readFile("shaders/RHIBatch.vert.spv");
        fragShaderCode = readFile("shaders/RHIBatch.frag.spv");
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
        {0, PixelFormat::RGB32_FLOAT, offsetof(Vertex2D, position)},    // vec3 position
        {1, PixelFormat::RGBA32_FLOAT, offsetof(Vertex2D, color)},      // vec4 color
        {2, PixelFormat::RG32_FLOAT, offsetof(Vertex2D, texCoord)},     // vec2 texCoord
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

    // One draw per texture segment (6 indices per quad, shared vertex data)
    for (const Segment& seg : segments) {
        if (seg.quadCount == 0) continue;
        TextureHandle tex = seg.texture.isValid() ? seg.texture : whiteTexture;
        if (tex.isValid() && sampler.isValid()) {
            rhi->setTexture(0, 0, tex, sampler);
        }
        rhi->drawIndexed(seg.quadCount * 6, 1, seg.quadStart * 6, 0, 0);
        drawCalls++;
    }
    totalQuads += quadCount;

    // Reset for next batch
    reset();
}

void Renderer::BatchRenderer::reset() {
    vertices.clear();
    indices.clear();
    quadCount = 0;
    segments.clear();
}

// Extend the current texture segment (or open a new one) to cover the quad
// that is about to be added.
void Renderer::BatchRenderer::accountQuadSegment() {
    TextureHandle want = pendingTexture.isValid() ? pendingTexture : whiteTexture;
    if (segments.empty() || segments.back().texture.id != want.id) {
        segments.push_back({ want, quadCount, 0 });
    }
    segments.back().quadCount++;
}

void Renderer::BatchRenderer::setTexture(TextureHandle texture) {
    // Normalize "no texture" to the white texture so comparisons are stable.
    // Only recorded — the segment split happens when the next quad arrives.
    pendingTexture = texture.isValid() ? texture : whiteTexture;
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

    accountQuadSegment();
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

    accountQuadSegment();
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

    accountQuadSegment();
    quadCount++;
}
