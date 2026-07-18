#include <algorithm>
#include <memory>
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "components.hpp"

// NOTE: `using namespace Vapor;` is intentionally placed AFTER all includes
// (see below). The branch's graphics.hpp defines Vapor:: copies of the GPU
// structs that also exist at global scope in graphics_gpu_structs.hpp; if the
// using-directive were active while those headers parse, their own unqualified
// references (e.g. InstanceData::primitiveMode) would be ambiguous.
#include "debug_draw.hpp"
#include "graphics_mesh.hpp"
#include "renderer_metal.hpp"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <fmt/core.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <tracy/Tracy.hpp>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#include "backends/imgui_impl_metal.h"
#include "backends/imgui_impl_sdl3.h"
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <set>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>
#include <vector>

#include "Vapor/rml_renderer_metal.hpp"
#include "asset_manager.hpp"
#include "engine_core.hpp"
#include "graphics.hpp"
// The branch's graphics.hpp is a monolith (not an umbrella), so pull in the
// effect/batch/gibs sub-headers the native Metal renderer needs directly.
#include "graphics_effects.hpp"   // AtmosphereData, WaterData, GPUParticle, ::Particle, …
#include "graphics_batch2d.hpp"   // Batch2DVertex, Batch2DBlendMode
#include "graphics_gibs.hpp"      // Surfel, SurfelCell, GIBSData
#include "helper.hpp"
#include "mesh_builder.hpp"
#include "rmlui_manager.hpp"

#include <RmlUi/Core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// GIBS (Global Illumination Based on Surfels)
#include "Vapor/gibs_manager.hpp"

// All headers are parsed above with no using-directive active, so their own
// unqualified references bind to the intended (global gpu_structs) types. The
// renderer body below uses Vapor:: types unqualified; GPU-struct names are
// qualified with :: where the global versions are required.
using namespace Vapor;

// Pre-pass: Renders depth and normals
class PrePass : public MetalRenderPass {
public:
    explicit PrePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "PrePass";
    }

    void execute() override {
        auto& r = *renderer;

        // Create render pass descriptor
        auto prePassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());

        // Color attachment 0: Normal
        auto prePassNormalRT = prePassDesc->colorAttachments()->object(0);
        prePassNormalRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        prePassNormalRT->setLoadAction(MTL::LoadActionClear);
        prePassNormalRT->setStoreAction(MTL::StoreActionStore);
        prePassNormalRT->setTexture(r.normalRT_MS.get());

        // Color attachment 1: Albedo (for GIBS)
        auto prePassAlbedoRT = prePassDesc->colorAttachments()->object(1);
        prePassAlbedoRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        prePassAlbedoRT->setLoadAction(MTL::LoadActionClear);
        prePassAlbedoRT->setStoreAction(MTL::StoreActionMultisampleResolve);
        prePassAlbedoRT->setTexture(r.albedoRT_MS.get());
        prePassAlbedoRT->setResolveTexture(r.albedoRT.get());

        auto prePassDepthRT = prePassDesc->depthAttachment();
        prePassDepthRT->setClearDepth(r.clearDepth);
        prePassDepthRT->setLoadAction(MTL::LoadActionClear);
        prePassDepthRT->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
        prePassDepthRT->setDepthResolveFilter(MTL::MultisampleDepthResolveFilter::MultisampleDepthResolveFilterMin);
        prePassDepthRT->setTexture(r.depthStencilRT_MS.get());
        prePassDepthRT->setResolveTexture(r.depthStencilRT.get());

        // Execute the pass
        applyTimingToRenderDesc(prePassDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(prePassDesc.get());
        encoder->setRenderPipelineState(r.prePassPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.depthStencilState.get());

        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.materialDataBuffer.get(), 0, 1);
        encoder->setVertexBuffer(r.instanceDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setVertexBuffer(r.getBuffer(r.currentScene->vertexBuffer).get(), 0, 3);

        for (const auto& [material, draws] : r.instanceBatches) {
            encoder->setFragmentTexture(
                r.getTexture(material->albedoMap ? material->albedoMap->texture : r.defaultAlbedoTexture).get(), 0
            );

            for (const auto& draw : draws) {
                if (!r.currentCamera->isVisible(r.instances[draw.instanceIndex].boundingSphere)) {
                    continue;
                }
                encoder->setVertexBytes(&draw.instanceIndex, sizeof(Uint32), 4);
                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    draw.mesh->indexCount,
                    MTL::IndexTypeUInt32,
                    r.getBuffer(r.currentScene->indexBuffer).get(),
                    draw.mesh->indexOffset * sizeof(Uint32)
                );
                r.drawCount++;
            }
        }

        encoder->endEncoding();
    }
};

// TLAS build pass: Builds top-level acceleration structure for ray tracing
class TLASBuildPass : public MetalRenderPass {
public:
    explicit TLASBuildPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "TLASBuildPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Build BLAS array if geometry is dirty
        if (r.currentScene->isGeometryDirty) {
            std::vector<NS::Object*> blasObjects;
            blasObjects.reserve(r.BLASs.size());
            for (auto blas : r.BLASs) {
                blasObjects.push_back(static_cast<NS::Object*>(blas.get()));
            }
            r.BLASArray = NS::TransferPtr(NS::Array::array(blasObjects.data(), blasObjects.size()));
            r.currentScene->isGeometryDirty = false;
        }

        auto slot = r.currentFrameInFlight;

        // Create TLAS descriptor (built every frame, matching mainline behavior;
        // skip/refit optimizations were backed out to shrink this change set)
        auto tlasDesc = NS::TransferPtr(MTL::InstanceAccelerationStructureDescriptor::alloc()->init());
        tlasDesc->setInstanceCount(r.accelInstances.size());
        tlasDesc->setInstancedAccelerationStructures(r.BLASArray.get());
        tlasDesc->setInstanceDescriptorBuffer(r.accelInstanceBuffers[slot].get());

        auto tlasSizes = r.device->accelerationStructureSizes(tlasDesc.get());
        if (!r.TLASScratchBuffers[slot]
            || r.TLASScratchBuffers[slot]->length() < tlasSizes.buildScratchBufferSize) {
            r.TLASScratchBuffers[slot] = NS::TransferPtr(
                r.device->newBuffer(tlasSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate)
            );
        }
        if (!r.TLASBuffers[slot] || r.TLASBuffers[slot]->size() < tlasSizes.accelerationStructureSize) {
            r.TLASBuffers[slot] =
                NS::TransferPtr(r.device->newAccelerationStructure(tlasSizes.accelerationStructureSize));
        }

        auto timedAccelDesc = makeTimedAccelDesc(true, true);
        auto accelEncoder = r.currentCommandBuffer->accelerationStructureCommandEncoder(timedAccelDesc.get());
        accelEncoder->buildAccelerationStructure(
            r.TLASBuffers[slot].get(),
            tlasDesc.get(),
            r.TLASScratchBuffers[slot].get(),
            0
        );
        accelEncoder->endEncoding();
    }
};

// Normal resolve pass: Resolves MSAA normal texture
class NormalResolvePass : public MetalRenderPass {
public:
    explicit NormalResolvePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "NormalResolvePass";
    }

    void execute() override {
        auto& r = *renderer;

        auto w = r.normalRT->width();
        auto h = r.normalRT->height();

        auto timedComputeDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(r.normalResolvePipeline.get());
        encoder->setTexture(r.normalRT_MS.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setBytes(&r.MSAA_SAMPLE_COUNT, sizeof(Uint32), 0);
        encoder->dispatchThreadgroups(MTL::Size(w, h, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();

        // Traffic: read all MS normal samples, write resolved normal (both RGBA16F)
        uint64_t px = uint64_t(w) * h;
        addTrafficEstimate(px * 8 * (r.MSAA_SAMPLE_COUNT + 1));
    }
};

// Velocity pass: camera-motion vectors from the depth buffer (see 3d_velocity.metal).
// Feeds every temporal technique (RT AO temporal accumulation, TAA).
class VelocityPass : public MetalRenderPass {
public:
    explicit VelocityPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "VelocityPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.velocityPipeline || !r.velocityRT) return;

        glm::mat4 curViewProj = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        if (!r.prevViewProjValid) {
            r.prevViewProj = curViewProj; // first frame: zero velocity
            r.prevViewProjValid = true;
        }

        auto timedComputeDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(r.velocityPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.velocityRT.get(), 1);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBytes(&r.prevViewProj, sizeof(glm::mat4), 1);
        auto w = r.velocityRT->width();
        auto h = r.velocityRT->height();
        encoder->dispatchThreads(MTL::Size(w, h, 1), MTL::Size(8, 8, 1));
        encoder->endEncoding();

        // Traffic: depth read (4B) + velocity write (4B) per pixel
        addTrafficEstimate(uint64_t(w) * h * 8);

        // setBytes copied prevViewProj into the command stream, so it's safe to roll forward now
        r.prevViewProj = curViewProj;
    }
};

// Tile culling pass: Performs light culling for tiled rendering
class TileCullingPass : public MetalRenderPass {
public:
    explicit TileCullingPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "TileCullingPass";
    }

    void execute() override {
        auto& r = *renderer;

        glm::vec2 screenSize = glm::vec2(r.colorRT->width(), r.colorRT->height());
        glm::uvec3 gridSize = glm::uvec3(r.clusterGridSizeX, r.clusterGridSizeY, r.clusterGridSizeZ);
        uint pointLightCount = r.currentScene->pointLights.size();

        auto timedComputeDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(r.tileCullingPipeline.get());
        encoder->setBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 1);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setBytes(&pointLightCount, sizeof(uint), 3);
        encoder->setBytes(&gridSize, sizeof(glm::uvec3), 4);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 5);
        encoder->dispatchThreadgroups(MTL::Size(r.clusterGridSizeX, r.clusterGridSizeY, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();
    }
};

// Raytrace shadow pass: Computes ray-traced shadows
class RaytraceShadowPass : public MetalRenderPass {
public:
    explicit RaytraceShadowPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "RaytraceShadowPass";
    }

    void execute() override {
        auto& r = *renderer;

        auto w = r.shadowRT->width();
        auto h = r.shadowRT->height();
        glm::vec2 screenSize = glm::vec2(w, h);

        auto timedComputeDesc = makeTimedComputeDesc(true, false);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(r.raytraceShadowPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.shadowRT.get(), 2);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.directionalLightBuffer.get(), 0, 1);
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 2);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 3);
        encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 4);
        encoder->dispatchThreadgroups(MTL::Size(w, h, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();

        // Generate mipmaps for shadow texture (restored while bisecting the
        // shadow visual regression)
        auto shadowBlitDesc = makeTimedBlitDesc(false, true);
        auto mipmapEncoder = NS::TransferPtr(r.currentCommandBuffer->blitCommandEncoder(shadowBlitDesc.get()));
        mipmapEncoder->generateMipmaps(r.shadowRT.get());
        mipmapEncoder->endEncoding();

        // Traffic: depth read (4B) + normal read (8B) + shadow write (4B) per pixel,
        // plus mip chain regeneration (~5/3 of the base level)
        uint64_t px = uint64_t(w) * h;
        addTrafficEstimate(px * (4 + 8 + 4) + px * 4 * 5 / 3);
    }
};

// Screen-space contact shadows: march the depth buffer toward the sun and write
// a visibility RT the PBR pass min-composites onto the directional shadow. Adds
// the fine near contact the RT/PSSM shadow misses (parity with the Vulkan path).
class SSCSPass : public MetalRenderPass {
public:
    explicit SSCSPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {}
    auto getName() const -> const char* override { return "SSCSPass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.sscsEnabled || !r.sscsPipeline || !r.sscsRT || !r.depthStencilRT) return;

        auto w = r.sscsRT->width();
        auto h = r.sscsRT->height();
        glm::vec2 screenSize = glm::vec2(w, h);
        struct SSCSParams { float rayLength; float thickness; uint32_t stepCount; float bias; } params;
        params.rayLength = r.sscsLength;
        params.thickness = r.sscsThickness;
        params.stepCount = r.sscsSteps;
        params.bias      = r.sscsBias;

        auto timedComputeDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(r.sscsPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.sscsRT.get(), 1);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.directionalLightBuffer.get(), 0, 1);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 2);
        encoder->setBytes(&params, sizeof(params), 3);
        encoder->dispatchThreadgroups(MTL::Size((w + 7) / 8, (h + 7) / 8, 1), MTL::Size(8, 8, 1));
        encoder->endEncoding();

        addTrafficEstimate(uint64_t(w) * h * (4 + 1));
    }
};

// PSSM shadow pass: renders scene depth into a 3-slice texture array for cascades 1-3
class PSSMShadowPass : public MetalRenderPass {
public:
    explicit PSSMShadowPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "PSSMShadowPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.currentScene || r.currentScene->directionalLights.empty()) return;

        const auto& sunLight = r.currentScene->directionalLights[0];
        const float nearClip  = r.currentCamera->near();
        const float farClip   = r.currentCamera->far();
        const float rtEnd     = r.m_supportsRaytracing ? r.pssmRTMaxDist : nearClip;
        const float blendRange = (farClip - rtEnd) * r.pssmRTBlendScale; // fraction of remaining range

        // ----- Cascade split depths (practical split: blend of log + uniform) -----
        // splits[0] = rtEnd, splits[1..3] = cascade ends, splits[3] = farClip
        float splits[4];
        splits[0] = rtEnd;
        const float lambda = 0.7f;
        for (int i = 1; i <= 3; i++) {
            float p = float(i) / 3.0f;
            float logS = rtEnd * std::pow(farClip / std::max(rtEnd, 0.1f), p);
            float uniS = rtEnd + (farClip - rtEnd) * p;
            splits[i] = lambda * logS + (1.0f - lambda) * uniS;
        }

        // ----- Light direction (direction light travels, toward scene) -----
        glm::vec3 lightDir = glm::normalize(sunLight.direction);
        glm::vec3 up = (std::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) < 0.99f)
                     ? glm::vec3(0, 1, 0)
                     : glm::vec3(0, 0, 1);

        // ----- Frustum corners in world space -----
        glm::mat4 view      = r.currentCamera->getViewMatrix();
        glm::mat4 proj      = r.currentCamera->getProjMatrix();
        glm::mat4 invVP     = glm::inverse(proj * view);

        // NDC corners (LH ZO: z in [0,1], y in [-1,+1])
        const glm::vec4 ndcCorners[8] = {
            {-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{1,1,0,1},
            {-1,-1,1,1},{1,-1,1,1},{-1,1,1,1},{1,1,1,1},
        };
        glm::vec3 worldCorners[8];
        for (int i = 0; i < 8; i++) {
            glm::vec4 w = invVP * ndcCorners[i];
            worldCorners[i] = glm::vec3(w) / w.w;
        }

        // ----- Build GPU data struct -----
        struct PSSMGPUData {
            glm::mat4 lightSpaceMatrices[3];
            glm::vec4 cascadeSplits;
            float blendRange;          // RT↔PSSM blend range
            float cascadeBlendRange;   // cascade↔cascade blend range
            uint32_t pcfSampleCount;   // 4, 8, 16, or 32
            uint32_t debugVisualize;   // 0 = off, 1 = visualize cascades
        };

        PSSMGPUData gpuData{};
        gpuData.cascadeSplits = glm::vec4(splits[0], splits[1], splits[2], splits[3]);
        gpuData.blendRange         = blendRange;
        gpuData.cascadeBlendRange  = r.pssmCascadeBlendRange;
        gpuData.pcfSampleCount     = r.pssmPcfSampleCount;
        gpuData.debugVisualize     = r.pssmDebugVisualize ? 1 : 0;

        // Convert a forward view-space distance to NDC z by projecting an actual
        // view-space point. Handedness-aware: RH projections (glm default; the
        // GLM_FORCE_LEFT_HANDED define never took effect because glm is included
        // transitively before it) have proj[2][3] == -1 and put visible geometry
        // at negative view z; LH puts it at positive z.
        const float zSign = (proj[2][3] < 0.0f) ? -1.0f : 1.0f;
        auto viewDepthToNDCz = [&](float d) -> float {
            glm::vec4 clip = proj * glm::vec4(0.0f, 0.0f, zSign * d, 1.0f);
            return clip.z / clip.w;
        };

        for (int ci = 0; ci < 3; ci++) {
            // Clamp split depths to valid [near, far] range before converting to NDC
            float splitNear = glm::clamp(splits[ci],     nearClip, farClip);
            float splitFar  = glm::clamp(splits[ci + 1], nearClip, farClip);

            float nearNDCz = viewDepthToNDCz(splitNear);
            float farNDCz  = viewDepthToNDCz(splitFar);

            // Sub-frustum corners: unproject 8 NDC corners at exact cascade z values
            const glm::vec4 cascadeNDC[8] = {
                {-1,-1,nearNDCz,1}, {1,-1,nearNDCz,1}, {-1,1,nearNDCz,1}, {1,1,nearNDCz,1},
                {-1,-1,farNDCz, 1}, {1,-1,farNDCz, 1}, {-1,1,farNDCz, 1}, {1,1,farNDCz, 1},
            };
            glm::vec3 corners[8];
            for (int i = 0; i < 8; i++) {
                glm::vec4 w = invVP * cascadeNDC[i];
                corners[i] = glm::vec3(w) / w.w;
            }

            // Bounding sphere center for stable (rotation-invariant) shadow map
            glm::vec3 sphereCenter(0.0f);
            for (auto& c : corners) sphereCenter += c;
            sphereCenter /= 8.0f;

            float sphereRadius = 0.0f;
            for (auto& c : corners)
                sphereRadius = glm::max(sphereRadius, glm::length(c - sphereCenter));
            // Quantize the radius so texelSize doesn't jitter from per-frame float noise
            sphereRadius = std::ceil(sphereRadius * 16.0f) / 16.0f;

            // Snap the cascade center to the shadow-map texel grid so the map
            // translates in whole texels as the camera moves (stops edge
            // crawling/swimming). The snap MUST happen in a world-anchored,
            // rotation-only light frame: the previous code snapped
            // lightView * sphereCenter, but that view looks AT sphereCenter, so
            // the product is always (0, 0, -lightDist) and the snap was a no-op.
            const glm::mat4 lightRot = glm::lookAt(glm::vec3(0.0f), lightDir, up);
            const float texelSize = (2.0f * sphereRadius) / float(r.PSSM_SHADOW_MAP_SIZE);
            glm::vec3 lsC = glm::vec3(lightRot * glm::vec4(sphereCenter, 1.0f));
            lsC.x = std::floor(lsC.x / texelSize) * texelSize;
            lsC.y = std::floor(lsC.y / texelSize) * texelSize;
            const glm::vec3 snappedCenter = glm::vec3(glm::inverse(lightRot) * glm::vec4(lsC, 1.0f));

            // Light view: eye pulled back far enough that the whole bounding
            // sphere sits in front of it (RH lookAt: forward is -z, so points in
            // front have negative view z; distance from eye = -z).
            const float lightDist = sphereRadius * 2.0f + 1.0f;
            glm::mat4 lightView = glm::lookAt(snappedCenter - lightDir * lightDist, snappedCenter, up);

            // Depth extents as positive distances in front of the light eye
            float minDist = 1e38f, maxDist = -1e38f;
            for (auto& c : corners) {
                float dist = -(lightView * glm::vec4(c, 1.0f)).z; // RH: -z is forward
                minDist = glm::min(minDist, dist);
                maxDist = glm::max(maxDist, dist);
            }
            // Pull the near plane back to capture shadow casters outside the sub-frustum
            // (a negative near just extends the ortho volume behind the eye — valid).
            minDist -= (maxDist - minDist);

            // orthoZO (not plain ortho): the camera uses perspectiveZO and Metal's
            // depth buffer is [0,1], but glm::ortho here follows the GL [-1,1]
            // convention unless GLM_FORCE_DEPTH_ZERO_TO_ONE took effect — which is
            // unreliable in this TU (its sibling GLM_FORCE_LEFT_HANDED did not).
            // Match the Vulkan path explicitly. RH (LEFT_HANDED inert) pairs with
            // the RH lightView above.
            glm::mat4 lightProj = glm::orthoZO(
                -sphereRadius,  sphereRadius,
                -sphereRadius,  sphereRadius,
                minDist, maxDist
            );
            gpuData.lightSpaceMatrices[ci] = lightProj * lightView;
        }

        // Upload PSSM data to triple-buffered GPU buffer
        memcpy(r.pssmDataBuffers[r.currentFrameInFlight]->contents(), &gpuData, sizeof(gpuData));
        r.pssmDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, sizeof(gpuData))
        );

        // ----- Render scene into each cascade shadow map slice -----
        for (int ci = 0; ci < 3; ci++) {
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto depthAtt = passDesc->depthAttachment();
            depthAtt->setTexture(r.pssmShadowMaps.get());
            depthAtt->setSlice(NS::UInteger(ci));
            depthAtt->setLoadAction(MTL::LoadActionClear);
            depthAtt->setStoreAction(MTL::StoreActionStore);
            depthAtt->setClearDepth(1.0);

            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.pssmShadowPipeline.get());
            encoder->setDepthStencilState(r.pssmDepthStencilState.get());
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
            // Slope-scale depth bias to prevent shadow acne
            encoder->setDepthBias(0.005f, 2.0f, 0.01f);

            glm::mat4 lsm = gpuData.lightSpaceMatrices[ci];
            encoder->setVertexBytes(&lsm, sizeof(glm::mat4), 0);
            encoder->setVertexBuffer(r.materialDataBuffer.get(), 0, 1);
            encoder->setVertexBuffer(r.instanceDataBuffers[r.currentFrameInFlight].get(), 0, 2);
            encoder->setVertexBuffer(r.getBuffer(r.currentScene->vertexBuffer).get(), 0, 3);

            for (const auto& [material, draws] : r.instanceBatches) {
                encoder->setFragmentTexture(
                    r.getTexture(material->albedoMap ? material->albedoMap->texture : r.defaultAlbedoTexture).get(), 0
                );
                for (const auto& draw : draws) {
                    encoder->setVertexBytes(&draw.instanceIndex, sizeof(Uint32), 4);
                    encoder->drawIndexedPrimitives(
                        MTL::PrimitiveTypeTriangle,
                        draw.mesh->indexCount,
                        MTL::IndexTypeUInt32,
                        r.getBuffer(r.currentScene->indexBuffer).get(),
                        draw.mesh->indexOffset * sizeof(Uint32)
                    );
                }
            }
            encoder->endEncoding();
        }
    }
};

// PSSM resolve pass (debug): writes the directional shadow factor into a
// camera-aligned screen-space texture so it can be inspected like RT shadow.
class PSSMResolvePass : public MetalRenderPass {
public:
    explicit PSSMResolvePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {}
    auto getName() const -> const char* override { return "PSSMResolvePass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.currentScene || r.currentScene->directionalLights.empty()) return;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        auto timedDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setComputePipelineState(r.pssmResolvePipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.pssmShadowMaps.get(), 1);
        encoder->setTexture(r.pssmShadowScreenRT.get(), 2);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.pssmDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 2);
        encoder->dispatchThreadgroups(
            MTL::Size((uint32_t(screenSize.x) + 7) / 8, (uint32_t(screenSize.y) + 7) / 8, 1),
            MTL::Size(8, 8, 1)
        );
        encoder->endEncoding();
    }
};

// Stochastic point shadow pass: MegaLights-style 2-ray shadow for clustered point lights
class StochasticPointShadowPass : public MetalRenderPass {
public:
    explicit StochasticPointShadowPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {}
    auto getName() const -> const char* override { return "StochasticPointShadowPass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.m_supportsRaytracing || !r.TLASBuffers[r.currentFrameInFlight]) return;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        glm::uvec3 gridDims = glm::uvec3(r.clusterGridSizeX, r.clusterGridSizeY, r.clusterGridSizeZ);
        uint32_t fi = r.frameNumber;

        auto timedDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setComputePipelineState(r.stochasticPointShadowPipeline.get());
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.pointShadowRT.get(), 2);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 1);
        encoder->setBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setBytes(&screenSize, sizeof(glm::vec2), 3);
        encoder->setBytes(&gridDims, sizeof(glm::uvec3), 4);
        encoder->setBytes(&fi, sizeof(uint32_t), 5);
        encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 6);
        encoder->setBytes(&r.pointShadowDebugMode, sizeof(uint32_t), 7);
        // The shared kernel now takes spot/rect shadow inputs at 8-10. Native
        // has no spot lights and keeps R16F targets, so bind placeholders with
        // zero counts — the kernel then leaves the extra channels at 1.0 and
        // this path stays byte-for-byte.
        encoder->setBuffer(r.pointLightBuffer.get(), 0, 8);   // placeholder (unread)
        encoder->setBuffer(r.rectLightBuffer.get(), 0, 9);
        glm::uvec2 extraCounts(0u, 0u);
        encoder->setBytes(&extraCounts, sizeof(glm::uvec2), 10);
        encoder->dispatchThreadgroups(
            MTL::Size((uint32_t(screenSize.x) + 7) / 8, (uint32_t(screenSize.y) + 7) / 8, 1),
            MTL::Size(8, 8, 1)
        );
        encoder->endEncoding();
    }
};

// Point shadow temporal pass: motion-vector reprojection + variance clamping denoiser
class PointShadowTemporalPass : public MetalRenderPass {
public:
    explicit PointShadowTemporalPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {}
    auto getName() const -> const char* override { return "PointShadowTemporalPass"; }

    void execute() override {
        auto& r = *renderer;
        auto drawableSize = r.swapchain->drawableSize();

        auto timedDesc = makeTimedComputeDesc(true, false);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setComputePipelineState(r.pointShadowTemporalPipeline.get());
        encoder->setTexture(r.pointShadowRT.get(), 0);
        encoder->setTexture(r.pointShadowHistoryRT.get(), 1);
        encoder->setTexture(r.velocityRT.get(), 2);
        encoder->setTexture(r.pointShadowDenoisedRT.get(), 3);
        encoder->dispatchThreadgroups(
            MTL::Size((uint32_t(drawableSize.width) + 7) / 8, (uint32_t(drawableSize.height) + 7) / 8, 1),
            MTL::Size(8, 8, 1)
        );
        encoder->endEncoding();

        // Copy denoised result into history for next frame via blit
        auto blitDesc = makeTimedBlitDesc(false, true);
        auto blit = NS::TransferPtr(r.currentCommandBuffer->blitCommandEncoder(blitDesc.get()));
        blit->copyFromTexture(r.pointShadowDenoisedRT.get(), r.pointShadowHistoryRT.get());
        blit->endEncoding();
    }
};

// Raytrace AO pass: Computes ray-traced ambient occlusion
class RaytraceAOPass : public MetalRenderPass {
public:
    explicit RaytraceAOPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "RaytraceAOPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.aoEnabled) return;

        // Both raygen kernels share the binding interface (SSAO ignores the TLAS slot)
        auto* pipeline = (r.aoMethod == 0 && r.raytraceAOPipeline) ? r.raytraceAOPipeline.get() : r.ssaoPipeline.get();
        if (!pipeline) return;

        auto timedComputeDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(pipeline);
        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.aoRawRT.get(), 2); // noisy output; temporal + à-trous passes produce aoRT
        encoder->setBuffer(r.frameDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 2);
        auto w = r.aoRT->width();
        auto h = r.aoRT->height();
        encoder->dispatchThreadgroups(MTL::Size(w, h, 1), MTL::Size(1, 1, 1));
        encoder->endEncoding();

        // Traffic: depth read (4B) + normal read (8B) + AO write (2B) per half-res pixel.
        // BVH traversal traffic is not estimable here.
        addTrafficEstimate(uint64_t(w) * h * (4 + 8 + 2));
    }
};

// AO temporal accumulation: reprojects last frame's AO with the velocity
// buffer and blends it with the raygen output (ADR-008 step 2).
class AOTemporalPass : public MetalRenderPass {
public:
    explicit AOTemporalPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "AOTemporalPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.aoTemporalPipeline || !r.aoEnabled) return;

        glm::mat4 curView = r.currentCamera->getViewMatrix();
        if (!r.prevViewValid) {
            r.prevView = curView;
            r.prevViewValid = true;
        }
        uint32_t historyValid = r.aoHistoryValid ? 1u : 0u;
        uint32_t inIdx = r.aoHistoryIndex;
        uint32_t outIdx = inIdx ^ 1u;

        auto timedComputeDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(r.aoTemporalPipeline.get());
        encoder->setTexture(r.aoRawRT.get(), 0);
        encoder->setTexture(r.aoHistoryRT[inIdx].get(), 1);
        encoder->setTexture(r.aoHistoryRT[outIdx].get(), 2);
        encoder->setTexture(r.velocityRT.get(), 3);
        encoder->setTexture(r.depthStencilRT.get(), 4);
        encoder->setTexture(r.normalRT.get(), 5);
        encoder->setBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setBytes(&r.prevView, sizeof(glm::mat4), 1);
        encoder->setBytes(&historyValid, sizeof(uint32_t), 2);
        auto w = r.aoRawRT->width();
        auto h = r.aoRawRT->height();
        encoder->dispatchThreads(MTL::Size(w, h, 1), MTL::Size(8, 8, 1));
        encoder->endEncoding();

        // Traffic: raw AO (2B) + history in/out (8B each) + velocity (4B) + depth (4B) + normal (8B)
        addTrafficEstimate(uint64_t(w) * h * (2 + 8 + 8 + 4 + 4 + 8));

        r.aoHistoryIndex = outIdx;
        r.aoHistoryValid = true;
        r.prevView = curView; // setBytes copied the old value into the command stream
    }
};

// AO spatial denoise: edge-aware à-trous iterations, history → scratch → aoRT
// (ADR-008 step 3). aoRT is what the lighting/post passes consume. One serial
// compute encoder for all iterations: successive dispatches are ordered and
// their writes visible to each other.
class AODenoisePass : public MetalRenderPass {
public:
    explicit AODenoisePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "AODenoisePass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.aoDenoisePipeline || !r.aoEnabled) return;

        auto w = r.aoRT->width();
        auto h = r.aoRT->height();
        struct Iteration {
            MTL::Texture* src;
            MTL::Texture* dst;
            uint32_t stride;
        };
        const Iteration iterations[] = {
            { r.aoHistoryRT[r.aoHistoryIndex].get(), r.aoScratchRT.get(), 1u },
            { r.aoScratchRT.get(), r.aoRT.get(), 2u }, // final target is single-channel; extras dropped here
        };
        constexpr size_t iterationCount = sizeof(iterations) / sizeof(iterations[0]);
        auto timedComputeDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());
        encoder->setComputePipelineState(r.aoDenoisePipeline.get());
        for (size_t i = 0; i < iterationCount; i++) {
            encoder->setTexture(iterations[i].src, 0);
            encoder->setTexture(iterations[i].dst, 1);
            encoder->setBytes(&iterations[i].stride, sizeof(uint32_t), 0);
            encoder->dispatchThreads(MTL::Size(w, h, 1), MTL::Size(8, 8, 1));
        }
        encoder->endEncoding();

        // Traffic per iteration: 25 src taps (8B) + write (8B),
        // issued reads — caches make real DRAM traffic much lower
        addTrafficEstimate(uint64_t(w) * h * (25 * 8 + 8) * iterationCount);
    }
};

// Sky atmosphere pass: Renders procedural sky with Rayleigh and Mie scattering
class SkyAtmospherePass : public MetalRenderPass {
public:
    explicit SkyAtmospherePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "SkyAtmospherePass";
    }

    void execute() override {
        auto& r = *renderer;

        // Create render pass descriptor - render to color RT with blending
        auto skyPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto skyPassColorRT = skyPassDesc->colorAttachments()->object(0);
        skyPassColorRT->setLoadAction(MTL::LoadActionLoad);// Preserve existing scene content
        skyPassColorRT->setStoreAction(MTL::StoreActionStore);
        skyPassColorRT->setTexture(r.colorRT.get());

        // Set depth attachment - use resolved depth from MainRenderPass
        auto skyPassDepthRT = skyPassDesc->depthAttachment();
        skyPassDepthRT->setLoadAction(MTL::LoadActionLoad);
        skyPassDepthRT->setStoreAction(MTL::StoreActionDontCare);// Don't write depth
        skyPassDepthRT->setTexture(r.depthStencilRT.get());

        // Execute the pass
        applyTimingToRenderDesc(skyPassDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(skyPassDesc.get());
        encoder->setRenderPipelineState(r.atmospherePipeline.get());
        encoder->setCullMode(MTL::CullModeNone);

        // Use hardware depth test: only render sky where depth == 1.0 (far plane, no geometry)
        // CompareFunctionEqual: sky depth (1.0) == depth buffer (1.0) -> pass, render sky
        //                      sky depth (1.0) == depth buffer (0.5) -> fail, don't render (preserves scene)
        MTL::DepthStencilDescriptor* skyDepthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        skyDepthDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionEqual
        );// Only pass when depth == 1.0 (far plane)
        skyDepthDesc->setDepthWriteEnabled(false);// Don't write depth for sky
        NS::SharedPtr<MTL::DepthStencilState> skyDepthState =
            NS::TransferPtr(r.device->newDepthStencilState(skyDepthDesc));
        skyDepthDesc->release();
        encoder->setDepthStencilState(skyDepthState.get());

        // Set buffers
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.atmosphereDataBuffer.get(), 0, 1);

        // Draw full-screen triangle
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Equirect-to-cubemap pass: Converts a loaded equirectangular HDRI texture to environmentCubemap
class EquirectToCubemapPass : public MetalRenderPass {
public:
    explicit EquirectToCubemapPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {}
    auto getName() const -> const char* override { return "EquirectToCubemapPass"; }

    void execute() override {
        auto& r = *renderer;

        if (r.iblSource != Renderer_Metal::IBLSource::HDRI) return;
        if (!r.iblNeedsUpdate) return;
        if (!r.equirectHDRITexture) return;

        for (uint32_t face = 0; face < 6; ++face) {
            auto* captureData = reinterpret_cast<::IBLCaptureData*>(r.iblCaptureDataBuffer->contents());
            captureData->faceIndex = face;
            captureData->roughness = 0.0f;
            r.iblCaptureDataBuffer->didModifyRange(NS::Range::Make(0, r.iblCaptureDataBuffer->length()));

            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = passDesc->colorAttachments()->object(0);
            colorAttachment->setLoadAction(MTL::LoadActionClear);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorAttachment->setTexture(r.environmentCubemap.get());
            colorAttachment->setSlice(face);
            colorAttachment->setLevel(0);

            applyTimingToRenderDesc(passDesc.get(), face == 0, false);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.equirectToCubemapPipeline.get());
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setVertexBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
            encoder->setFragmentTexture(r.equirectHDRITexture.get(), 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }

        // Generate mipmaps for prefiltering
        auto equirectBlitDesc = makeTimedBlitDesc(false, true);
        auto blitEncoder = r.currentCommandBuffer->blitCommandEncoder(equirectBlitDesc.get());
        blitEncoder->generateMipmaps(r.environmentCubemap.get());
        blitEncoder->endEncoding();
    }
};

// Sky capture pass: Captures atmosphere to environment cubemap for IBL
class SkyCapturePass : public MetalRenderPass {
public:
    explicit SkyCapturePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "SkyCapturePass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;
        if (r.iblSource != Renderer_Metal::IBLSource::Sky) return;

        // Cubemap face view matrices (looking outward from origin)
        const glm::mat4 captureViews[6] = {
            glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)),// +X
            glm::lookAt(glm::vec3(0), glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)),// -X
            glm::lookAt(glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),// +Y
            glm::lookAt(glm::vec3(0), glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)),// -Y
            glm::lookAt(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(0, -1, 0)),// +Z
            glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)),// -Z
        };
        const glm::mat4 captureProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

        // Render each face of the cubemap
        for (uint32_t face = 0; face < 6; ++face) {
            // Update capture data
            auto* captureData = reinterpret_cast<::IBLCaptureData*>(r.iblCaptureDataBuffer->contents());
            captureData->viewProj = captureProj * captureViews[face];
            captureData->faceIndex = face;
            captureData->roughness = 0.0f;
            r.iblCaptureDataBuffer->didModifyRange(NS::Range::Make(0, r.iblCaptureDataBuffer->length()));

            // Create render pass for this face
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = passDesc->colorAttachments()->object(0);
            colorAttachment->setLoadAction(MTL::LoadActionClear);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorAttachment->setTexture(r.environmentCubemap.get());
            colorAttachment->setSlice(face);
            colorAttachment->setLevel(0);

            applyTimingToRenderDesc(passDesc.get(), face == 0, false);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.skyCapturePipeline.get());
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setVertexBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
            encoder->setFragmentBuffer(r.atmosphereDataBuffer.get(), 0, 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }

        // Generate mipmaps for environment cubemap
        auto skyBlitDesc = makeTimedBlitDesc(false, true);
        auto blitEncoder = r.currentCommandBuffer->blitCommandEncoder(skyBlitDesc.get());
        blitEncoder->generateMipmaps(r.environmentCubemap.get());
        blitEncoder->endEncoding();
    }
};

// Irradiance convolution pass: Creates diffuse irradiance map from environment cubemap
class IrradianceConvolutionPass : public MetalRenderPass {
public:
    explicit IrradianceConvolutionPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "IrradianceConvolutionPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;

        // Render each face of the irradiance cubemap
        for (uint32_t face = 0; face < 6; ++face) {
            auto* captureData = reinterpret_cast<::IBLCaptureData*>(r.iblCaptureDataBuffer->contents());
            captureData->faceIndex = face;
            captureData->roughness = 0.0f;
            r.iblCaptureDataBuffer->didModifyRange(NS::Range::Make(0, r.iblCaptureDataBuffer->length()));

            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = passDesc->colorAttachments()->object(0);
            colorAttachment->setLoadAction(MTL::LoadActionClear);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorAttachment->setTexture(r.irradianceMap.get());
            colorAttachment->setSlice(face);
            colorAttachment->setLevel(0);

            applyTimingToRenderDesc(passDesc.get(), face == 0, face == 5);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.irradianceConvolutionPipeline.get());
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setVertexBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
            encoder->setFragmentTexture(r.environmentCubemap.get(), 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }
    }
};

// Pre-filter environment map pass: Creates specular pre-filtered cubemap with roughness mips
class PrefilterEnvMapPass : public MetalRenderPass {
public:
    explicit PrefilterEnvMapPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "PrefilterEnvMapPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;

        const uint32_t maxMipLevels = 5;

        // For each mip level (roughness level)
        for (uint32_t mip = 0; mip < maxMipLevels; ++mip) {
            float roughness = (float)mip / (float)(maxMipLevels - 1);

            // For each face
            for (uint32_t face = 0; face < 6; ++face) {
                auto* captureData = reinterpret_cast<::IBLCaptureData*>(r.iblCaptureDataBuffer->contents());
                captureData->faceIndex = face;
                captureData->roughness = roughness;
                r.iblCaptureDataBuffer->didModifyRange(NS::Range::Make(0, r.iblCaptureDataBuffer->length()));

                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttachment = passDesc->colorAttachments()->object(0);
                colorAttachment->setLoadAction(MTL::LoadActionClear);
                colorAttachment->setStoreAction(MTL::StoreActionStore);
                colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
                colorAttachment->setTexture(r.prefilterMap.get());
                colorAttachment->setSlice(face);
                colorAttachment->setLevel(mip);

                bool pfIsFirst = (mip == 0 && face == 0);
                bool pfIsLast  = (mip == maxMipLevels - 1 && face == 5);
                applyTimingToRenderDesc(passDesc.get(), pfIsFirst, pfIsLast);
                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.prefilterEnvMapPipeline.get());
                encoder->setCullMode(MTL::CullModeNone);
                encoder->setVertexBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
                encoder->setFragmentBuffer(r.iblCaptureDataBuffer.get(), 0, 0);
                encoder->setFragmentTexture(r.environmentCubemap.get(), 0);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();
            }
        }
    }
};

// BRDF LUT pass: Pre-computes BRDF integration lookup table
class BRDFLUTPass : public MetalRenderPass {
public:
    explicit BRDFLUTPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "BRDFLUTPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.iblNeedsUpdate) return;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setLoadAction(MTL::LoadActionClear);
        colorAttachment->setStoreAction(MTL::StoreActionStore);
        colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorAttachment->setTexture(r.brdfLUT.get());

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.brdfLUTPipeline.get());
        encoder->setCullMode(MTL::CullModeNone);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();

        // IBL update complete
        r.iblNeedsUpdate = false;
    }
};

// Main render pass: Renders the scene with PBR lighting
class MainRenderPass : public MetalRenderPass {
public:
    explicit MainRenderPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "MainRenderPass";
    }

    void execute() override {
        auto& r = *renderer;

        // screenUV in the fragment shader is position / screenSize; position is in
        // framebuffer pixels, so screenSize must be the framebuffer's size — NOT the
        // live drawable size, which can drift after a window resize/DPI change and
        // then mismaps every screen-space texture lookup (shadow, AO, cluster tiles):
        // with a repeat sampler that shows up as tiled/compressed shadows.
        glm::vec2 screenSize = glm::vec2(r.colorRT->width(), r.colorRT->height());
        glm::uvec3 gridSize = glm::uvec3(r.clusterGridSizeX, r.clusterGridSizeY, r.clusterGridSizeZ);
        auto time = (float)SDL_GetTicks() / 1000.0f;

        // Create render pass descriptor
        auto renderPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto renderPassColorRT = renderPassDesc->colorAttachments()->object(0);
        renderPassColorRT->setClearColor(MTL::ClearColor(r.clearColor.r, r.clearColor.g, r.clearColor.b, r.clearColor.a)
        );
        renderPassColorRT->setLoadAction(MTL::LoadActionClear);
        renderPassColorRT->setStoreAction(MTL::StoreActionMultisampleResolve);
        renderPassColorRT->setTexture(r.colorRT_MS.get());
        renderPassColorRT->setResolveTexture(r.colorRT.get());

        auto renderPassDepthRT = renderPassDesc->depthAttachment();
        renderPassDepthRT->setLoadAction(MTL::LoadActionLoad);
        renderPassDepthRT->setTexture(r.depthStencilRT_MS.get());

        // Execute the pass
        applyTimingToRenderDesc(renderPassDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(renderPassDesc.get());
        encoder->setRenderPipelineState(r.drawPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.depthStencilState.get());

        r.currentInstanceCount = 0;
        r.culledInstanceCount = 0;

        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.materialDataBuffer.get(), 0, 1);
        encoder->setVertexBuffer(r.instanceDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setVertexBuffer(r.getBuffer(r.currentScene->vertexBuffer).get(), 0, 3);
        encoder->setFragmentBuffer(r.directionalLightBuffer.get(), 0, 0);
        encoder->setFragmentBuffer(r.pointLightBuffer.get(), 0, 1);
        encoder->setFragmentBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 3);
        encoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 4);
        encoder->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
        encoder->setFragmentBytes(&time, sizeof(float), 6);
        encoder->setFragmentBuffer(r.rectLightBuffer.get(), 0, 7);
        uint32_t rectLightCount = static_cast<uint32_t>(r.currentScene->rectLights.size());
        encoder->setFragmentBytes(&rectLightCount, sizeof(uint32_t), 8);
        encoder->setFragmentBuffer(r.pssmDataBuffers[r.currentFrameInFlight].get(), 0, 9);
        // Denoised AO attenuates the IBL/ambient term; white = AO off
        encoder->setFragmentTexture(r.aoEnabled ? r.aoRT.get() : r.batch2DWhiteTexture.get(), 6);
        auto* vidTex = r.rectLightVideoTexture
                           ? r.rectLightVideoTexture.get()
                           : r.getTexture(r.defaultAlbedoTexture).get();
        encoder->setFragmentTexture(vidTex, 11);

        for (const auto& [material, draws] : r.instanceBatches) {
            if (material->materialType == Vapor::MaterialType::Iridescent) {
                encoder->setRenderPipelineState(r.iridescentPipeline.get());
            } else {
                encoder->setRenderPipelineState(r.drawPipeline.get());
            }
            encoder->setFragmentTexture(
                r.getTexture(material->albedoMap ? material->albedoMap->texture : r.defaultAlbedoTexture).get(), 0
            );
            encoder->setFragmentTexture(
                r.getTexture(material->normalMap ? material->normalMap->texture : r.defaultNormalTexture).get(), 1
            );
            encoder->setFragmentTexture(
                r.getTexture(material->metallicMap ? material->metallicMap->texture : r.defaultORMTexture).get(), 2
            );
            encoder->setFragmentTexture(
                r.getTexture(material->roughnessMap ? material->roughnessMap->texture : r.defaultORMTexture).get(), 3
            );
            encoder->setFragmentTexture(
                r.getTexture(material->occlusionMap ? material->occlusionMap->texture : r.defaultORMTexture).get(), 4
            );
            encoder->setFragmentTexture(
                r.getTexture(material->emissiveMap ? material->emissiveMap->texture : r.defaultEmissiveTexture).get(), 5
            );
            encoder->setFragmentTexture(r.shadowRT.get(), 7);
            // White (=lit) when SSCS is off, so the shader's min() is a no-op.
            encoder->setFragmentTexture(r.sscsEnabled ? r.sscsRT.get() : r.batch2DWhiteTexture.get(), 15);

            // IBL textures
            encoder->setFragmentTexture(r.irradianceMap.get(), 8);
            encoder->setFragmentTexture(r.prefilterMap.get(), 9);
            encoder->setFragmentTexture(r.brdfLUT.get(), 10);

            // PSSM shadow maps (data buffer bound once before this loop, at buffer 9)
            encoder->setFragmentTexture(r.pssmShadowMaps.get(), 12);
            encoder->setFragmentTexture(r.pointShadowDenoisedRT.get(), 13);

            // GIBS (Global Illumination Based on Surfels)
            if (r.gibsEnabled && r.gibsManager && r.gibsManager->getGIResultTexture()) {
                encoder->setFragmentTexture(r.gibsManager->getGIResultTexture(), 14);
            }
            Uint32 gibsEnabledFlag = (r.gibsEnabled && r.gibsManager) ? 1 : 0;
            encoder->setFragmentBytes(&gibsEnabledFlag, sizeof(Uint32), 10);
            // Perf-isolation flags at buffer(12) — the shared PBR shader now
            // reads this slot, so it MUST be bound or the reference is UB.
            encoder->setFragmentBytes(&r.mainDebugFlags, sizeof(uint32_t), 12);
            // Spot lights are an RHI-path feature: bind a placeholder buffer
            // (count 0 -> the loop never dereferences it) and shadowFlags 0
            // (legacy R16F shadow target: rect/spot channels unavailable).
            encoder->setFragmentBuffer(r.pointLightBuffer.get(), 0, 14);
            glm::uvec2 spotRectParams(0u, 0u);
            encoder->setFragmentBytes(&spotRectParams, sizeof(spotRectParams), 15);

            for (const auto& draw : draws) {
                if (!r.currentCamera->isVisible(r.instances[draw.instanceIndex].boundingSphere)) {
                    r.culledInstanceCount++;
                    continue;
                }
                r.currentInstanceCount++;
                encoder->setVertexBytes(&draw.instanceIndex, sizeof(Uint32), 4);
                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    draw.mesh->indexCount,
                    MTL::IndexTypeUInt32,
                    r.getBuffer(r.currentScene->indexBuffer).get(),
                    draw.mesh->indexOffset * sizeof(Uint32)
                );
                r.drawCount++;
            }
        }

        encoder->endEncoding();
    }
};

// Water pass: Renders water surface with Gerstner waves, reflections, and refractions
class WaterPass : public MetalRenderPass {
public:
    explicit WaterPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "WaterPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.waterEnabled || r.waterIndexCount == 0) {
            return;
        }

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        auto time = (float)SDL_GetTicks() / 1000.0f;

        // Build model matrix from transform
        auto modelMatrix = glm::mat4(1.0f);
        modelMatrix = glm::translate(modelMatrix, r.waterTransform.position);
        modelMatrix = glm::scale(modelMatrix, r.waterTransform.scale);

        // Update water data buffer
        auto* waterData = reinterpret_cast<WaterData*>(r.waterDataBuffers[r.currentFrameInFlight]->contents());
        *waterData = r.waterSettings;
        waterData->modelMatrix = modelMatrix;
        waterData->time = time;
        r.waterDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.waterDataBuffers[r.currentFrameInFlight]->length())
        );

        // Create render pass descriptor - renders to resolved HDR target (no MSAA for water)
        auto waterPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto waterPassColorRT = waterPassDesc->colorAttachments()->object(0);
        waterPassColorRT->setLoadAction(MTL::LoadActionLoad);
        waterPassColorRT->setStoreAction(MTL::StoreActionStore);
        waterPassColorRT->setTexture(r.colorRT.get());

        auto waterPassDepthRT = waterPassDesc->depthAttachment();
        waterPassDepthRT->setLoadAction(MTL::LoadActionLoad);
        waterPassDepthRT->setStoreAction(MTL::StoreActionStore);
        waterPassDepthRT->setTexture(r.depthStencilRT.get());

        // Execute the pass
        applyTimingToRenderDesc(waterPassDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(waterPassDesc.get());
        encoder->setRenderPipelineState(r.waterPipeline.get());
        encoder->setCullMode(MTL::CullModeNone);// Water is double-sided
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setDepthStencilState(r.waterDepthStencilState.get());

        // Set vertex buffers
        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setVertexBuffer(r.waterDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setVertexBuffer(r.waterVertexBuffer.get(), 0, 2);

        // Set fragment textures
        encoder->setFragmentTexture(r.getTexture(r.waterNormalMap1).get(), 0);
        encoder->setFragmentTexture(r.getTexture(r.waterNormalMap2).get(), 1);
        encoder->setFragmentTexture(r.colorRT.get(), 2);// HDR scene for refraction
        encoder->setFragmentTexture(r.depthStencilRT.get(), 3);// Depth for depth softening
        encoder->setFragmentTexture(r.normalRT.get(), 4);// Scene normals for SSR
        encoder->setFragmentTexture(r.environmentCubeMap.get(), 5);// Environment cube map
        encoder->setFragmentTexture(r.getTexture(r.waterFoamMap).get(), 6);
        encoder->setFragmentTexture(r.getTexture(r.waterNoiseMap).get(), 7);

        // Set fragment buffers
        encoder->setFragmentBuffer(r.waterDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        encoder->setFragmentBuffer(r.directionalLightBuffer.get(), 0, 2);
        encoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 3);

        // Draw water mesh
        encoder->drawIndexedPrimitives(
            MTL::PrimitiveType::PrimitiveTypeTriangle,
            r.waterIndexCount,
            MTL::IndexTypeUInt32,
            r.waterIndexBuffer.get(),
            0
        );
        r.drawCount++;

        encoder->endEncoding();
    }
};

// Particle pass: GPU particle simulation and rendering
class ParticlePass : public MetalRenderPass {
public:
    explicit ParticlePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "ParticlePass";
    }

    void execute() override {
        auto& r = *renderer;

        if (r.particleCount == 0) {
            return;
        }
        if (!r.particleForcePipeline || !r.particleIntegratePipeline || !r.particleRenderPipeline) {
            return;
        }

        // ── Simulate ────────────────────────────────────────────────────────
        // Pause: skip the sim (deltaTime=0 would be a no-op) and leave state frozen.
        if (!r.m_particleSimPaused) {
            auto time = (float)SDL_GetTicks() / 1000.0f;

            // Update simulation params buffer using the canonical render_data.hpp struct.
            ParticleSimParams simParams;
            auto drawableSize = r.swapchain->drawableSize();
            simParams.resolution = glm::vec2(drawableSize.width, drawableSize.height);
            simParams.mousePosition = glm::vec2(0.0f);
            simParams.time = time;
            simParams.deltaTime = 1.0f / 60.0f;
            simParams.particleCount = r.particleCount;
            simParams.wind      = r.m_forceField.wind;
            simParams.turbulence = glm::vec4(0.0f, 0.0f, 0.0f, r.m_forceField.turbulence);

            const auto& attractors = r.m_forceField.attractors;
            simParams.attractorCount = static_cast<Uint32>(attractors.size());

            memcpy(r.particleSimParamsBuffers[r.currentFrameInFlight]->contents(), &simParams, sizeof(ParticleSimParams));
            r.particleSimParamsBuffers[r.currentFrameInFlight]->didModifyRange(NS::Range::Make(0, sizeof(ParticleSimParams)));

            if (!attractors.empty()) {
                const size_t attractorBytes = attractors.size() * sizeof(ParticleAttractor);
                memcpy(r.particleAttractorBuffers[r.currentFrameInFlight]->contents(), attractors.data(), attractorBytes);
                r.particleAttractorBuffers[r.currentFrameInFlight]->didModifyRange(NS::Range::Make(0, attractorBytes));
            }

            // Compute passes (single particle buffer - persistent state)
            auto timedComputeDesc = makeTimedComputeDesc(true, false);
            auto computeEncoder = r.currentCommandBuffer->computeCommandEncoder(timedComputeDesc.get());

            // Force calculation
            computeEncoder->setComputePipelineState(r.particleForcePipeline.get());
            computeEncoder->setBuffer(r.particleBuffer.get(), 0, 0);
            computeEncoder->setBuffer(r.particleSimParamsBuffers[r.currentFrameInFlight].get(), 0, 1);
            computeEncoder->setBuffer(r.particleAttractorBuffers[r.currentFrameInFlight].get(), 0, 2);

            MTL::Size gridSize = MTL::Size((r.particleCount + 255) / 256, 1, 1);
            MTL::Size threadGroupSize = MTL::Size(256, 1, 1);
            computeEncoder->dispatchThreadgroups(gridSize, threadGroupSize);

            // Integration
            computeEncoder->setComputePipelineState(r.particleIntegratePipeline.get());
            computeEncoder->setBuffer(r.particleBuffer.get(), 0, 0);
            computeEncoder->setBuffer(r.particleSimParamsBuffers[r.currentFrameInFlight].get(), 0, 1);
            computeEncoder->dispatchThreadgroups(gridSize, threadGroupSize);

            computeEncoder->endEncoding();
        }

        // ── Render ──────────────────────────────────────────────────────────
        // Hide: skip drawing but keep simulating, so unhiding is seamless.
        if (!r.particleVisible) return;

        // Render pass: Draw particles
        {
            auto renderPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttachment = renderPassDesc->colorAttachments()->object(0);
            colorAttachment->setLoadAction(MTL::LoadActionLoad);
            colorAttachment->setStoreAction(MTL::StoreActionStore);
            colorAttachment->setTexture(r.colorRT.get());

            auto depthAttachment = renderPassDesc->depthAttachment();
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
            depthAttachment->setStoreAction(MTL::StoreActionDontCare);
            depthAttachment->setTexture(r.depthStencilRT.get());

            applyTimingToRenderDesc(renderPassDesc.get(), false, true);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(renderPassDesc.get());
            encoder->setRenderPipelineState(r.particleRenderPipeline.get());
            encoder->setDepthStencilState(r.particleDepthStencilState.get());
            encoder->setCullMode(MTL::CullModeNone);

            // Set buffers
            encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 0);

            struct ParticlePushConstants {
                float particleSize;
                float _pad1;
                float _pad2;
                float _pad3;
            } pushConstants;
            pushConstants.particleSize = 0.1f;// Larger particles for visibility
            encoder->setVertexBytes(&pushConstants, sizeof(ParticlePushConstants), 1);
            encoder->setVertexBuffer(r.particleBuffer.get(), 0, 2);

            // Draw 6 vertices per particle (2 triangles = 1 quad), instanced
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 6, r.particleCount);
            encoder->endEncoding();
        }
    }
};

// Light scattering pass: Renders volumetric god rays effect
class LightScatteringPass : public MetalRenderPass {
public:
    explicit LightScatteringPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "LightScatteringPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.lightScatteringEnabled) return;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        // Calculate sun screen position by projecting sun direction
        auto* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());
        glm::vec3 sunDir = glm::normalize(atmos->sunDirection);

        // Project sun position to screen space
        // Sun is at infinity, so we use camera position + sun direction * large distance
        glm::vec3 camPos = r.currentCamera->getEye();
        glm::vec3 sunWorldPos = camPos + sunDir * 10000.0f;

        glm::mat4 viewProj = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        glm::vec4 sunClip = viewProj * glm::vec4(sunWorldPos, 1.0f);

        // Check if sun is behind camera
        if (sunClip.w <= 0.0f) {
            return;// Sun behind camera, no god rays
        }

        // Convert to NDC then to UV [0,1]
        glm::vec2 sunNDC = glm::vec2(sunClip.x, sunClip.y) / sunClip.w;
        glm::vec2 sunScreenPos = sunNDC * 0.5f + 0.5f;
        sunScreenPos.y = 1.0f - sunScreenPos.y;// Flip Y for Metal

        // Update light scattering data buffer
        auto* lsData =
            reinterpret_cast<LightScatteringData*>(r.lightScatteringDataBuffers[r.currentFrameInFlight]->contents());
        lsData->sunScreenPos = sunScreenPos;
        lsData->screenSize = screenSize;
        lsData->density = r.lightScatteringSettings.density;
        lsData->weight = r.lightScatteringSettings.weight;
        lsData->decay = r.lightScatteringSettings.decay;
        lsData->exposure = r.lightScatteringSettings.exposure;
        lsData->numSamples = r.lightScatteringSettings.numSamples;
        lsData->maxDistance = r.lightScatteringSettings.maxDistance;
        lsData->sunIntensity = r.lightScatteringSettings.sunIntensity;
        lsData->mieG = r.lightScatteringSettings.mieG;
        lsData->sunColor = atmos->sunColor;
        lsData->depthThreshold = r.lightScatteringSettings.depthThreshold;
        lsData->jitter = r.lightScatteringSettings.jitter;
        r.lightScatteringDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.lightScatteringDataBuffers[r.currentFrameInFlight]->length())
        );

        // Create render pass descriptor - render to light scattering RT
        auto lsPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto lsPassColorRT = lsPassDesc->colorAttachments()->object(0);
        lsPassColorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        lsPassColorRT->setLoadAction(MTL::LoadActionClear);
        lsPassColorRT->setStoreAction(MTL::StoreActionStore);
        lsPassColorRT->setTexture(r.lightScatteringRT.get());

        // Execute the pass
        applyTimingToRenderDesc(lsPassDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(lsPassDesc.get());
        encoder->setRenderPipelineState(r.lightScatteringPipeline.get());
        encoder->setCullMode(MTL::CullModeNone);

        // Set textures
        encoder->setFragmentTexture(r.colorRT.get(), 0);// Scene color
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);// Scene depth

        // Set buffers
        encoder->setFragmentBuffer(r.lightScatteringDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.frameDataBuffers[r.currentFrameInFlight].get(), 0, 1);

        // Draw full-screen triangle
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// ============================================================================
// Volumetric Fog Pass: Height-based fog with scattering
// ============================================================================

class VolumetricFogPass : public MetalRenderPass {
public:
    explicit VolumetricFogPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "VolumetricFogPass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.volumetricFogEnabled || !r.fogSimplePipeline) return;

        auto drawableSize = r.swapchain->drawableSize();

        // Update fog data buffer
        auto* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());

        auto* fogData =
            reinterpret_cast<VolumetricFogData*>(r.volumetricFogDataBuffers[r.currentFrameInFlight]->contents());
        fogData->invViewProj = glm::inverse(r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix());
        fogData->cameraPosition = r.currentCamera->getEye();
        fogData->sunDirection = glm::normalize(atmos->sunDirection);
        fogData->sunColor = atmos->sunColor;
        fogData->sunIntensity = atmos->sunIntensity;
        fogData->screenSize = glm::vec2(drawableSize.width, drawableSize.height);
        fogData->nearPlane = r.currentCamera->near();
        fogData->farPlane = r.volumetricFogSettings.farPlane;
        fogData->frameIndex = r.currentFrameInFlight;
        fogData->time = r.volumetricFogSettings.time;

        // Copy settings
        fogData->fogDensity = r.volumetricFogSettings.fogDensity;
        fogData->fogHeightFalloff = r.volumetricFogSettings.fogHeightFalloff;
        fogData->fogBaseHeight = r.volumetricFogSettings.fogBaseHeight;
        fogData->fogMaxHeight = r.volumetricFogSettings.fogMaxHeight;
        fogData->scatteringCoeff = r.volumetricFogSettings.scatteringCoeff;
        fogData->extinctionCoeff = r.volumetricFogSettings.extinctionCoeff;
        fogData->anisotropy = r.volumetricFogSettings.anisotropy;
        fogData->ambientIntensity = r.volumetricFogSettings.ambientIntensity;
        fogData->noiseScale = r.volumetricFogSettings.noiseScale;
        fogData->noiseIntensity = r.volumetricFogSettings.noiseIntensity;
        fogData->windSpeed = r.volumetricFogSettings.windSpeed;
        fogData->windDirection = r.volumetricFogSettings.windDirection;
        fogData->temporalBlend = r.volumetricFogSettings.temporalBlend;

        r.volumetricFogDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.volumetricFogDataBuffers[r.currentFrameInFlight]->length())
        );

        // Simple fog pass - ping-pong: read from colorRT, write to tempColorRT
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttach = passDesc->colorAttachments()->object(0);
        colorAttach->setLoadAction(MTL::LoadActionDontCare);
        colorAttach->setStoreAction(MTL::StoreActionStore);
        colorAttach->setTexture(r.tempColorRT.get());// Write to temp

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.fogSimplePipeline.get());
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setFragmentTexture(r.colorRT.get(), 0);// Read from color
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);
        encoder->setFragmentBuffer(r.volumetricFogDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
        // Volumetric raymarch inputs (shared shader contract): PSSM cascades
        // for sun shafts + the light set. Native has no spot lights — bind a
        // placeholder with count 0.
        encoder->setFragmentTexture(r.pssmShadowMaps.get(), 2);
        encoder->setFragmentBuffer(r.pssmDataBuffers[r.currentFrameInFlight].get(), 0, 2);
        encoder->setFragmentBuffer(r.pointLightBuffer.get(), 0, 3);
        encoder->setFragmentBuffer(r.clusterBuffers[r.currentFrameInFlight].get(), 0, 4);
        encoder->setFragmentBuffer(r.pointLightBuffer.get(), 0, 5);  // spot placeholder (count 0)
        encoder->setFragmentBuffer(r.rectLightBuffer.get(), 0, 6);
        uint32_t fogRectCount = r.currentScene
            ? static_cast<uint32_t>(r.currentScene->rectLights.size()) : 0u;
        glm::uvec4 fogLightParams(r.clusterGridSizeX, r.clusterGridSizeY, 0u, fogRectCount);
        encoder->setFragmentBytes(&fogLightParams, sizeof(fogLightParams), 7);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();

        // Swap so colorRT now contains the fogged result
        std::swap(r.colorRT, r.tempColorRT);
    }
};

// ============================================================================
// Volumetric Cloud Pass: Ray-marched clouds
// ============================================================================

class VolumetricCloudPass : public MetalRenderPass {
public:
    explicit VolumetricCloudPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "VolumetricCloudPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Check if any required pipeline is available
        bool hasLowResPipeline = r.cloudLowResPipeline && r.cloudCompositePipeline;
        bool hasFullResPipeline = r.cloudRenderPipeline.get() != nullptr;

        if (!r.volumetricCloudsEnabled || (!hasLowResPipeline && !hasFullResPipeline)) return;

        auto drawableSize = r.swapchain->drawableSize();

        // Update cloud data buffer
        auto* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());

        auto* cloudData =
            reinterpret_cast<VolumetricCloudData*>(r.volumetricCloudDataBuffers[r.currentFrameInFlight]->contents());
        cloudData->invViewProj = glm::inverse(r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix());
        cloudData->prevViewProj = r.volumetricCloudSettings.prevViewProj;// For temporal reprojection
        cloudData->cameraPosition = r.currentCamera->getEye();
        cloudData->sunDirection = glm::normalize(atmos->sunDirection);
        cloudData->sunColor = atmos->sunColor;
        cloudData->sunIntensity = atmos->sunIntensity;
        cloudData->frameIndex = r.currentFrameInFlight;
        cloudData->time = r.volumetricCloudSettings.time;

        // Update wind offset (accumulate over time)
        r.volumetricCloudSettings.windOffset +=
            r.volumetricCloudSettings.windDirection * r.volumetricCloudSettings.windSpeed * 0.016f;
        cloudData->windOffset = r.volumetricCloudSettings.windOffset;

        // Copy settings
        cloudData->cloudLayerBottom = r.volumetricCloudSettings.cloudLayerBottom;
        cloudData->cloudLayerTop = r.volumetricCloudSettings.cloudLayerTop;
        cloudData->cloudLayerThickness = cloudData->cloudLayerTop - cloudData->cloudLayerBottom;
        cloudData->cloudCoverage = r.volumetricCloudSettings.cloudCoverage;
        cloudData->cloudDensity = r.volumetricCloudSettings.cloudDensity;
        cloudData->cloudType = r.volumetricCloudSettings.cloudType;
        cloudData->erosionStrength = r.volumetricCloudSettings.erosionStrength;
        cloudData->shapeNoiseScale = r.volumetricCloudSettings.shapeNoiseScale;
        cloudData->detailNoiseScale = r.volumetricCloudSettings.detailNoiseScale;
        cloudData->ambientIntensity = r.volumetricCloudSettings.ambientIntensity;
        cloudData->silverLiningIntensity = r.volumetricCloudSettings.silverLiningIntensity;
        cloudData->silverLiningSpread = r.volumetricCloudSettings.silverLiningSpread;
        cloudData->phaseG1 = r.volumetricCloudSettings.phaseG1;
        cloudData->phaseG2 = r.volumetricCloudSettings.phaseG2;
        cloudData->phaseBlend = r.volumetricCloudSettings.phaseBlend;
        cloudData->powderStrength = r.volumetricCloudSettings.powderStrength;
        cloudData->windDirection = r.volumetricCloudSettings.windDirection;
        cloudData->windSpeed = r.volumetricCloudSettings.windSpeed;
        cloudData->primarySteps = r.volumetricCloudSettings.primarySteps;
        cloudData->lightSteps = r.volumetricCloudSettings.lightSteps;
        cloudData->temporalBlend = r.volumetricCloudSettings.temporalBlend;

        r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
        );

        // Use quarter-resolution pipeline if available, otherwise fall back to full-res
        if (hasLowResPipeline && r.cloudRT && r.cloudHistoryRT) {
            Uint32 cloudWidth = drawableSize.width / 4;
            Uint32 cloudHeight = drawableSize.height / 4;

            // Update screen size for quarter resolution
            cloudData->screenSize = glm::vec2(cloudWidth, cloudHeight);
            r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
                NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
            );

            // ================================================================
            // Pass 1: Render clouds at quarter resolution
            // ================================================================
            {
                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttach = passDesc->colorAttachments()->object(0);
                colorAttach->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
                colorAttach->setLoadAction(MTL::LoadActionClear);
                colorAttach->setStoreAction(MTL::StoreActionStore);
                colorAttach->setTexture(r.cloudRT.get());

                applyTimingToRenderDesc(passDesc.get(), true, false);
                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.cloudLowResPipeline.get());
                encoder->setCullMode(MTL::CullModeNone);

                // Set viewport to quarter resolution
                MTL::Viewport viewport;
                viewport.originX = 0.0;
                viewport.originY = 0.0;
                viewport.width = cloudWidth;
                viewport.height = cloudHeight;
                viewport.znear = 0.0;
                viewport.zfar = 1.0;
                encoder->setViewport(viewport);

                encoder->setFragmentTexture(r.depthStencilRT.get(), 0);
                encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
                encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();
            }

            // ================================================================
            // Pass 2: Temporal resolve (blend current with history)
            // ================================================================
            if (r.cloudTemporalResolvePipeline) {
                // Swap cloudRT and cloudHistoryRT for temporal accumulation
                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttach = passDesc->colorAttachments()->object(0);
                colorAttach->setLoadAction(MTL::LoadActionDontCare);
                colorAttach->setStoreAction(MTL::StoreActionStore);
                colorAttach->setTexture(r.cloudHistoryRT.get());

                // no timing — this is a middle encoder
                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.cloudTemporalResolvePipeline.get());
                encoder->setCullMode(MTL::CullModeNone);

                MTL::Viewport viewport;
                viewport.originX = 0.0;
                viewport.originY = 0.0;
                viewport.width = cloudWidth;
                viewport.height = cloudHeight;
                viewport.znear = 0.0;
                viewport.zfar = 1.0;
                encoder->setViewport(viewport);

                encoder->setFragmentTexture(r.cloudRT.get(), 0);// Current frame
                encoder->setFragmentTexture(r.cloudHistoryRT.get(), 1);// History (will be overwritten)
                encoder->setFragmentTexture(r.depthStencilRT.get(), 2);
                encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();

                // Swap RT pointers for next frame
                std::swap(r.cloudRT, r.cloudHistoryRT);
            }

            // ================================================================
            // Pass 3: Upscale and composite - ping-pong to avoid hazard
            // ================================================================
            {
                // Restore screen size for composite pass
                cloudData->screenSize = glm::vec2(drawableSize.width, drawableSize.height);
                r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
                    NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
                );

                auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
                auto colorAttach = passDesc->colorAttachments()->object(0);
                colorAttach->setLoadAction(MTL::LoadActionDontCare);
                colorAttach->setStoreAction(MTL::StoreActionStore);
                colorAttach->setTexture(r.tempColorRT.get());// Write to temp

                applyTimingToRenderDesc(passDesc.get(), false, true);
                auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
                encoder->setRenderPipelineState(r.cloudCompositePipeline.get());
                encoder->setCullMode(MTL::CullModeNone);
                encoder->setFragmentTexture(r.colorRT.get(), 0);// Read from color
                encoder->setFragmentTexture(r.cloudRT.get(), 1);// Cloud (quarter res)
                encoder->setFragmentTexture(r.depthStencilRT.get(), 2);
                encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
                encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
                encoder->endEncoding();

                // Swap so colorRT now contains the composited result
                std::swap(r.colorRT, r.tempColorRT);
            }
        } else if (hasFullResPipeline) {
            // Fallback: Full resolution rendering - ping-pong to avoid hazard
            cloudData->screenSize = glm::vec2(drawableSize.width, drawableSize.height);
            r.volumetricCloudDataBuffers[r.currentFrameInFlight]->didModifyRange(
                NS::Range::Make(0, r.volumetricCloudDataBuffers[r.currentFrameInFlight]->length())
            );

            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorAttach = passDesc->colorAttachments()->object(0);
            colorAttach->setLoadAction(MTL::LoadActionDontCare);
            colorAttach->setStoreAction(MTL::StoreActionStore);
            colorAttach->setTexture(r.tempColorRT.get());// Write to temp

            applyTimingToRenderDesc(passDesc.get(), true, true);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.cloudRenderPipeline.get());
            encoder->setCullMode(MTL::CullModeNone);
            encoder->setFragmentTexture(r.colorRT.get(), 0);// Read from color
            encoder->setFragmentTexture(r.depthStencilRT.get(), 1);
            encoder->setFragmentBuffer(r.volumetricCloudDataBuffers[r.currentFrameInFlight].get(), 0, 0);
            encoder->setFragmentBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();

            // Swap so colorRT now contains the composited result
            std::swap(r.colorRT, r.tempColorRT);
        }

        // Store current view-proj for next frame's temporal reprojection
        r.volumetricCloudSettings.prevViewProj = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
    }
};

// ============================================================================
// Sun Flare Pass: Lens flare effect with procedural textures
// ============================================================================

class SunFlarePass : public MetalRenderPass {
public:
    explicit SunFlarePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "SunFlarePass";
    }

    void execute() override {
        auto& r = *renderer;

        if (!r.sunFlareEnabled || !r.sunFlarePipeline) return;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize = glm::vec2(drawableSize.width, drawableSize.height);

        // Calculate sun screen position
        auto* atmos = reinterpret_cast<AtmosphereData*>(r.atmosphereDataBuffer->contents());
        glm::vec3 sunDir = glm::normalize(atmos->sunDirection);
        glm::vec3 camPos = r.currentCamera->getEye();
        glm::vec3 sunWorldPos = camPos + sunDir * 10000.0f;

        glm::mat4 viewProj = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        glm::vec4 sunClip = viewProj * glm::vec4(sunWorldPos, 1.0f);

        // Sun behind camera
        if (sunClip.w <= 0.0f) {
            return;
        }

        // Convert to screen UV
        glm::vec2 sunNDC = glm::vec2(sunClip.x, sunClip.y) / sunClip.w;
        glm::vec2 sunScreenPos = sunNDC * 0.5f + 0.5f;
        sunScreenPos.y = 1.0f - sunScreenPos.y;

        // Update flare data buffer
        auto* flareData = reinterpret_cast<SunFlareData*>(r.sunFlareDataBuffers[r.currentFrameInFlight]->contents());
        flareData->sunScreenPos = sunScreenPos;
        flareData->screenSize = screenSize;
        flareData->screenCenter = glm::vec2(0.5f, 0.5f);
        flareData->aspectRatio = glm::vec2(screenSize.x / screenSize.y, 1.0f);
        flareData->sunColor = atmos->sunColor;

        // Simple visibility check using depth at sun position
        // For proper occlusion, we'd use the compute shader, but this is a simple approximation
        float visibility = 1.0f;
        if (sunScreenPos.x < 0.0f || sunScreenPos.x > 1.0f || sunScreenPos.y < 0.0f || sunScreenPos.y > 1.0f) {
            visibility = 0.0f;
        }
        flareData->visibility = visibility;

        // Copy settings
        flareData->sunIntensity = r.sunFlareSettings.sunIntensity;
        flareData->fadeEdge = r.sunFlareSettings.fadeEdge;
        flareData->glowIntensity = r.sunFlareSettings.glowIntensity;
        flareData->glowFalloff = r.sunFlareSettings.glowFalloff;
        flareData->glowSize = r.sunFlareSettings.glowSize;
        flareData->haloIntensity = r.sunFlareSettings.haloIntensity;
        flareData->haloRadius = r.sunFlareSettings.haloRadius;
        flareData->haloWidth = r.sunFlareSettings.haloWidth;
        flareData->haloFalloff = r.sunFlareSettings.haloFalloff;
        flareData->ghostCount = r.sunFlareSettings.ghostCount;
        flareData->ghostSpacing = r.sunFlareSettings.ghostSpacing;
        flareData->ghostIntensity = r.sunFlareSettings.ghostIntensity;
        flareData->ghostSize = r.sunFlareSettings.ghostSize;
        flareData->ghostChromaticOffset = r.sunFlareSettings.ghostChromaticOffset;
        flareData->ghostFalloff = r.sunFlareSettings.ghostFalloff;
        flareData->streakIntensity = r.sunFlareSettings.streakIntensity;
        flareData->streakLength = r.sunFlareSettings.streakLength;
        flareData->streakFalloff = r.sunFlareSettings.streakFalloff;
        flareData->starburstIntensity = r.sunFlareSettings.starburstIntensity;
        flareData->starburstSize = r.sunFlareSettings.starburstSize;
        flareData->starburstPoints = r.sunFlareSettings.starburstPoints;
        flareData->starburstRotation = r.sunFlareSettings.starburstRotation;
        flareData->dirtIntensity = r.sunFlareSettings.dirtIntensity;
        flareData->dirtScale = r.sunFlareSettings.dirtScale;
        flareData->time = r.sunFlareSettings.time;

        r.sunFlareDataBuffers[r.currentFrameInFlight]->didModifyRange(
            NS::Range::Make(0, r.sunFlareDataBuffers[r.currentFrameInFlight]->length())
        );

        // Render flare with additive blending (hardware blends output onto existing content)
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttach = passDesc->colorAttachments()->object(0);
        colorAttach->setLoadAction(MTL::LoadActionLoad);// Preserve existing bloom result
        colorAttach->setStoreAction(MTL::StoreActionStore);
        colorAttach->setTexture(r.bloomResultRT.get());

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.sunFlarePipeline.get());
        encoder->setCullMode(MTL::CullModeNone);
        // No need to bind bloomResultRT as input - hardware blending handles compositing
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);
        encoder->setFragmentBuffer(r.sunFlareDataBuffers[r.currentFrameInFlight].get(), 0, 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// ============================================================================
// Bloom passes: Physically-based bloom implementation
// ============================================================================

// Bloom brightness pass: Extracts bright pixels from the scene
class BloomBrightnessPass : public MetalRenderPass {
public:
    explicit BloomBrightnessPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "BloomBrightnessPass";
    }

    void execute() override {
        auto& r = *renderer;
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.bloomBrightnessRT.get());

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.bloomBrightnessPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.colorRT.get(), 0);
        encoder->setFragmentBytes(&r.bloomThreshold, sizeof(float), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Bloom downsample pass: Creates the bloom mipmap pyramid
class BloomDownsamplePass : public MetalRenderPass {
public:
    explicit BloomDownsamplePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "BloomDownsamplePass";
    }

    void execute() override {
        auto& r = *renderer;

        // First downsample from brightness RT to pyramid level 0
        {
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorRT = passDesc->colorAttachments()->object(0);
            colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorRT->setLoadAction(MTL::LoadActionClear);
            colorRT->setStoreAction(MTL::StoreActionStore);
            colorRT->setTexture(r.bloomPyramidRTs[0].get());

            applyTimingToRenderDesc(passDesc.get(), true, r.BLOOM_PYRAMID_LEVELS == 1);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.bloomDownsamplePipeline.get());
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
            encoder->setFragmentTexture(r.bloomBrightnessRT.get(), 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }

        // Downsample through the rest of the pyramid
        for (Uint32 i = 1; i < r.BLOOM_PYRAMID_LEVELS; i++) {
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorRT = passDesc->colorAttachments()->object(0);
            colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
            colorRT->setLoadAction(MTL::LoadActionClear);
            colorRT->setStoreAction(MTL::StoreActionStore);
            colorRT->setTexture(r.bloomPyramidRTs[i].get());

            applyTimingToRenderDesc(passDesc.get(), false, i == r.BLOOM_PYRAMID_LEVELS - 1);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.bloomDownsamplePipeline.get());
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
            encoder->setFragmentTexture(r.bloomPyramidRTs[i - 1].get(), 0);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }
    }
};

// Bloom upsample pass: Upsamples and accumulates the bloom
class BloomUpsamplePass : public MetalRenderPass {
public:
    explicit BloomUpsamplePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "BloomUpsamplePass";
    }

    void execute() override {
        auto& r = *renderer;

        // Upsample from bottom of pyramid to top, accumulating bloom
        for (int i = static_cast<int>(r.BLOOM_PYRAMID_LEVELS) - 2; i >= 0; i--) {
            auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
            auto colorRT = passDesc->colorAttachments()->object(0);
            colorRT->setLoadAction(MTL::LoadActionLoad);// Load to blend with existing content
            colorRT->setStoreAction(MTL::StoreActionStore);
            colorRT->setTexture(r.bloomPyramidRTs[i].get());

            bool upIsFirst = (i == static_cast<int>(r.BLOOM_PYRAMID_LEVELS) - 2);
            applyTimingToRenderDesc(passDesc.get(), upIsFirst, i == 0);
            auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
            encoder->setRenderPipelineState(r.bloomUpsamplePipeline.get());
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
            encoder->setFragmentTexture(r.bloomPyramidRTs[i + 1].get(), 0);// Lower res texture
            encoder->setFragmentTexture(r.bloomPyramidRTs[i].get(), 1);// Current level to blend
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
            encoder->endEncoding();
        }
    }
};

// Bloom composite pass: Combines bloom with the scene
class BloomCompositePass : public MetalRenderPass {
public:
    explicit BloomCompositePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "BloomCompositePass";
    }

    void execute() override {
        auto& r = *renderer;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.bloomResultRT.get());

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.bloomCompositePipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.colorRT.get(), 0);// Original scene
        encoder->setFragmentTexture(r.bloomPyramidRTs[0].get(), 1);// Accumulated bloom
        encoder->setFragmentBytes(&r.bloomStrength, sizeof(float), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// ============================================================================
// DOF (Tilt-Shift) passes: Octopath Traveler style depth of field
// ============================================================================

// DOF CoC pass: Calculate Circle of Confusion based on screen position
class DOFCoCPass : public MetalRenderPass {
public:
    explicit DOFCoCPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "DOFCoCPass";
    }

    void execute() override {
        auto& r = *renderer;

        // GPU-compatible DOF params struct (matches shader)
        struct GPUDOFParams {
            float focusCenter;
            float focusWidth;
            float focusFalloff;
            float maxBlur;
            float tiltAngle;
            float bokehRoundness;
            float padding1;
            float padding2;
        } gpuParams = { r.dofParams.focusCenter,
                        r.dofParams.focusWidth,
                        r.dofParams.focusFalloff,
                        r.dofParams.maxBlur,
                        r.dofParams.tiltAngle,
                        r.dofParams.bokehRoundness,
                        0.0f,
                        0.0f };

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.dofCoCRT.get());

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.dofCoCPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.bloomResultRT.get(), 0);// Input from bloom
        encoder->setFragmentTexture(r.depthStencilRT.get(), 1);// Depth (optional for hybrid mode)
        encoder->setFragmentBytes(&gpuParams, sizeof(GPUDOFParams), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// DOF Blur pass: Apply bokeh blur based on CoC
class DOFBlurPass : public MetalRenderPass {
public:
    explicit DOFBlurPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "DOFBlurPass";
    }

    void execute() override {
        auto& r = *renderer;

        struct DOFBlurParams {
            float texelSizeX;
            float texelSizeY;
            float blurScale;
            int sampleCount;
        } blurParams = { 1.0f / r.dofBlurRT->width(), 1.0f / r.dofBlurRT->height(), 1.0f, r.dofParams.sampleCount };

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 0.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.dofBlurRT.get());

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.dofBlurPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.dofCoCRT.get(), 0);
        encoder->setFragmentBytes(&blurParams, sizeof(DOFBlurParams), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// DOF Composite pass: Blend sharp and blurred images
class DOFCompositePass : public MetalRenderPass {
public:
    explicit DOFCompositePass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "DOFCompositePass";
    }

    void execute() override {
        auto& r = *renderer;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorRT = passDesc->colorAttachments()->object(0);
        colorRT->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
        colorRT->setLoadAction(MTL::LoadActionClear);
        colorRT->setStoreAction(MTL::StoreActionStore);
        colorRT->setTexture(r.dofResultRT.get());

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(r.dofCompositePipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);
        encoder->setFragmentTexture(r.bloomResultRT.get(), 0);// Sharp (from bloom)
        encoder->setFragmentTexture(r.dofBlurRT.get(), 1);// Blurred
        encoder->setFragmentBytes(&r.dofParams.blendSharpness, sizeof(float), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Post-process pass: Applies tone mapping, color grading, chromatic aberration, vignette
class PostProcessPass : public MetalRenderPass {
public:
    explicit PostProcessPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "PostProcessPass";
    }

    void execute() override {
        auto& r = *renderer;

        // GPU-compatible post-process params struct (must match shader)
        struct GPUPostProcessParams {
            float chromaticAberrationStrength;
            float chromaticAberrationFalloff;
            float vignetteStrength;
            float vignetteRadius;
            float vignetteSoftness;
            float saturation;
            float contrast;
            float brightness;
            float temperature;
            float tint;
            float exposure;
        } gpuParams = { r.postProcessParams.chromaticAberrationStrength,
                        r.postProcessParams.chromaticAberrationFalloff,
                        r.postProcessParams.vignetteStrength,
                        r.postProcessParams.vignetteRadius,
                        r.postProcessParams.vignetteSoftness,
                        r.postProcessParams.saturation,
                        r.postProcessParams.contrast,
                        r.postProcessParams.brightness,
                        r.postProcessParams.temperature,
                        r.postProcessParams.tint,
                        r.postProcessParams.exposure };

        // Create render pass descriptor
        auto postPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto postPassColorRT = postPassDesc->colorAttachments()->object(0);
        postPassColorRT->setClearColor(MTL::ClearColor(r.clearColor.r, r.clearColor.g, r.clearColor.b, r.clearColor.a));
        postPassColorRT->setLoadAction(MTL::LoadActionClear);
        postPassColorRT->setStoreAction(MTL::StoreActionStore);
        postPassColorRT->setTexture(r.currentDrawable->texture());

        // Execute the pass
        applyTimingToRenderDesc(postPassDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(postPassDesc.get());
        encoder->setRenderPipelineState(r.postProcessPipeline.get());
        encoder->setCullMode(MTL::CullModeBack);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);

        // Input texture: DOF result if DOF enabled, otherwise bloom result
        // Note: When DOF passes are commented out, dofResultRT won't have valid content
        // So we use bloomResultRT by default. Uncomment DOF passes and change this to dofResultRT.
        encoder->setFragmentTexture(r.bloomResultRT.get(), 0);
        encoder->setFragmentTexture(r.aoRT.get(), 1);
        encoder->setFragmentTexture(r.normalRT.get(), 2);
        encoder->setFragmentTexture(r.lightScatteringRT.get(), 3);// God rays texture
        encoder->setFragmentBytes(&gpuParams, sizeof(GPUPostProcessParams), 0);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 0, 3, 1);
        encoder->endEncoding();
    }
};

// Debug draw pass: Renders wireframe debug shapes (lines)
class DebugDrawPass : public MetalRenderPass {
public:
    explicit DebugDrawPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "DebugDrawPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Skip if no debug draw data
        if (!r.debugDraw || !r.debugDraw->hasContent()) {
            return;
        }

        const auto& lineVertices = r.debugDraw->getLineVertices();
        if (lineVertices.empty()) {
            return;
        }

        // Get or create vertex buffer for this frame
        auto& vertexBuffer = r.debugDrawVertexBuffers[r.currentFrameInFlight];

        // Calculate required buffer size
        size_t requiredSize = lineVertices.size() * sizeof(Vapor::DebugVertex);

        // Reallocate buffer if needed
        if (!vertexBuffer || vertexBuffer->length() < requiredSize) {
            // Allocate with some extra space to avoid frequent reallocations
            size_t allocSize = std::max(requiredSize, size_t(64 * 1024));// Min 64KB
            vertexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }

        // Upload vertex data
        memcpy(vertexBuffer->contents(), lineVertices.data(), requiredSize);
        vertexBuffer->didModifyRange(NS::Range(0, requiredSize));

        // Create render pass descriptor
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(r.currentDrawable->texture());
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        // Use depth buffer for proper occlusion
        auto depthAttachment = passDesc->depthAttachment();
        depthAttachment->setTexture(r.depthStencilRT.get());
        depthAttachment->setLoadAction(MTL::LoadActionLoad);
        depthAttachment->setStoreAction(MTL::StoreActionStore);

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());

        // Set viewport
        auto drawableSize = r.currentDrawable->texture()->width();
        auto drawableHeight = r.currentDrawable->texture()->height();
        MTL::Viewport viewport = { 0.0, 0.0, static_cast<double>(drawableSize), static_cast<double>(drawableHeight),
                                   0.0, 1.0 };
        encoder->setViewport(viewport);

        // Set pipeline and depth state
        encoder->setRenderPipelineState(r.debugDrawPipeline.get());
        encoder->setDepthStencilState(r.debugDrawDepthStencilState.get());
        encoder->setCullMode(MTL::CullModeNone);

        // Set vertex buffer
        encoder->setVertexBuffer(vertexBuffer.get(), 0, 0);
        encoder->setVertexBuffer(r.cameraDataBuffers[r.currentFrameInFlight].get(), 0, 1);

        // Draw lines
        encoder->drawPrimitives(MTL::PrimitiveTypeLine, NS::UInteger(0), NS::UInteger(lineVertices.size()));

        encoder->endEncoding();

        r.debugDraw->clear();
    }
};

// RmlUI pass: Renders the RmlUI overlay (before ImGui)
class RmlUiPass : public MetalRenderPass {
public:
    explicit RmlUiPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "RmlUiPass";
    }

    void execute() override {
        auto& r = *renderer;
        // Simply call the renderer's UI rendering method
        r.renderUI();
    }
};

// ImGui pass: Renders the ImGui UI overlay
class ImGuiPass : public MetalRenderPass {
public:
    explicit ImGuiPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "ImGuiPass";
    }

    void execute() override {
        auto& r = *renderer;

        // UI building is done in draw() before this pass
        // This pass just renders the ImGui draw data
        ImGui::Render();

        // Create render pass descriptor
        auto imguiPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto imguiPassColorRT = imguiPassDesc->colorAttachments()->object(0);
        imguiPassColorRT->setLoadAction(MTL::LoadActionLoad);
        imguiPassColorRT->setStoreAction(MTL::StoreActionStore);
        imguiPassColorRT->setTexture(r.currentDrawable->texture());

        applyTimingToRenderDesc(imguiPassDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(imguiPassDesc.get());
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), r.currentCommandBuffer, encoder);
        encoder->endEncoding();
    }
};

// 2D Batch pass: Renders batched 2D primitives (quads, lines, shapes)
class WorldCanvasPass : public MetalRenderPass {
public:
    explicit WorldCanvasPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "WorldCanvasPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Skip if no batch data
        if (r.batch3DVertices.empty() || r.batch3DIndices.empty()) {
            return;
        }

        // Use 3D buffers
        auto& vertexBuffer = r.batch3DVertexBuffers[r.currentFrameInFlight];
        auto& indexBuffer = r.batch3DIndexBuffers[r.currentFrameInFlight];
        auto& uniformBuffer = r.batch3DUniformBuffers[r.currentFrameInFlight];

        auto vertexCount = static_cast<Uint32>(r.batch3DVertices.size());
        auto indexCount = static_cast<Uint32>(r.batch3DIndices.size());

        size_t vertexDataSize = vertexCount * sizeof(Batch2DVertex);
        size_t indexDataSize = indexCount * sizeof(Uint32);

        if (!vertexBuffer || vertexBuffer->length() < vertexDataSize) {
            size_t allocSize = std::max(vertexDataSize, size_t(256 * 1024));
            vertexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }
        if (!indexBuffer || indexBuffer->length() < indexDataSize) {
            size_t allocSize = std::max(indexDataSize, size_t(128 * 1024));
            indexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }

        memcpy(vertexBuffer->contents(), r.batch3DVertices.data(), vertexDataSize);
        memcpy(indexBuffer->contents(), r.batch3DIndices.data(), indexDataSize);

        // Use camera's viewProj for 3D batch
        Batch2DUniforms uniforms;
        uniforms.projectionMatrix = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        memcpy(uniformBuffer->contents(), &uniforms, sizeof(Batch2DUniforms));

        MTL::RenderPipelineState* pipeline = r.batch2DPipeline.get();
        if (!pipeline) return;

        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(r.colorRT.get());// Render to HDR RT (before bloom)
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        // Always use depth buffer for 3D
        if (r.depthStencilRT) {
            auto depthAttachment = passDesc->depthAttachment();
            depthAttachment->setTexture(r.depthStencilRT.get());
            depthAttachment->setLoadAction(MTL::LoadActionLoad);
            depthAttachment->setStoreAction(MTL::StoreActionStore);
        }

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());

        auto drawableWidth = r.colorRT->width();
        auto drawableHeight = r.colorRT->height();
        MTL::Viewport viewport = { 0.0, 0.0, static_cast<double>(drawableWidth), static_cast<double>(drawableHeight),
                                   0.0, 1.0 };
        encoder->setViewport(viewport);

        encoder->setRenderPipelineState(pipeline);
        encoder->setDepthStencilState(r.batch2DDepthStencilStateEnabled.get());
        encoder->setCullMode(MTL::CullModeNone);

        encoder->setVertexBuffer(vertexBuffer.get(), 0, 0);
        encoder->setVertexBuffer(uniformBuffer.get(), 0, 1);

        for (Uint32 i = 0; i < r.batch3DTextureSlotIndex; i++) {
            TextureHandle handle = r.batch3DTextureSlots[i];
            MTL::Texture* texture = r.batch2DWhiteTexture.get();
            if (handle.id != UINT32_MAX) {
                auto texPtr = r.getTexture(handle);
                if (texPtr) texture = texPtr.get();
            }
            encoder->setFragmentTexture(texture, i);
        }

        encoder->drawIndexedPrimitives(
            MTL::PrimitiveTypeTriangle,
            NS::UInteger(indexCount),
            MTL::IndexTypeUInt32,
            indexBuffer.get(),
            NS::UInteger(0)
        );
        encoder->endEncoding();

        // Clear batch
        r.batch3DVertices.clear();
        r.batch3DIndices.clear();
        r.batch3DTextureSlotIndex = 1;
        r.batch3DActive = false;
    }
};

class CanvasPass : public MetalRenderPass {
public:
    explicit CanvasPass(Renderer_Metal* renderer) : MetalRenderPass(renderer) {
    }

    auto getName() const -> const char* override {
        return "CanvasPass";
    }

    void execute() override {
        auto& r = *renderer;

        // Gather all sub-batches (from texture-slot splits) plus the current in-progress batch
        struct BatchRef {
            const std::vector<Batch2DVertex>*      vertices;
            const std::vector<Uint32>*             indices;
            const std::array<TextureHandle, 16>*   slots;
            Uint32                                 slotCount;
        };
        std::vector<BatchRef> batches;
        batches.reserve(r.batch2DSubBatches.size() + 1);
        for (const auto& sub : r.batch2DSubBatches) {
            if (!sub.vertices.empty())
                batches.push_back({&sub.vertices, &sub.indices, &sub.textureSlots, sub.textureSlotCount});
        }
        if (!r.batch2DVertices.empty())
            batches.push_back({&r.batch2DVertices, &r.batch2DIndices, &r.batch2DTextureSlots, r.batch2DTextureSlotIndex});

        if (batches.empty()) return;

        // Compute combined buffer sizes
        size_t totalVertices = 0, totalIndices = 0;
        for (const auto& b : batches) {
            totalVertices += b.vertices->size();
            totalIndices  += b.indices->size();
        }

        // Get / resize frame-buffered GPU buffers
        auto& vertexBuffer  = r.batch2DVertexBuffers[r.currentFrameInFlight];
        auto& indexBuffer   = r.batch2DIndexBuffers[r.currentFrameInFlight];
        auto& uniformBuffer = r.batch2DUniformBuffers[r.currentFrameInFlight];

        const size_t vertexDataSize = totalVertices * sizeof(Batch2DVertex);
        const size_t indexDataSize  = totalIndices  * sizeof(Uint32);

        if (!vertexBuffer || vertexBuffer->length() < vertexDataSize) {
            size_t allocSize = std::max(vertexDataSize, size_t(256 * 1024));
            vertexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }
        if (!indexBuffer || indexBuffer->length() < indexDataSize) {
            size_t allocSize = std::max(indexDataSize, size_t(128 * 1024));
            indexBuffer = NS::TransferPtr(r.device->newBuffer(allocSize, MTL::ResourceStorageModeShared));
        }

        // Upload all vertices contiguously
        {
            auto* dst = static_cast<Batch2DVertex*>(vertexBuffer->contents());
            for (const auto& b : batches) {
                memcpy(dst, b.vertices->data(), b.vertices->size() * sizeof(Batch2DVertex));
                dst += b.vertices->size();
            }
        }

        // Upload all indices with per-sub-batch vertex base offset applied
        {
            auto* dst = static_cast<Uint32*>(indexBuffer->contents());
            Uint32 vertexBase = 0;
            for (const auto& b : batches) {
                for (Uint32 idx : *b.indices)
                    *dst++ = idx + vertexBase;
                vertexBase += static_cast<Uint32>(b.vertices->size());
            }
        }

        // Projection matrix
        int windowWidth, windowHeight;
        SDL_GetWindowSize(r.window, &windowWidth, &windowHeight);
        Batch2DUniforms uniforms;
        if (r.currentCamera && r.currentCamera->isOrthographic()) {
            uniforms.projectionMatrix = r.currentCamera->getProjMatrix() * r.currentCamera->getViewMatrix();
        } else {
            uniforms.projectionMatrix =
                glm::ortho(0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0.0f, -1.0f, 1.0f);
        }
        memcpy(uniformBuffer->contents(), &uniforms, sizeof(Batch2DUniforms));

        MTL::RenderPipelineState* pipeline = r.batch2DPipeline.get();
        if (!pipeline) return;

        // Create render pass (render to HDR RT, before bloom)
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto* colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(r.colorRT.get());
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        applyTimingToRenderDesc(passDesc.get(), true, true);
        auto encoder = r.currentCommandBuffer->renderCommandEncoder(passDesc.get());
        auto rtWidth  = r.colorRT->width();
        auto rtHeight = r.colorRT->height();
        MTL::Viewport viewport = { 0.0, 0.0, static_cast<double>(rtWidth), static_cast<double>(rtHeight), 0.0, 1.0 };
        encoder->setViewport(viewport);
        encoder->setRenderPipelineState(pipeline);
        encoder->setDepthStencilState(r.batch2DDepthStencilState.get());
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBuffer(vertexBuffer.get(), 0, 0);
        encoder->setVertexBuffer(uniformBuffer.get(), 0, 1);

        // One draw call per sub-batch (texture slots differ between sub-batches)
        NS::UInteger indexByteOffset = 0;
        for (const auto& b : batches) {
            // Bind this sub-batch's texture slots
            for (Uint32 i = 0; i < b.slotCount; i++) {
                TextureHandle handle = (*b.slots)[i];
                MTL::Texture* tex = nullptr;
                if (handle.id != UINT32_MAX) {
                    auto texPtr = r.getTexture(handle);
                    if (texPtr) tex = texPtr.get();
                }
                encoder->setFragmentTexture(tex ? tex : r.batch2DWhiteTexture.get(), i);
            }

            auto indexCount = static_cast<NS::UInteger>(b.indices->size());
            encoder->drawIndexedPrimitives(
                MTL::PrimitiveTypeTriangle,
                indexCount,
                MTL::IndexTypeUInt32,
                indexBuffer.get(),
                indexByteOffset
            );
            indexByteOffset += indexCount * sizeof(Uint32);

            r.batch2DStats.drawCalls++;
            r.batch2DStats.vertexCount += static_cast<Uint32>(b.vertices->size());
            r.batch2DStats.indexCount  += static_cast<Uint32>(b.indices->size());
        }

        encoder->endEncoding();

        // Clear all batch state for next frame
        r.batch2DSubBatches.clear();
        r.batch2DVertices.clear();
        r.batch2DIndices.clear();
        r.batch2DTextureSlotIndex = 1;
        r.batch2DActive = false;
    }
};

// ============================================================================
// GIBS (Global Illumination Based on Surfels) Passes
// ============================================================================

class SurfelGenerationPass : public MetalRenderPass {
public:
    SurfelGenerationPass(Renderer_Metal* renderer, Vapor::GIBSManager* gm)
        : MetalRenderPass(renderer), gibsManager(gm) {}

    const char* getName() const override { return "SurfelGenerationPass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.gibsEnabled || !gibsManager || !r.surfelGenerationPipeline) return;

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize(drawableSize.width, drawableSize.height);

        SurfelGenerationParams params;
        params.invViewProj = gibsManager->getGIBSData().invViewProj;
        params.screenSize = screenSize;
        params.surfelRadius = gibsManager->getGIBSData().surfelRadius;
        params.densityThreshold = 0.01f;
        // Small per-frame budget: coverage dedup only sees LAST frame's surfels,
        // so same-frame duplicates are invisible to each other. A large budget
        // floods the pool with duplicates before the hash catches up; a small
        // one converges in ~1-2s with dedup effective from frame 2 onward.
        params.maxNewSurfels = std::max(gibsManager->getMaxSurfels() / 100, 1000u);
        params.frameIndex = r.frameNumber;

        auto timedDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setLabel(NS::String::string("Surfel Generation", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.surfelGenerationPipeline.get());

        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(r.albedoRT.get(), 2);

        encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getCounterBuffer(), 0, 1);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);
        encoder->setBytes(&params, sizeof(params), 3);
        // Fresh spatial hash (hash build runs before this pass) for coverage rejection
        encoder->setBuffer(gibsManager->getCellHeadBuffer(), 0, 4);
        encoder->setBuffer(gibsManager->getSurfelNextBuffer(), 0, 5);

        uint32_t dispatchX = (static_cast<uint32_t>(screenSize.x) + 7) / 8;
        uint32_t dispatchY = (static_cast<uint32_t>(screenSize.y) + 7) / 8;
        encoder->dispatchThreadgroups(MTL::Size(dispatchX, dispatchY, 1), MTL::Size(8, 8, 1));
        encoder->endEncoding();
    }

private:
    Vapor::GIBSManager* gibsManager;
};

class SurfelHashBuildPass : public MetalRenderPass {
public:
    SurfelHashBuildPass(Renderer_Metal* renderer, Vapor::GIBSManager* gm)
        : MetalRenderPass(renderer), gibsManager(gm) {}

    const char* getName() const override { return "SurfelHashBuildPass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.gibsEnabled || !gibsManager || !r.surfelClearCellHeadsPipeline) return;
        uint32_t totalCells = gibsManager->getTotalCells();
        uint32_t activeSurfels = gibsManager->getActiveSurfelCount();
        if (activeSurfels == 0) activeSurfels = 1;

        MTL::Buffer* gibsData = gibsManager->getGIBSDataBuffer(r.currentFrameInFlight);

        // Linked-list spatial hash: two fully parallel dispatches (clear heads,
        // then atomic-push each surfel onto its cell's list). Replaces the
        // counting sort whose single-threaded prefix sum walked every cell.
        auto timedDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setLabel(NS::String::string("Surfel Hash Build", NS::UTF8StringEncoding));

        // Step 1: Reset cell list heads
        encoder->setComputePipelineState(r.surfelClearCellHeadsPipeline.get());
        encoder->setBuffer(gibsManager->getCellHeadBuffer(), 0, 0);
        encoder->setBuffer(gibsData, 0, 1);
        encoder->dispatchThreadgroups(MTL::Size((totalCells + 255) / 256, 1, 1), MTL::Size(256, 1, 1));

        // Step 2: Insert surfels into their cell lists
        encoder->setComputePipelineState(r.surfelInsertPipeline.get());
        encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getCellHeadBuffer(), 0, 1);
        encoder->setBuffer(gibsManager->getSurfelNextBuffer(), 0, 2);
        encoder->setBuffer(gibsData, 0, 3);
        encoder->dispatchThreadgroups(MTL::Size((activeSurfels + 255) / 256, 1, 1), MTL::Size(256, 1, 1));

        encoder->endEncoding();

        addTrafficEstimate(uint64_t(totalCells) * 4 + uint64_t(activeSurfels) * (sizeof(Surfel) + 8));
    }

private:
    Vapor::GIBSManager* gibsManager;
};

class SurfelRaytracingPass : public MetalRenderPass {
public:
    SurfelRaytracingPass(Renderer_Metal* renderer, Vapor::GIBSManager* gm)
        : MetalRenderPass(renderer), gibsManager(gm) {}

    const char* getName() const override { return "SurfelRaytracingPass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.gibsEnabled || !gibsManager || !r.surfelRaytracingSimplePipeline) return;
        const auto& gibsData = gibsManager->getGIBSData();

        // Staggered updates: refresh 1/4 of the surfel pool per frame; temporal
        // blending integrates the rest. Cuts per-frame ray cost by 4x.
        constexpr uint32_t UPDATE_INTERVAL = 4;

        SurfelRaytracingParams params;
        params.surfelOffset = 0;
        params.surfelCount = gibsManager->getActiveSurfelCount();
        params.raysPerSurfel = gibsManager->getRaysPerSurfel();
        params.frameIndex = r.frameNumber;
        params.rayBias = gibsData.rayBias;
        params.rayMaxDistance = gibsData.rayMaxDistance;
        params.updateInterval = UPDATE_INTERVAL;

        if (params.surfelCount == 0) return;

        auto timedDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setLabel(NS::String::string("Surfel Raytracing", NS::UTF8StringEncoding));

        bool useRT = r.m_supportsRaytracing && r.TLASBuffers[r.currentFrameInFlight] && r.surfelRaytracingPipeline;
        encoder->setComputePipelineState(useRT ? r.surfelRaytracingPipeline.get()
                                                : r.surfelRaytracingSimplePipeline.get());

        // Canonical buffer at 0: irradiance accumulates here across frames.
        // Neighbor lookups walk the cell linked lists over the same buffer.
        encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getCellHeadBuffer(), 0, 1);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);
        encoder->setBytes(&params, sizeof(params), 3);

        if (useRT) {
            encoder->setAccelerationStructure(r.TLASBuffers[r.currentFrameInFlight].get(), 4);
            encoder->setBuffer(gibsManager->getSurfelNextBuffer(), 0, 5);
        }

        // Only dispatch threads for this frame's residue class of surfels
        uint32_t surfelsThisFrame = (params.surfelCount + UPDATE_INTERVAL - 1) / UPDATE_INTERVAL;
        uint32_t threadGroups = (surfelsThisFrame + 63) / 64;
        encoder->dispatchThreadgroups(MTL::Size(threadGroups, 1, 1), MTL::Size(64, 1, 1));
        encoder->endEncoding();

        addTrafficEstimate(uint64_t(surfelsThisFrame) * sizeof(Surfel) * 2);
    }

private:
    Vapor::GIBSManager* gibsManager;
};

class GIBSTemporalPass : public MetalRenderPass {
public:
    GIBSTemporalPass(Renderer_Metal* renderer, Vapor::GIBSManager* gm)
        : MetalRenderPass(renderer), gibsManager(gm) {}

    const char* getName() const override { return "GIBSTemporalPass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.gibsEnabled || !gibsManager || !r.gibsTemporalPipeline) return;
        uint32_t activeSurfels = gibsManager->getActiveSurfelCount();
        if (activeSurfels == 0) return;

        auto timedDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setLabel(NS::String::string("GIBS Temporal", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.gibsTemporalPipeline.get());

        // Canonical buffer: smoothing must persist, the sorted copy is rebuilt each frame
        encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 1);

        uint32_t threadGroups = (activeSurfels + 255) / 256;
        encoder->dispatchThreadgroups(MTL::Size(threadGroups, 1, 1), MTL::Size(256, 1, 1));
        encoder->endEncoding();
    }

private:
    Vapor::GIBSManager* gibsManager;
};

class GIBSSamplePass : public MetalRenderPass {
public:
    GIBSSamplePass(Renderer_Metal* renderer, Vapor::GIBSManager* gm)
        : MetalRenderPass(renderer), gibsManager(gm) {}

    const char* getName() const override { return "GIBSSamplePass"; }

    void execute() override {
        auto& r = *renderer;
        if (!r.gibsEnabled || !gibsManager || !r.gibsSamplePipeline) return;
        if (!gibsManager->getGIResultTexture()) return; // textures created lazily in draw()
        const auto& gibsData = gibsManager->getGIBSData();

        auto drawableSize = r.swapchain->drawableSize();
        glm::vec2 screenSize(drawableSize.width, drawableSize.height);
        glm::vec2 giResolution = screenSize * gibsManager->getResolutionScale();

        GIBSSampleParams params;
        params.invViewProj = gibsData.invViewProj;
        params.screenSize = screenSize;
        params.giResolution = giResolution;
        // Surfel influence radius in WORLD units; the cell search radius
        // (gibs.sampleRadius, in cells) is a separate parameter in the kernel
        params.sampleRadius = gibsData.cellSize;
        params.maxSamples = gibsData.maxSurfelsPerPixel;
        params.normalWeight = 1.0f;
        params.distanceWeight = 1.0f;

        // Sample at GI resolution; the main pass upsamples for free via bilinear sampling
        auto timedDesc = makeTimedComputeDesc(true, true);
        auto encoder = r.currentCommandBuffer->computeCommandEncoder(timedDesc.get());
        encoder->setLabel(NS::String::string("GIBS Sample", NS::UTF8StringEncoding));
        encoder->setComputePipelineState(r.gibsSamplePipeline.get());

        encoder->setTexture(r.depthStencilRT.get(), 0);
        encoder->setTexture(r.normalRT.get(), 1);
        encoder->setTexture(gibsManager->getGIResultTexture(), 2);

        encoder->setBuffer(gibsManager->getSurfelBuffer(), 0, 0);
        encoder->setBuffer(gibsManager->getCellHeadBuffer(), 0, 1);
        encoder->setBuffer(gibsManager->getGIBSDataBuffer(r.currentFrameInFlight), 0, 2);
        encoder->setBytes(&params, sizeof(params), 3);
        encoder->setBuffer(gibsManager->getSurfelNextBuffer(), 0, 4);

        uint32_t dispatchX = (static_cast<uint32_t>(giResolution.x) + 7) / 8;
        uint32_t dispatchY = (static_cast<uint32_t>(giResolution.y) + 7) / 8;
        encoder->dispatchThreadgroups(MTL::Size(dispatchX, dispatchY, 1), MTL::Size(8, 8, 1));
        encoder->endEncoding();

        addTrafficEstimate(uint64_t(giResolution.x) * uint64_t(giResolution.y) * (4 + 8 + 8));
    }

private:
    Vapor::GIBSManager* gibsManager;
};

std::unique_ptr<IRenderer> createRendererMetal(SDL_Window* window) {
    auto r = std::make_unique<Renderer_Metal>();
    r->init(window);  // creates device/swapchain and initializes the ImGui Metal backend
    return r;
}

Renderer_Metal::Renderer_Metal() {
}

Renderer_Metal::~Renderer_Metal() {
    deinit();
}

auto Renderer_Metal::init(SDL_Window* window) -> void {
    ZoneScoped;

    this->window = window;
    renderer = SDL_CreateRenderer(window, nullptr);
    swapchain = (CA::MetalLayer*)SDL_GetRenderMetalLayer(renderer);
    // swapchain->setDisplaySyncEnabled(true);
    swapchain->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
    swapchain->setColorspace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    device = swapchain->device();
    queue = NS::TransferPtr(device->newCommandQueue());
    m_supportsRaytracing = device->supportsRaytracing();
    if (std::getenv("GITHUB_ACTIONS")) m_supportsRaytracing = false;

    // GPU pass timing: find timestamp counter set and create sample buffer.
    // Each pass embeds samples via descriptor-level sampleBufferAttachments (AtStageBoundary).
    if (device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary)) {
        auto counterSets = device->counterSets();
        for (NS::UInteger i = 0; counterSets && i < counterSets->count(); ++i) {
            auto cs = static_cast<MTL::CounterSet*>(counterSets->object(i));
            if (cs->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) {
                auto desc = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
                desc->setCounterSet(cs);
                desc->setSampleCount(GPU_TIMER_SAMPLE_COUNT);
                desc->setStorageMode(MTL::StorageModeShared);
                NS::Error* err = nullptr;
                gpuTimerSampleBuffer = NS::TransferPtr(device->newCounterSampleBuffer(desc.get(), &err));
                if (gpuTimerSampleBuffer && !err) {
                    gpuTimingSupported = true;
                }
                break;
            }
        }
    }

    // ImGui init
    ImGui_ImplSDL3_InitForMetal(window);
    ImGui_ImplMetal_Init(device);

    isInitialized = true;

    createResources();

    // Initialize render graph with all passes
    // IBL passes (run conditionally when iblNeedsUpdate is true)
    graph.addPass(std::make_unique<EquirectToCubemapPass>(this));
    graph.addPass(std::make_unique<SkyCapturePass>(this));
    graph.addPass(std::make_unique<IrradianceConvolutionPass>(this));
    graph.addPass(std::make_unique<PrefilterEnvMapPass>(this));
    graph.addPass(std::make_unique<BRDFLUTPass>(this));

    // Scene rendering passes
    if (m_supportsRaytracing) graph.addPass(std::make_unique<TLASBuildPass>(this));
    graph.addPass(std::make_unique<PrePass>(this));
    graph.addPass(std::make_unique<NormalResolvePass>(this));
    graph.addPass(std::make_unique<VelocityPass>(this));
    graph.addPass(std::make_unique<TileCullingPass>(this));
    graph.addPass(std::make_unique<PSSMShadowPass>(this));
    graph.addPass(std::make_unique<PSSMResolvePass>(this));
    if (m_supportsRaytracing) graph.addPass(std::make_unique<RaytraceShadowPass>(this));
    graph.addPass(std::make_unique<SSCSPass>(this));  // contact shadows (no RT needed)
    if (m_supportsRaytracing) graph.addPass(std::make_unique<RaytraceAOPass>(this));
    if (m_supportsRaytracing) graph.addPass(std::make_unique<AOTemporalPass>(this));
    if (m_supportsRaytracing) graph.addPass(std::make_unique<AODenoisePass>(this));
    if (m_supportsRaytracing) graph.addPass(std::make_unique<StochasticPointShadowPass>(this));
    if (m_supportsRaytracing) graph.addPass(std::make_unique<PointShadowTemporalPass>(this));

    // GIBS (Global Illumination Based on Surfels) passes
    // Always add passes; they check gibsEnabled in execute() for runtime toggle.
    // Hash build runs FIRST so generation can coverage-check against the fresh
    // spatial hash (built from last frame's pool) and raytracing/sampling get
    // up-to-date cells.
    if (gibsManager) {
        graph.addPass(std::make_unique<SurfelHashBuildPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<SurfelGenerationPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<SurfelRaytracingPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<GIBSTemporalPass>(this, gibsManager.get()));
        graph.addPass(std::make_unique<GIBSSamplePass>(this, gibsManager.get()));
    }

    graph.addPass(std::make_unique<MainRenderPass>(this));
    graph.addPass(std::make_unique<SkyAtmospherePass>(this));
    // graph.addPass(std::make_unique<WaterPass>(this));
    graph.addPass(std::make_unique<ParticlePass>(this));

    // Volumetric effects (fog and clouds)
    graph.addPass(std::make_unique<VolumetricFogPass>(this));
    graph.addPass(std::make_unique<VolumetricCloudPass>(this));

    // Light scattering (god rays)
    graph.addPass(std::make_unique<LightScatteringPass>(this));
    graph.addPass(std::make_unique<WorldCanvasPass>(this));// 3D world-space quads (with depth)
    graph.addPass(std::make_unique<CanvasPass>(this));// 2D screen-space quads (no depth, for pure 2D games)

    // Bloom passes (physically-based bloom)
    graph.addPass(std::make_unique<BloomBrightnessPass>(this));
    graph.addPass(std::make_unique<BloomDownsamplePass>(this));
    graph.addPass(std::make_unique<BloomUpsamplePass>(this));
    graph.addPass(std::make_unique<BloomCompositePass>(this));

    // Sun flare / lens flare effect (after bloom)
    graph.addPass(std::make_unique<SunFlarePass>(this));

    // DOF passes (Octopath Traveler style tilt-shift)
    // Uncomment these to enable DOF, and change PostProcessPass input to dofResultRT
    // graph.addPass(std::make_unique<DOFCoCPass>(this));
    // graph.addPass(std::make_unique<DOFBlurPass>(this));
    // graph.addPass(std::make_unique<DOFCompositePass>(this));

    // Post-processing (tone mapping, color grading, chromatic aberration, vignette)
    graph.addPass(std::make_unique<PostProcessPass>(this));
    graph.addPass(std::make_unique<DebugDrawPass>(this));// Debug draw after post-process
    graph.addPass(std::make_unique<RmlUiPass>(this));// RmlUI (pure UI, no bloom)
    // NOTE: ImGuiPass is NOT part of the graph. ImGui draw-data submission runs
    // in endFrame(), after the caller's ImGui::Render() (frame model parity with
    // the RHI renderer). See Renderer_Metal::endFrame().

    debugDraw = std::make_shared<Vapor::DebugDraw>();

    // Initialize 2D batch state
    batch2DVertices.reserve(BatchMaxVertices);
    batch2DIndices.reserve(BatchMaxIndices);
    batch2DTextureSlots[0] = batch2DWhiteTextureHandle;
    batch2DTextureSlotIndex = 1;

    // Pre-compute quad vertex positions (centered at origin, size 1x1)
    batchQuadPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
    batchQuadPositions[1] = { 0.5f, -0.5f, 0.0f, 1.0f };
    batchQuadPositions[2] = { 0.5f, 0.5f, 0.0f, 1.0f };
    batchQuadPositions[3] = { -0.5f, 0.5f, 0.0f, 1.0f };

    // Default UVs
    batchQuadTexCoords[0] = { 0.0f, 0.0f };
    batchQuadTexCoords[1] = { 1.0f, 0.0f };
    batchQuadTexCoords[2] = { 1.0f, 1.0f };
    batchQuadTexCoords[3] = { 0.0f, 1.0f };
}

auto Renderer_Metal::deinit() -> void {
    if (!isInitialized) {
        return;
    }

    // UI cleanup
    if (m_uiRenderer) {
        auto* uiRenderer = static_cast<Vapor::RmlRendererMetal*>(m_uiRenderer);
        uiRenderer->shutdown();
        delete uiRenderer;
        m_uiRenderer = nullptr;
    }

    // ImGui deinit
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplSDL3_Shutdown();

    SDL_DestroyRenderer(renderer);

    isInitialized = false;
}

auto Renderer_Metal::initUI() -> bool {
    // Get the engine core and RmlUI manager
    auto* engineCore = Vapor::EngineCore::Get();
    if (!engineCore) {
        fmt::print("Renderer_Metal::initUI: EngineCore not available\n");
        return false;
    }

    auto* rmluiManager = engineCore->getRmlUiManager();
    if (!rmluiManager || !rmluiManager->IsInitialized()) {
        fmt::print("Renderer_Metal::initUI: RmlUiManager not initialized\n");
        return false;
    }

    // Create Metal UI renderer (shared implementation)
    auto* uiRenderer = new Vapor::RmlRendererMetal(device);
    if (!uiRenderer->initialize()) {
        fmt::print("Renderer_Metal::initUI: Failed to initialize Metal UI renderer\n");
        delete uiRenderer;
        return false;
    }

    m_uiRenderer = uiRenderer;

    // Set as RmlUI's render interface
    Rml::SetRenderInterface(uiRenderer);

    // Now finalize RmlUI initialization (creates context, loads fonts, etc.)
    if (!rmluiManager->FinalizeInitialization()) {
        fmt::print("Renderer_Metal::initUI: Failed to finalize RmlUI\n");
        delete uiRenderer;
        m_uiRenderer = nullptr;
        return false;
    }

    // Store the context
    m_uiContext = rmluiManager->GetContext();

    fmt::print("Renderer_Metal::initUI: UI renderer initialized successfully\n");
    return true;
}

void Renderer_Metal::renderUI() {
    if (!m_uiRenderer || !m_uiContext) {
        return;
    }

    auto* uiRenderer = static_cast<Vapor::RmlRendererMetal*>(m_uiRenderer);

    auto surface = currentDrawable;
    if (!surface) return;

    // Use window size for RmlUI coordinate system (not framebuffer size)
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    MTL::Texture* renderTarget = surface->texture();
    int fbWidth = static_cast<int>(renderTarget->width());
    int fbHeight = static_cast<int>(renderTarget->height());

    // Create render pass (load existing content)
    auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    auto colorAttachment = passDesc->colorAttachments()->object(0);
    colorAttachment->setTexture(renderTarget);
    colorAttachment->setLoadAction(MTL::LoadActionLoad);
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    auto* encoder = currentCommandBuffer->renderCommandEncoder(passDesc.get());
    uiRenderer->setEncoder(encoder, windowWidth, windowHeight, fbWidth, fbHeight);
    m_uiContext->Render();
    uiRenderer->clearEncoder();
    encoder->endEncoding();
}

auto Renderer_Metal::createResources() -> void {
    // Create pipelines
    drawPipeline = createPipeline("shaders/3d_pbr_normal_mapped.metal", true, false, MSAA_SAMPLE_COUNT);
    iridescentPipeline = createPipeline("shaders/3d_pbr_iridescent.metal", true, false, MSAA_SAMPLE_COUNT);
    equirectToCubemapPipeline = createPipeline("shaders/3d_equirect_to_cubemap.metal", false, true, 1);

    // PrePass pipeline with MRT (normal + albedo for GIBS)
    {
        auto shaderSrc = readFile("shaders/3d_depth_only.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format("PrePass shader compile error: {}\n", error->localizedDescription()->utf8String()));
        }

        auto vertexMain = library->newFunction(NS::String::string("vertexMain", NS::UTF8StringEncoding));
        auto fragmentMain = library->newFunction(NS::String::string("fragmentMain", NS::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);

        // Color attachment 0: Normal (HDR)
        auto colorAttachment0 = pipelineDesc->colorAttachments()->object(0);
        colorAttachment0->setPixelFormat(MTL::PixelFormatRGBA16Float);

        // Color attachment 1: Albedo (LDR)
        auto colorAttachment1 = pipelineDesc->colorAttachments()->object(1);
        colorAttachment1->setPixelFormat(MTL::PixelFormatRGBA8Unorm);

        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        pipelineDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));

        prePassPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!prePassPipeline) {
            throw std::runtime_error(fmt::format("PrePass pipeline error: {}\n", error->localizedDescription()->utf8String()));
        }

        pipelineDesc->release();
        vertexMain->release();
        fragmentMain->release();
        library->release();
        code->release();
    }

    postProcessPipeline = createPipeline("shaders/3d_post_process.metal", false, true, 1);
    buildClustersPipeline = createComputePipeline("shaders/3d_cluster_build.metal");
    cullLightsPipeline = createComputePipeline("shaders/3d_light_cull.metal");
    tileCullingPipeline = createComputePipeline("shaders/3d_tile_light_cull.metal");
    normalResolvePipeline = createComputePipeline("shaders/3d_normal_resolve.metal");
    velocityPipeline = createComputePipeline("shaders/3d_velocity.metal");
    if (m_supportsRaytracing) raytraceShadowPipeline = createComputePipeline("shaders/3d_raytrace_shadow.metal");
    sscsPipeline = createComputePipeline("shaders/3d_sscs.metal");
    // AO raygen: 3d_ssao.metal (screen-space) and 3d_raytrace_ao.metal (ray-traced)
    // are drop-in interchangeable here; both feed the temporal + à-trous chain.
    // RT AO: 2 cosine-weighted any-hit rays/px, 1.5m cap (the visibility knob —
    // open areas correctly read as unoccluded; corners/contact darken).
    if (m_supportsRaytracing) raytraceAOPipeline = createComputePipeline("shaders/3d_raytrace_ao.metal");
    if (m_supportsRaytracing) ssaoPipeline = createComputePipeline("shaders/3d_ssao.metal");
    if (m_supportsRaytracing) aoTemporalPipeline = createComputePipeline("shaders/3d_ao_temporal.metal");
    if (m_supportsRaytracing) aoDenoisePipeline = createComputePipeline("shaders/3d_ao_denoise.metal");
    if (m_supportsRaytracing) stochasticPointShadowPipeline = createComputePipeline("shaders/3d_stochastic_shadow.metal");
    pointShadowTemporalPipeline = createComputePipeline("shaders/3d_stochastic_shadow_temporal.metal");
    pssmResolvePipeline = createComputePipeline("shaders/3d_pssm_resolve.metal");

    // PSSM depth-only pipeline
    {
        auto shaderSrc = readFile("shaders/3d_pssm_shadow_depth.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print("Warning: Could not compile PSSM shadow shader: {}\n",
                       error ? error->localizedDescription()->utf8String() : "unknown");
        } else {
            auto vFn = library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
            auto fFn = library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

            auto desc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
            desc->setVertexFunction(vFn);
            desc->setFragmentFunction(fFn);
            desc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
            // No colour attachments — depth only

            pssmShadowPipeline = NS::TransferPtr(device->newRenderPipelineState(desc.get(), &error));
            if (!pssmShadowPipeline) {
                fmt::print("Warning: Could not create PSSM shadow pipeline: {}\n",
                           error ? error->localizedDescription()->utf8String() : "unknown");
            }
            vFn->release();
            fFn->release();
            library->release();
        }

        // Depth stencil state for PSSM shadow pass
        MTL::DepthStencilDescriptor* dsDesc = MTL::DepthStencilDescriptor::alloc()->init();
        dsDesc->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        dsDesc->setDepthWriteEnabled(true);
        pssmDepthStencilState = NS::TransferPtr(device->newDepthStencilState(dsDesc));
        dsDesc->release();
    }
    atmospherePipeline =
        createPipeline("shaders/3d_atmosphere.metal", true, false, 1);// No MSAA for sky (full-screen triangle)
    skyCapturePipeline = createPipeline("shaders/3d_sky_capture.metal", true, true, 1);
    irradianceConvolutionPipeline = createPipeline("shaders/3d_irradiance_convolution.metal", true, true, 1);
    prefilterEnvMapPipeline = createPipeline("shaders/3d_prefilter_envmap.metal", true, true, 1);
    brdfLUTPipeline = createPipeline("shaders/3d_brdf_lut.metal", false, true, 1);
    lightScatteringPipeline = createPipeline("shaders/3d_light_scattering.metal", true, true, 1);

    // GIBS (Global Illumination Based on Surfels) pipelines
    if (m_supportsRaytracing) {
        surfelGenerationPipeline = createComputePipeline("shaders/gibs_surfel_generation.metal", "surfelGeneration");
        surfelClearCellHeadsPipeline = createComputePipeline("shaders/gibs_spatial_hash.metal", "clearCellHeads");
        surfelInsertPipeline = createComputePipeline("shaders/gibs_spatial_hash.metal", "insertSurfels");
        surfelRaytracingPipeline = createComputePipeline("shaders/gibs_raytracing.metal", "surfelRaytracing");
        surfelRaytracingSimplePipeline = createComputePipeline("shaders/gibs_raytracing.metal", "surfelRaytracingSimple");
        gibsTemporalPipeline = createComputePipeline("shaders/gibs_temporal.metal", "surfelTemporalSmooth");
        gibsSamplePipeline = createComputePipeline("shaders/gibs_sample.metal", "giSample");
        gibsUpsamplePipeline = createComputePipeline("shaders/gibs_sample.metal", "giBilateralUpsample");
        gibsCompositePipeline = createComputePipeline("shaders/gibs_sample.metal", "giComposite");

        // Initialize GIBS Manager
        gibsManager = std::make_unique<Vapor::GIBSManager>(this);
        gibsManager->setQuality(gibsQuality);
        gibsManager->init();
    }

    // Create debug draw pipeline
    {
        auto shaderSrc = readFile("shaders/3d_debug.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile debug draw shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexFuncName = NS::String::string("debug_vertex", NS::StringEncoding::UTF8StringEncoding);
            auto vertexMain = library->newFunction(vertexFuncName);

            auto fragmentFuncName = NS::String::string("debug_fragment", NS::StringEncoding::UTF8StringEncoding);
            auto fragmentMain = library->newFunction(fragmentFuncName);

            MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
            pipelineDesc->setVertexFunction(vertexMain);
            pipelineDesc->setFragmentFunction(fragmentMain);
            pipelineDesc->colorAttachments()->object(0)->setPixelFormat(swapchain->pixelFormat());
            pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

            // Enable blending for semi-transparent debug shapes
            auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
            colorAttachment->setBlendingEnabled(true);
            colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
            colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
            colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);

            debugDrawPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
            if (!debugDrawPipeline) {
                fmt::print(
                    "Warning: Could not create debug draw pipeline: {}\n",
                    error ? error->localizedDescription()->utf8String() : "unknown error"
                );
            }

            pipelineDesc->release();
            vertexMain->release();
            fragmentMain->release();
            library->release();
        }

        // Create depth stencil state for debug draw (read depth, don't write)
        MTL::DepthStencilDescriptor* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        depthDesc->setDepthWriteEnabled(false);// Don't write to depth buffer
        debugDrawDepthStencilState = NS::TransferPtr(device->newDepthStencilState(depthDesc));
        depthDesc->release();

        // Create per-frame vertex buffers for debug draw
        debugDrawVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        for (auto& buffer : debugDrawVertexBuffers) {
            buffer = nullptr;// Will be allocated on demand
        }
    }

    // Create 2D batch rendering pipeline
    {
        auto shaderSrc = readFile("shaders/2d_batch.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile 2D batch shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexFuncName = NS::String::string("batch2d_vertex", NS::StringEncoding::UTF8StringEncoding);
            auto vertexMain = library->newFunction(vertexFuncName);

            auto fragmentFuncName = NS::String::string("batch2d_fragment", NS::StringEncoding::UTF8StringEncoding);
            auto fragmentMain = library->newFunction(fragmentFuncName);

            if (!vertexMain || !fragmentMain) {
                fmt::print("Warning: Could not find batch2d shader functions\n");
            } else {
                // Create pipeline with alpha blending (default)
                {
                    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                    pipelineDesc->setVertexFunction(vertexMain);
                    pipelineDesc->setFragmentFunction(fragmentMain);
                    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                    colorAttachment->setBlendingEnabled(true);
                    colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                    colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                    colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);

                    batch2DPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                    if (!batch2DPipeline) {
                        fmt::print(
                            "Warning: Could not create 2D batch pipeline: {}\n",
                            error ? error->localizedDescription()->utf8String() : "unknown error"
                        );
                    }
                    pipelineDesc->release();
                }

                // Create pipeline with additive blending
                {
                    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                    pipelineDesc->setVertexFunction(vertexMain);
                    pipelineDesc->setFragmentFunction(fragmentMain);
                    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                    colorAttachment->setBlendingEnabled(true);
                    colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                    colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);

                    batch2DPipelineAdditive = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                    pipelineDesc->release();
                }

                // Create pipeline with multiply blending
                {
                    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                    pipelineDesc->setVertexFunction(vertexMain);
                    pipelineDesc->setFragmentFunction(fragmentMain);
                    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                    colorAttachment->setBlendingEnabled(true);
                    colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
                    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorDestinationColor);
                    colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorZero);
                    colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                    colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);

                    batch2DPipelineMultiply = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                    pipelineDesc->release();
                }

                vertexMain->release();
                fragmentMain->release();
            }
            library->release();
        }

        // Create depth stencil state for 2D batch (no depth testing/writing)
        MTL::DepthStencilDescriptor* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
        depthDesc->setDepthWriteEnabled(false);
        batch2DDepthStencilState = NS::TransferPtr(device->newDepthStencilState(depthDesc));
        depthDesc->release();

        // Create depth stencil state for 2D batch with depth testing (for world UI)
        MTL::DepthStencilDescriptor* depthDescEnabled = MTL::DepthStencilDescriptor::alloc()->init();
        depthDescEnabled->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        depthDescEnabled->setDepthWriteEnabled(true);
        batch2DDepthStencilStateEnabled = NS::TransferPtr(device->newDepthStencilState(depthDescEnabled));
        depthDescEnabled->release();

        // Create per-frame buffers for 2D batch
        batch2DVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch2DIndexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch2DUniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch3DVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch3DIndexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        batch3DUniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            batch2DVertexBuffers[i] = nullptr;// Allocated on demand
            batch2DIndexBuffers[i] = nullptr;// Allocated on demand
            batch2DUniformBuffers[i] =
                NS::TransferPtr(device->newBuffer(sizeof(Batch2DUniforms), MTL::ResourceStorageModeShared));

            batch3DVertexBuffers[i] = nullptr;// Allocated on demand
            batch3DIndexBuffers[i] = nullptr;// Allocated on demand
            batch3DUniformBuffers[i] =
                NS::TransferPtr(device->newBuffer(sizeof(Batch2DUniforms), MTL::ResourceStorageModeShared));
        }

        // Create 1x1 white texture for untextured primitives
        MTL::TextureDescriptor* texDesc = MTL::TextureDescriptor::alloc()->init();
        texDesc->setWidth(1);
        texDesc->setHeight(1);
        texDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        texDesc->setTextureType(MTL::TextureType2D);
        texDesc->setStorageMode(MTL::StorageModeShared);
        texDesc->setUsage(MTL::TextureUsageShaderRead);

        batch2DWhiteTexture = NS::TransferPtr(device->newTexture(texDesc));
        texDesc->release();

        // Fill with white pixel
        uint32_t whitePixel = 0xFFFFFFFF;
        batch2DWhiteTexture->replaceRegion(MTL::Region(0, 0, 1, 1), 0, &whitePixel, sizeof(uint32_t));

        // Create texture handle for the white texture
        batch2DWhiteTextureHandle.id = nextTextureID++;
        textures[batch2DWhiteTextureHandle.id] = batch2DWhiteTexture;

        fmt::print("2D batch rendering pipeline initialized\n");
    }

    // Create buffers
    frameDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& frameDataBuffer : frameDataBuffers) {
        frameDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(::FrameData), MTL::ResourceStorageModeManaged));
    }
    cameraDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& cameraDataBuffer : cameraDataBuffers) {
        cameraDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(::CameraData), MTL::ResourceStorageModeManaged));
    }
    instanceDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& instanceDataBuffer : instanceDataBuffers) {
        instanceDataBuffer =
            NS::TransferPtr(device->newBuffer(sizeof(::InstanceData) * MAX_INSTANCES, MTL::ResourceStorageModeManaged));
    }

    std::vector<::Particle> particles{ 1000 };
    testStorageBuffer =
        NS::TransferPtr(device->newBuffer(particles.size() * sizeof(::Particle), MTL::ResourceStorageModeManaged));
    memcpy(testStorageBuffer->contents(), particles.data(), particles.size() * sizeof(::Particle));
    testStorageBuffer->didModifyRange(NS::Range::Make(0, testStorageBuffer->length()));

    clusterBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& clusterBuffer : clusterBuffers) {
        clusterBuffer = NS::TransferPtr(device->newBuffer(
            clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ * sizeof(::Cluster), MTL::ResourceStorageModeManaged
        ));
    }

    // Create light scattering data buffers and initialize default settings
    lightScatteringDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& lsBuffer : lightScatteringDataBuffers) {
        lsBuffer = NS::TransferPtr(device->newBuffer(sizeof(LightScatteringData), MTL::ResourceStorageModeManaged));
    }

    // Initialize light scattering default settings
    lightScatteringSettings.sunScreenPos = glm::vec2(0.5f, 0.5f);
    lightScatteringSettings.screenSize = glm::vec2(1920.0f, 1080.0f);
    lightScatteringSettings.density = 1.0f;
    lightScatteringSettings.weight = 0.05f;
    lightScatteringSettings.decay = 0.97f;
    lightScatteringSettings.exposure = 0.3f;
    lightScatteringSettings.numSamples = 64;
    lightScatteringSettings.maxDistance = 1.0f;
    lightScatteringSettings.sunIntensity = 1.0f;
    lightScatteringSettings.mieG = 0.76f;
    lightScatteringSettings.sunColor = glm::vec3(1.0f, 0.95f, 0.9f);
    lightScatteringSettings.depthThreshold = 0.9999f;
    lightScatteringSettings.jitter = 0.5f;

    // ========================================================================
    // Volumetric Fog buffers and initialization
    // ========================================================================
    volumetricFogDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& fogBuffer : volumetricFogDataBuffers) {
        fogBuffer = NS::TransferPtr(device->newBuffer(sizeof(VolumetricFogData), MTL::ResourceStorageModeManaged));
    }

    // Initialize volumetric fog default settings
    volumetricFogSettings.fogDensity = 0.02f;
    volumetricFogSettings.fogHeightFalloff = 0.1f;
    volumetricFogSettings.fogBaseHeight = 0.0f;
    volumetricFogSettings.fogMaxHeight = 100.0f;
    volumetricFogSettings.scatteringCoeff = 0.5f;
    volumetricFogSettings.extinctionCoeff = 0.5f;
    volumetricFogSettings.anisotropy = 0.6f;
    volumetricFogSettings.ambientIntensity = 0.3f;
    volumetricFogSettings.nearPlane = 0.1f;
    volumetricFogSettings.farPlane = 500.0f;
    volumetricFogSettings.noiseScale = 0.01f;
    volumetricFogSettings.noiseIntensity = 0.5f;
    volumetricFogSettings.windSpeed = 1.0f;
    volumetricFogSettings.windDirection = glm::vec3(1.0f, 0.0f, 0.0f);
    volumetricFogSettings.temporalBlend = 0.1f;

    // ========================================================================
    // Volumetric Cloud buffers and initialization
    // ========================================================================
    volumetricCloudDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& cloudBuffer : volumetricCloudDataBuffers) {
        cloudBuffer = NS::TransferPtr(device->newBuffer(sizeof(VolumetricCloudData), MTL::ResourceStorageModeManaged));
    }

    // Initialize volumetric cloud default settings
    volumetricCloudSettings.cloudLayerBottom = 2000.0f;
    volumetricCloudSettings.cloudLayerTop = 12000.0f;
    volumetricCloudSettings.cloudLayerThickness = 2500.0f;
    volumetricCloudSettings.cloudCoverage = 0.25f;
    volumetricCloudSettings.cloudDensity = 0.3f;
    volumetricCloudSettings.cloudType = 0.5f;
    volumetricCloudSettings.erosionStrength = 0.3f;
    volumetricCloudSettings.shapeNoiseScale = 1.0f;
    volumetricCloudSettings.detailNoiseScale = 5.0f;
    volumetricCloudSettings.ambientIntensity = 0.001f;
    volumetricCloudSettings.silverLiningIntensity = 0.001f;
    volumetricCloudSettings.silverLiningSpread = 2.0f;
    volumetricCloudSettings.phaseG1 = 0.8f;
    volumetricCloudSettings.phaseG2 = -0.3f;
    volumetricCloudSettings.phaseBlend = 0.3f;
    volumetricCloudSettings.powderStrength = 0.5f;
    volumetricCloudSettings.windDirection = glm::vec3(1.0f, 0.0f, 0.0f);
    volumetricCloudSettings.windSpeed = 10.0f;
    volumetricCloudSettings.windOffset = glm::vec3(0.0f);
    volumetricCloudSettings.primarySteps = 64;
    volumetricCloudSettings.lightSteps = 6;
    volumetricCloudSettings.temporalBlend = 0.05f;

    // ========================================================================
    // Sun Flare buffers and initialization
    // ========================================================================
    sunFlareDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& flareBuffer : sunFlareDataBuffers) {
        flareBuffer = NS::TransferPtr(device->newBuffer(sizeof(SunFlareData), MTL::ResourceStorageModeManaged));
    }

    // Initialize sun flare default settings
    sunFlareSettings.sunIntensity = 1.0f;
    sunFlareSettings.visibility = 1.0f;
    sunFlareSettings.fadeEdge = 0.8f;
    sunFlareSettings.sunColor = glm::vec3(1.0f, 0.95f, 0.8f);
    sunFlareSettings.glowIntensity = 0.5f;
    sunFlareSettings.glowFalloff = 8.0f;
    sunFlareSettings.glowSize = 0.15f;
    sunFlareSettings.haloIntensity = 0.08f;
    sunFlareSettings.haloRadius = 0.09f;
    sunFlareSettings.haloWidth = 0.001f;
    sunFlareSettings.haloFalloff = 0.01f;
    sunFlareSettings.ghostCount = 10;
    sunFlareSettings.ghostSpacing = 0.3f;
    sunFlareSettings.ghostIntensity = 0.02f;
    sunFlareSettings.ghostSize = 0.3f;
    sunFlareSettings.ghostChromaticOffset = 0.015f;
    sunFlareSettings.ghostFalloff = 2.5f;
    sunFlareSettings.streakIntensity = 0.2f;
    sunFlareSettings.streakLength = 0.3f;
    sunFlareSettings.streakFalloff = 50.0f;
    sunFlareSettings.starburstIntensity = 0.15f;
    sunFlareSettings.starburstSize = 0.4f;
    sunFlareSettings.starburstPoints = 6;
    sunFlareSettings.starburstRotation = 0.0f;
    sunFlareSettings.dirtIntensity = 0.0f;
    sunFlareSettings.dirtScale = 10.0f;

    // Create atmosphere data buffer with default Earth-like settings
    atmosphereDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(AtmosphereData), MTL::ResourceStorageModeManaged));
    auto* atmosphereData = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
    atmosphereData->sunDirection = glm::normalize(glm::vec3(0.5f, 0.5f, 0.5f));
    atmosphereData->sunIntensity = 12.0f;
    atmosphereData->sunColor = glm::vec3(1.0f, 1.0f, 1.0f);
    atmosphereData->planetRadius = 6371e3f;// Earth radius in meters
    atmosphereData->atmosphereRadius = 6471e3f;// Atmosphere radius (100km above surface)
    atmosphereData->rayleighScaleHeight = 8500.0f;// Rayleigh scale height
    atmosphereData->mieScaleHeight = 1200.0f;// Mie scale height
    atmosphereData->miePreferredDirection = 0.758f;// Mie phase function g parameter
    atmosphereData->rayleighCoefficients = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);
    atmosphereData->mieCoefficient = 21e-6f;
    atmosphereData->exposure = 1.0f;
    atmosphereData->groundColor = glm::vec3(0.015f, 0.015f, 0.02f);// Default dark blue
    atmosphereDataBuffer->didModifyRange(NS::Range::Make(0, atmosphereDataBuffer->length()));

    // Create IBL capture data buffer
    iblCaptureDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(::IBLCaptureData), MTL::ResourceStorageModeManaged));

    // Create IBL textures
    const uint32_t envMapSize = 512;
    const uint32_t irradianceMapSize = 32;
    const uint32_t prefilterMapSize = 128;
    const uint32_t brdfLUTSize = 512;
    const uint32_t prefilterMipLevels = 5;

    // Environment cubemap (captured from atmosphere)
    MTL::TextureDescriptor* envMapDesc = MTL::TextureDescriptor::alloc()->init();
    envMapDesc->setTextureType(MTL::TextureTypeCube);
    envMapDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    envMapDesc->setWidth(envMapSize);
    envMapDesc->setHeight(envMapSize);
    envMapDesc->setMipmapLevelCount(calculateMipmapLevelCount(envMapSize, envMapSize));
    envMapDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    envMapDesc->setStorageMode(MTL::StorageModePrivate);
    environmentCubemap = NS::TransferPtr(device->newTexture(envMapDesc));
    envMapDesc->release();

    // Irradiance cubemap (diffuse IBL)
    MTL::TextureDescriptor* irradianceDesc = MTL::TextureDescriptor::alloc()->init();
    irradianceDesc->setTextureType(MTL::TextureTypeCube);
    irradianceDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    irradianceDesc->setWidth(irradianceMapSize);
    irradianceDesc->setHeight(irradianceMapSize);
    irradianceDesc->setMipmapLevelCount(1);
    irradianceDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    irradianceDesc->setStorageMode(MTL::StorageModePrivate);
    irradianceMap = NS::TransferPtr(device->newTexture(irradianceDesc));
    irradianceDesc->release();

    // Pre-filtered environment cubemap (specular IBL)
    MTL::TextureDescriptor* prefilterDesc = MTL::TextureDescriptor::alloc()->init();
    prefilterDesc->setTextureType(MTL::TextureTypeCube);
    prefilterDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    prefilterDesc->setWidth(prefilterMapSize);
    prefilterDesc->setHeight(prefilterMapSize);
    prefilterDesc->setMipmapLevelCount(prefilterMipLevels);
    prefilterDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    prefilterDesc->setStorageMode(MTL::StorageModePrivate);
    prefilterMap = NS::TransferPtr(device->newTexture(prefilterDesc));
    prefilterDesc->release();

    // BRDF LUT (2D texture)
    MTL::TextureDescriptor* brdfDesc = MTL::TextureDescriptor::alloc()->init();
    brdfDesc->setTextureType(MTL::TextureType2D);
    brdfDesc->setPixelFormat(MTL::PixelFormatRG16Float);
    brdfDesc->setWidth(brdfLUTSize);
    brdfDesc->setHeight(brdfLUTSize);
    brdfDesc->setMipmapLevelCount(1);
    brdfDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    brdfDesc->setStorageMode(MTL::StorageModePrivate);
    brdfLUT = NS::TransferPtr(device->newTexture(brdfDesc));
    brdfDesc->release();

    accelInstanceBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    TLASScratchBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    TLASBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        accelInstanceBuffers[i] = NS::TransferPtr(device->newBuffer(
            MAX_INSTANCES * sizeof(MTL::AccelerationStructureInstanceDescriptor), MTL::ResourceStorageModeManaged
        ));
        TLASScratchBuffers[i] = nullptr;
        TLASBuffers[i] = nullptr;
    }

    // Create textures
    defaultAlbedoTexture = createTexture(AssetManager::loadImage("textures/default_albedo.png")
    );// createTexture(AssetManager::loadImage("textures/viking_room.png"));
    defaultNormalTexture = createTexture(AssetManager::loadImage("textures/default_norm.png"));
    defaultORMTexture = createTexture(AssetManager::loadImage("textures/default_orm.png"));
    defaultEmissiveTexture = createTexture(AssetManager::loadImage("textures/default_emissive.png"));

    MTL::TextureDescriptor* depthStencilTextureDesc = MTL::TextureDescriptor::alloc()->init();
    depthStencilTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    depthStencilTextureDesc->setPixelFormat(MTL::PixelFormatDepth32Float);
    depthStencilTextureDesc->setWidth(swapchain->drawableSize().width);
    depthStencilTextureDesc->setHeight(swapchain->drawableSize().height);
    depthStencilTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    depthStencilTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    depthStencilRT_MS = NS::TransferPtr(device->newTexture(depthStencilTextureDesc));
    depthStencilTextureDesc->setTextureType(MTL::TextureType2D);
    depthStencilTextureDesc->setSampleCount(1);
    depthStencilTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    depthStencilRT = NS::TransferPtr(device->newTexture(depthStencilTextureDesc));
    depthStencilTextureDesc->release();

    MTL::TextureDescriptor* colorTextureDesc = MTL::TextureDescriptor::alloc()->init();
    colorTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    colorTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);// HDR format
    colorTextureDesc->setWidth(swapchain->drawableSize().width);
    colorTextureDesc->setHeight(swapchain->drawableSize().height);
    colorTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    colorTextureDesc->setUsage(MTL::TextureUsageRenderTarget);
    colorRT_MS = NS::TransferPtr(device->newTexture(colorTextureDesc));
    colorTextureDesc->setTextureType(MTL::TextureType2D);
    colorTextureDesc->setSampleCount(1);
    colorTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    colorRT = NS::TransferPtr(device->newTexture(colorTextureDesc));
    // Create tempColorRT for ping-pong post-processing (same format as colorRT)
    tempColorRT = NS::TransferPtr(device->newTexture(colorTextureDesc));
    colorTextureDesc->release();

    MTL::TextureDescriptor* normalTextureDesc = MTL::TextureDescriptor::alloc()->init();
    normalTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    normalTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);// HDR format
    normalTextureDesc->setWidth(swapchain->drawableSize().width);
    normalTextureDesc->setHeight(swapchain->drawableSize().height);
    normalTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    normalTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    normalRT_MS = NS::TransferPtr(device->newTexture(normalTextureDesc));
    normalTextureDesc->setTextureType(MTL::TextureType2D);
    normalTextureDesc->setSampleCount(1);
    normalTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    normalRT = NS::TransferPtr(device->newTexture(normalTextureDesc));
    normalTextureDesc->release();

    // Albedo RT for GIBS (stores albedo color from PrePass)
    MTL::TextureDescriptor* albedoTextureDesc = MTL::TextureDescriptor::alloc()->init();
    albedoTextureDesc->setTextureType(MTL::TextureType2DMultisample);
    albedoTextureDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    albedoTextureDesc->setWidth(swapchain->drawableSize().width);
    albedoTextureDesc->setHeight(swapchain->drawableSize().height);
    albedoTextureDesc->setSampleCount(NS::UInteger(MSAA_SAMPLE_COUNT));
    albedoTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    albedoRT_MS = NS::TransferPtr(device->newTexture(albedoTextureDesc));
    albedoTextureDesc->setTextureType(MTL::TextureType2D);
    albedoTextureDesc->setSampleCount(1);
    // RenderTarget usage is REQUIRED: this texture is the MSAA resolve target of PrePass
    albedoTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    albedoRT = NS::TransferPtr(device->newTexture(albedoTextureDesc));
    albedoTextureDesc->release();

    // Half resolution: 4x fewer (miss-dominated, expensive) shadow rays; consumers
    // sample at screen UVs with a bilinear sampler, which upsamples for free and
    // softens the 1-ray hard edges by ~2px. The kernel is resolution-agnostic, so
    // switching back to full res is just this size change.
    MTL::TextureDescriptor* shadowTextureDesc = MTL::TextureDescriptor::alloc()->init();
    shadowTextureDesc->setTextureType(MTL::TextureType2D);
    shadowTextureDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    shadowTextureDesc->setWidth((swapchain->drawableSize().width + 1) / 2);
    shadowTextureDesc->setHeight((swapchain->drawableSize().height + 1) / 2);
    shadowTextureDesc->setMipmapLevelCount(
        calculateMipmapLevelCount((swapchain->drawableSize().width + 1) / 2, (swapchain->drawableSize().height + 1) / 2)
    );
    shadowTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    shadowRT = NS::TransferPtr(device->newTexture(shadowTextureDesc));
    shadowRTGrayView = NS::TransferPtr(shadowRT->newTextureView(
        MTL::PixelFormatRGBA8Unorm,
        MTL::TextureType2D,
        NS::Range::Make(0, 1),
        NS::Range::Make(0, 1),
        MTL::TextureSwizzleChannels::Make(
            MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleOne
        )
    ));
    shadowTextureDesc->release();

    // Screen-space contact shadow RT (half-res, single-channel visibility).
    MTL::TextureDescriptor* sscsTextureDesc = MTL::TextureDescriptor::alloc()->init();
    sscsTextureDesc->setTextureType(MTL::TextureType2D);
    sscsTextureDesc->setPixelFormat(MTL::PixelFormatR8Unorm);
    sscsTextureDesc->setWidth((swapchain->drawableSize().width + 1) / 2);
    sscsTextureDesc->setHeight((swapchain->drawableSize().height + 1) / 2);
    sscsTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    sscsRT = NS::TransferPtr(device->newTexture(sscsTextureDesc));
    sscsRTGrayView = NS::TransferPtr(sscsRT->newTextureView(
        MTL::PixelFormatR8Unorm,
        MTL::TextureType2D,
        NS::Range::Make(0, 1),
        NS::Range::Make(0, 1),
        MTL::TextureSwizzleChannels::Make(
            MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleOne
        )
    ));
    sscsTextureDesc->release();

    // PSSM shadow maps: 2D texture array, 3 cascades × 4096×4096 Depth32
    {
        MTL::TextureDescriptor* pssmDesc = MTL::TextureDescriptor::alloc()->init();
        pssmDesc->setTextureType(MTL::TextureType2DArray);
        pssmDesc->setPixelFormat(MTL::PixelFormatDepth32Float);
        pssmDesc->setWidth(PSSM_SHADOW_MAP_SIZE);
        pssmDesc->setHeight(PSSM_SHADOW_MAP_SIZE);
        pssmDesc->setArrayLength(PSSM_CASCADE_COUNT);
        pssmDesc->setMipmapLevelCount(1);
        pssmDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        pssmShadowMaps = NS::TransferPtr(device->newTexture(pssmDesc));
        pssmDesc->release();

        // Per-slice texture2d views for ImGui display (depth2d_array can't be shown directly)
        for (uint32_t i = 0; i < PSSM_CASCADE_COUNT; i++) {
            pssmShadowMapViews[i] = NS::TransferPtr(
                pssmShadowMaps->newTextureView(
                    MTL::PixelFormatDepth32Float,
                    MTL::TextureType2D,
                    NS::Range::Make(0, 1),
                    NS::Range::Make(i, 1)
                )
            );
        }

        // Triple-buffered uniform buffers for PSSM data
        constexpr size_t pssmDataSize = sizeof(glm::mat4) * 3 + sizeof(glm::vec4) + sizeof(float) * 4;
        pssmDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        for (auto& buf : pssmDataBuffers) {
            buf = NS::TransferPtr(device->newBuffer(pssmDataSize, MTL::ResourceStorageModeShared));
        }
    }

    // Stochastic point shadow RTs: R16F (raw + denoised + history)
    {
        auto* desc = MTL::TextureDescriptor::alloc()->init();
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatR16Float);
        desc->setWidth(swapchain->drawableSize().width);
        desc->setHeight(swapchain->drawableSize().height);
        desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
        pointShadowRT         = NS::TransferPtr(device->newTexture(desc));
        pointShadowDenoisedRT = NS::TransferPtr(device->newTexture(desc));
        pointShadowHistoryRT  = NS::TransferPtr(device->newTexture(desc));
        desc->release();

        // Grayscale swizzle views for the ImGui previews (raw R16F renders red)
        auto grayView = [](MTL::Texture* tex) {
            return NS::TransferPtr(tex->newTextureView(
                MTL::PixelFormatR16Float,
                MTL::TextureType2D,
                NS::Range::Make(0, 1),
                NS::Range::Make(0, 1),
                MTL::TextureSwizzleChannels::Make(
                    MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleOne
                )
            ));
        };
        pointShadowRTGrayView         = grayView(pointShadowRT.get());
        pointShadowDenoisedRTGrayView = grayView(pointShadowDenoisedRT.get());

        // Initialize all three to 1.0 (fully lit). Prevents garbage in the first
        // frame's temporal history, and keeps point lights unshadowed when
        // raytracing is unsupported (the passes never write these textures).
        {
            const uint32_t texW = pointShadowRT->width();
            const uint32_t texH = pointShadowRT->height();
            std::vector<uint16_t> ones(size_t(texW) * texH, 0x3C00); // 1.0 in half-float
            for (auto* tex : { pointShadowRT.get(), pointShadowDenoisedRT.get(), pointShadowHistoryRT.get() }) {
                tex->replaceRegion(MTL::Region::Make2D(0, 0, texW, texH), 0, ones.data(), texW * sizeof(uint16_t));
            }
        }
    }

    // Screen-space resolved PSSM shadow (camera-aligned, debug display)
    {
        auto* desc = MTL::TextureDescriptor::alloc()->init();
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatR8Unorm);
        desc->setWidth(swapchain->drawableSize().width);
        desc->setHeight(swapchain->drawableSize().height);
        desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
        pssmShadowScreenRT = NS::TransferPtr(device->newTexture(desc));
        desc->release();

        pssmShadowScreenRTGrayView = NS::TransferPtr(pssmShadowScreenRT->newTextureView(
            MTL::PixelFormatR8Unorm,
            MTL::TextureType2D,
            NS::Range::Make(0, 1),
            NS::Range::Make(0, 1),
            MTL::TextureSwizzleChannels::Make(
                MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleOne
            )
        ));
    }

    // Half resolution: the AO chain kernels are resolution-agnostic and consumers
    // sample aoRT bilinearly at screen UVs, so this size is the only change needed
    MTL::TextureDescriptor* aoTextureDesc = MTL::TextureDescriptor::alloc()->init();
    aoTextureDesc->setTextureType(MTL::TextureType2D);
    aoTextureDesc->setPixelFormat(MTL::PixelFormatR16Float);
    aoTextureDesc->setWidth((swapchain->drawableSize().width + 1) / 2);
    aoTextureDesc->setHeight((swapchain->drawableSize().height + 1) / 2);
    aoTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    aoRT = NS::TransferPtr(device->newTexture(aoTextureDesc));
    aoTextureDesc->release();

    MTL::TextureDescriptor* velocityTextureDesc = MTL::TextureDescriptor::alloc()->init();
    velocityTextureDesc->setTextureType(MTL::TextureType2D);
    velocityTextureDesc->setPixelFormat(MTL::PixelFormatRG16Float);
    velocityTextureDesc->setWidth(swapchain->drawableSize().width);
    velocityTextureDesc->setHeight(swapchain->drawableSize().height);
    velocityTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    velocityRT = NS::TransferPtr(device->newTexture(velocityTextureDesc));
    velocityTextureDesc->release();

    // AO denoise chain targets (raygen → temporal history ping-pong → à-trous scratch).
    // Full resolution; the kernels are resolution-agnostic, so half-res later is
    // purely a size change here (ADR-008).
    MTL::TextureDescriptor* aoChainDesc = MTL::TextureDescriptor::alloc()->init();
    aoChainDesc->setTextureType(MTL::TextureType2D);
    aoChainDesc->setWidth((swapchain->drawableSize().width + 1) / 2);
    aoChainDesc->setHeight((swapchain->drawableSize().height + 1) / 2);
    aoChainDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    aoChainDesc->setPixelFormat(MTL::PixelFormatR16Float);
    aoRawRT = NS::TransferPtr(device->newTexture(aoChainDesc));
    // RGBA16F: (ao, view-space depth, octahedral normal) — see 3d_ao_temporal.metal
    aoChainDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
    aoHistoryRT[0] = NS::TransferPtr(device->newTexture(aoChainDesc));
    aoHistoryRT[1] = NS::TransferPtr(device->newTexture(aoChainDesc));
    aoScratchRT = NS::TransferPtr(device->newTexture(aoChainDesc));
    aoChainDesc->release();

    // Grayscale swizzle view of the single-channel AO target for the ImGui preview
    aoRTGrayView = NS::TransferPtr(aoRT->newTextureView(
        MTL::PixelFormatR16Float,
        MTL::TextureType2D,
        NS::Range::Make(0, 1),
        NS::Range::Make(0, 1),
        MTL::TextureSwizzleChannels::Make(
            MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleOne
        )
    ));

    // Create light scattering render target (HDR format for god rays)
    MTL::TextureDescriptor* lightScatteringTextureDesc = MTL::TextureDescriptor::alloc()->init();
    lightScatteringTextureDesc->setTextureType(MTL::TextureType2D);
    lightScatteringTextureDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);// HDR for bright rays
    lightScatteringTextureDesc->setWidth(swapchain->drawableSize().width);
    lightScatteringTextureDesc->setHeight(swapchain->drawableSize().height);
    lightScatteringTextureDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    lightScatteringRT = NS::TransferPtr(device->newTexture(lightScatteringTextureDesc));
    lightScatteringTextureDesc->release();

    // ========================================================================
    // Bloom render targets
    // ========================================================================

    // Brightness extraction RT (half resolution)
    {
        MTL::TextureDescriptor* bloomBrightnessDesc = MTL::TextureDescriptor::alloc()->init();
        bloomBrightnessDesc->setTextureType(MTL::TextureType2D);
        bloomBrightnessDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        bloomBrightnessDesc->setWidth(swapchain->drawableSize().width / 2);
        bloomBrightnessDesc->setHeight(swapchain->drawableSize().height / 2);
        bloomBrightnessDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        bloomBrightnessRT = NS::TransferPtr(device->newTexture(bloomBrightnessDesc));
        bloomBrightnessDesc->release();
    }

    // Bloom pyramid render targets (progressively smaller)
    bloomPyramidRTs.resize(BLOOM_PYRAMID_LEVELS);
    for (Uint32 i = 0; i < BLOOM_PYRAMID_LEVELS; i++) {
        Uint32 width = swapchain->drawableSize().width / (1 << (i + 1));
        Uint32 height = swapchain->drawableSize().height / (1 << (i + 1));
        width = std::max(width, 1u);
        height = std::max(height, 1u);

        MTL::TextureDescriptor* pyramidDesc = MTL::TextureDescriptor::alloc()->init();
        pyramidDesc->setTextureType(MTL::TextureType2D);
        pyramidDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        pyramidDesc->setWidth(width);
        pyramidDesc->setHeight(height);
        pyramidDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        bloomPyramidRTs[i] = NS::TransferPtr(device->newTexture(pyramidDesc));
        pyramidDesc->release();
    }

    // Final bloom result RT (full resolution)
    {
        MTL::TextureDescriptor* bloomResultDesc = MTL::TextureDescriptor::alloc()->init();
        bloomResultDesc->setTextureType(MTL::TextureType2D);
        bloomResultDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        bloomResultDesc->setWidth(swapchain->drawableSize().width);
        bloomResultDesc->setHeight(swapchain->drawableSize().height);
        bloomResultDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        bloomResultRT = NS::TransferPtr(device->newTexture(bloomResultDesc));
        bloomResultDesc->release();
    }

    // ========================================================================
    // Bloom pipelines
    // ========================================================================

    // Bloom brightness pipeline
    {
        auto shaderSrc = readFile("shaders/3d_bloom_brightness.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom brightness shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomBrightnessPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomBrightnessPipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom brightness pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Bloom downsample pipeline
    {
        auto shaderSrc = readFile("shaders/3d_bloom_downsample.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom downsample shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomDownsamplePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomDownsamplePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom downsample pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Bloom upsample pipeline
    {
        auto shaderSrc = readFile("shaders/3d_bloom_upsample.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom upsample shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomUpsamplePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomUpsamplePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom upsample pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Bloom composite pipeline
    {
        auto shaderSrc = readFile("shaders/3d_bloom_composite.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile bloom composite shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        bloomCompositePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!bloomCompositePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create bloom composite pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // ========================================================================
    // Volumetric Fog pipeline (simple height fog)
    // ========================================================================
    {
        auto shaderSrc = readFile("shaders/3d_volumetric_fog.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile volumetric fog shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexMain =
                library->newFunction(NS::String::string("volumetricFogVertex", NS::StringEncoding::UTF8StringEncoding));
            auto fragmentMain =
                library->newFunction(NS::String::string("simpleFogFragment", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentMain) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentMain);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                fogSimplePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!fogSimplePipeline) {
                    fmt::print(
                        "Warning: Could not create fog simple pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }

                pipelineDesc->release();
                vertexMain->release();
                fragmentMain->release();
            }
            library->release();
        }
        code->release();
    }

    // ========================================================================
    // Volumetric Cloud render targets (quarter resolution for performance)
    // ========================================================================
    {
        Uint32 cloudWidth = swapchain->drawableSize().width / 4;
        Uint32 cloudHeight = swapchain->drawableSize().height / 4;

        // Quarter-res cloud render target
        MTL::TextureDescriptor* cloudRTDesc = MTL::TextureDescriptor::alloc()->init();
        cloudRTDesc->setTextureType(MTL::TextureType2D);
        cloudRTDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        cloudRTDesc->setWidth(cloudWidth);
        cloudRTDesc->setHeight(cloudHeight);
        cloudRTDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        cloudRT = NS::TransferPtr(device->newTexture(cloudRTDesc));
        cloudRTDesc->release();

        // History buffer for temporal reprojection (same size as cloudRT)
        MTL::TextureDescriptor* cloudHistoryDesc = MTL::TextureDescriptor::alloc()->init();
        cloudHistoryDesc->setTextureType(MTL::TextureType2D);
        cloudHistoryDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        cloudHistoryDesc->setWidth(cloudWidth);
        cloudHistoryDesc->setHeight(cloudHeight);
        cloudHistoryDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        cloudHistoryRT = NS::TransferPtr(device->newTexture(cloudHistoryDesc));
        cloudHistoryDesc->release();
    }

    // ========================================================================
    // Volumetric Cloud pipelines
    // ========================================================================
    {
        auto shaderSrc = readFile("shaders/3d_volumetric_clouds.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile volumetric clouds shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            // Low-res cloud rendering pipeline (quarter resolution)
            auto vertexMain =
                library->newFunction(NS::String::string("cloudVertex", NS::StringEncoding::UTF8StringEncoding));
            auto fragmentLowRes =
                library->newFunction(NS::String::string("cloudFragmentLowRes", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentLowRes) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentLowRes);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudLowResPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudLowResPipeline) {
                    fmt::print(
                        "Warning: Could not create cloud low-res pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentLowRes->release();
            }

            // Temporal resolve pipeline
            auto fragmentTemporal =
                library->newFunction(NS::String::string("cloudTemporalResolve", NS::StringEncoding::UTF8StringEncoding)
                );

            if (vertexMain && fragmentTemporal) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentTemporal);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudTemporalResolvePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudTemporalResolvePipeline) {
                    fmt::print(
                        "Warning: Could not create cloud temporal resolve pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentTemporal->release();
            }

            // Upscale and composite pipeline
            auto fragmentComposite =
                library->newFunction(NS::String::string("cloudUpscaleComposite", NS::StringEncoding::UTF8StringEncoding)
                );

            if (vertexMain && fragmentComposite) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentComposite);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudCompositePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudCompositePipeline) {
                    fmt::print(
                        "Warning: Could not create cloud composite pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentComposite->release();
            }

            // Full-res cloud pipeline (fallback/debug)
            auto fragmentFull =
                library->newFunction(NS::String::string("cloudFragment", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentFull) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentFull);
                pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

                cloudRenderPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!cloudRenderPipeline) {
                    fmt::print(
                        "Warning: Could not create cloud render pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }
                pipelineDesc->release();
                fragmentFull->release();
            }

            if (vertexMain) vertexMain->release();
            library->release();
        }
        code->release();
    }

    // ========================================================================
    // Sun Flare pipeline
    // ========================================================================
    {
        auto shaderSrc = readFile("shaders/3d_sun_flare.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            fmt::print(
                "Warning: Could not compile sun flare shader: {}\n",
                error ? error->localizedDescription()->utf8String() : "unknown error"
            );
        } else {
            auto vertexMain =
                library->newFunction(NS::String::string("sunFlareVertex", NS::StringEncoding::UTF8StringEncoding));
            auto fragmentMain =
                library->newFunction(NS::String::string("sunFlareFragment", NS::StringEncoding::UTF8StringEncoding));

            if (vertexMain && fragmentMain) {
                auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
                pipelineDesc->setVertexFunction(vertexMain);
                pipelineDesc->setFragmentFunction(fragmentMain);
                auto colorAttach = pipelineDesc->colorAttachments()->object(0);
                colorAttach->setPixelFormat(MTL::PixelFormatRGBA16Float);
                // Additive blending: output = src + dst
                colorAttach->setBlendingEnabled(true);
                colorAttach->setSourceRGBBlendFactor(MTL::BlendFactorOne);
                colorAttach->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                colorAttach->setRgbBlendOperation(MTL::BlendOperationAdd);
                colorAttach->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                colorAttach->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
                colorAttach->setAlphaBlendOperation(MTL::BlendOperationAdd);

                sunFlarePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
                if (!sunFlarePipeline) {
                    fmt::print(
                        "Warning: Could not create sun flare pipeline: {}\n",
                        error ? error->localizedDescription()->utf8String() : "unknown error"
                    );
                }

                pipelineDesc->release();
                vertexMain->release();
                fragmentMain->release();
            }
            library->release();
        }
        code->release();
    }

    // ========================================================================
    // DOF (Tilt-Shift) render targets
    // ========================================================================

    // DOF CoC RT (full resolution, RGBA for color + CoC in alpha)
    {
        MTL::TextureDescriptor* dofCoCDesc = MTL::TextureDescriptor::alloc()->init();
        dofCoCDesc->setTextureType(MTL::TextureType2D);
        dofCoCDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        dofCoCDesc->setWidth(swapchain->drawableSize().width);
        dofCoCDesc->setHeight(swapchain->drawableSize().height);
        dofCoCDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        dofCoCRT = NS::TransferPtr(device->newTexture(dofCoCDesc));
        dofCoCDesc->release();
    }

    // DOF Blur RT (half resolution for performance)
    {
        MTL::TextureDescriptor* dofBlurDesc = MTL::TextureDescriptor::alloc()->init();
        dofBlurDesc->setTextureType(MTL::TextureType2D);
        dofBlurDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        dofBlurDesc->setWidth(swapchain->drawableSize().width / 2);
        dofBlurDesc->setHeight(swapchain->drawableSize().height / 2);
        dofBlurDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        dofBlurRT = NS::TransferPtr(device->newTexture(dofBlurDesc));
        dofBlurDesc->release();
    }

    // DOF Result RT (full resolution)
    {
        MTL::TextureDescriptor* dofResultDesc = MTL::TextureDescriptor::alloc()->init();
        dofResultDesc->setTextureType(MTL::TextureType2D);
        dofResultDesc->setPixelFormat(MTL::PixelFormatRGBA16Float);
        dofResultDesc->setWidth(swapchain->drawableSize().width);
        dofResultDesc->setHeight(swapchain->drawableSize().height);
        dofResultDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        dofResultRT = NS::TransferPtr(device->newTexture(dofResultDesc));
        dofResultDesc->release();
    }

    // ========================================================================
    // DOF (Tilt-Shift) pipelines
    // ========================================================================

    // DOF CoC pipeline
    {
        auto shaderSrc = readFile("shaders/3d_dof_coc.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile DOF CoC shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        dofCoCPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!dofCoCPipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create DOF CoC pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // DOF Blur pipeline
    {
        auto shaderSrc = readFile("shaders/3d_dof_blur.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile DOF Blur shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        dofBlurPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!dofBlurPipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create DOF Blur pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // DOF Composite pipeline
    {
        auto shaderSrc = readFile("shaders/3d_dof_composite.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(fmt::format(
                "Could not compile DOF Composite shader! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);

        dofCompositePipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!dofCompositePipeline) {
            throw std::runtime_error(fmt::format(
                "Could not create DOF Composite pipeline! Error: {}\n", error->localizedDescription()->utf8String()
            ));
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Create depth stencil states (for depth testing)
    MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
    depthStencilDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
    depthStencilDesc->setDepthWriteEnabled(true);
    depthStencilState = NS::TransferPtr(device->newDepthStencilState(depthStencilDesc));
    depthStencilDesc->release();

    // ========================================================================
    // Water rendering resources
    // ========================================================================

    // Create water pipeline with alpha blending
    {
        auto shaderSrc = readFile("shaders/3d_water.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        NS::Error* error = nullptr;
        MTL::Library* library = device->newLibrary(code, nullptr, &error);
        if (!library) {
            throw std::runtime_error(
                fmt::format("Could not compile water shader! Error: {}\n", error->localizedDescription()->utf8String())
            );
        }

        auto vertexMain =
            library->newFunction(NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding));
        auto fragmentMain =
            library->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));

        auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
        pipelineDesc->setVertexFunction(vertexMain);
        pipelineDesc->setFragmentFunction(fragmentMain);

        auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
        colorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float);
        colorAttachment->setBlendingEnabled(true);
        colorAttachment->setAlphaBlendOperation(MTL::BlendOperation::BlendOperationAdd);
        colorAttachment->setRgbBlendOperation(MTL::BlendOperation::BlendOperationAdd);
        colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
        colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        pipelineDesc->setSampleCount(1);// No MSAA for water pass

        waterPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
        if (!waterPipeline) {
            throw std::runtime_error(
                fmt::format("Could not create water pipeline! Error: {}\n", error->localizedDescription()->utf8String())
            );
        }

        code->release();
        library->release();
        vertexMain->release();
        fragmentMain->release();
        pipelineDesc->release();
    }

    // Create water depth stencil state (depth test but no write for transparency)
    {
        MTL::DepthStencilDescriptor* waterDepthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        waterDepthDesc->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
        waterDepthDesc->setDepthWriteEnabled(false);// Don't write depth for transparent water
        waterDepthStencilState = NS::TransferPtr(device->newDepthStencilState(waterDepthDesc));
        waterDepthDesc->release();
    }

    // Create water data buffers (triple buffered)
    waterDataBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& waterDataBuffer : waterDataBuffers) {
        waterDataBuffer = NS::TransferPtr(device->newBuffer(sizeof(WaterData), MTL::ResourceStorageModeManaged));
    }

    // Create water mesh (100x100 grid with 1.0 unit tiles, 5x5 UV tiling)
    {
        std::vector<WaterVertexData> waterVertices;
        std::vector<Uint32> waterIndices;
        MeshBuilder::buildWaterGrid(100, 100, 1.0f, 5.0f, 5.0f, waterVertices, waterIndices);

        waterVertexBuffer = NS::TransferPtr(
            device->newBuffer(waterVertices.size() * sizeof(WaterVertexData), MTL::ResourceStorageModeManaged)
        );
        memcpy(waterVertexBuffer->contents(), waterVertices.data(), waterVertices.size() * sizeof(WaterVertexData));
        waterVertexBuffer->didModifyRange(NS::Range::Make(0, waterVertexBuffer->length()));

        waterIndexBuffer =
            NS::TransferPtr(device->newBuffer(waterIndices.size() * sizeof(Uint32), MTL::ResourceStorageModeManaged));
        memcpy(waterIndexBuffer->contents(), waterIndices.data(), waterIndices.size() * sizeof(Uint32));
        waterIndexBuffer->didModifyRange(NS::Range::Make(0, waterIndexBuffer->length()));

        waterIndexCount = static_cast<Uint32>(waterIndices.size());
    }

    // Initialize default water transform (positioned above floor in Sponza)
    waterTransform.position = glm::vec3(0.0f, 0.5f, 0.0f);// y=0.5 to be above the floor
    waterTransform.scale = glm::vec3(1.0f, 1.0f, 1.0f);

    // Initialize default water settings
    waterSettings.modelMatrix = glm::mat4(1.0f);
    waterSettings.surfaceColor = glm::vec4(0.465f, 0.797f, 0.991f, 1.0f);
    waterSettings.refractionColor = glm::vec4(0.003f, 0.599f, 0.812f, 1.0f);
    // SSR settings: x=step size, y=max steps (0 to disable), z=refinement steps, w=distance factor
    waterSettings.ssrSettings = glm::vec4(0.5f, 0.0f, 10.0f, 20.0f);// Set y=0 to disable SSR
    waterSettings.normalMapScroll = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    waterSettings.normalMapScrollSpeed = glm::vec2(0.01f, 0.01f);
    waterSettings.refractionDistortionFactor = 0.04f;
    waterSettings.refractionHeightFactor = 2.5f;
    waterSettings.refractionDistanceFactor = 15.0f;
    waterSettings.depthSofteningDistance = 0.5f;
    waterSettings.foamHeightStart = 0.8f;
    waterSettings.foamFadeDistance = 0.4f;
    waterSettings.foamTiling = 2.0f;
    waterSettings.foamAngleExponent = 80.0f;
    waterSettings.roughness = 0.08f;
    waterSettings.reflectance = 0.55f;
    waterSettings.specIntensity = 125.0f;
    waterSettings.foamBrightness = 4.0f;
    // waterSettings.tessellationFactor = 7.0f;
    waterSettings.dampeningFactor = 5.0f;
    waterSettings.waveCount = 2;

    // Wave 1
    waterSettings.waves[0].direction = glm::vec3(0.3f, 0.0f, -0.7f);
    waterSettings.waves[0].steepness = 1.79f;
    waterSettings.waves[0].waveLength = 3.75f;
    waterSettings.waves[0].amplitude = 0.85f;
    waterSettings.waves[0].speed = 1.21f;

    // Wave 2
    waterSettings.waves[1].direction = glm::vec3(0.5f, 0.0f, -0.2f);
    waterSettings.waves[1].steepness = 1.79f;
    waterSettings.waves[1].waveLength = 4.1f;
    waterSettings.waves[1].amplitude = 0.52f;
    waterSettings.waves[1].speed = 1.03f;

    // Create placeholder water textures (procedural normal maps and noise)
    // Water normal map 1 - a simple procedural normal texture
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> normalData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize * 6.28f;
                float fy = static_cast<float>(y) / texSize * 6.28f;
                float nx = sin(fx * 2.0f + fy) * 0.5f + 0.5f;
                float ny = sin(fy * 2.0f + fx * 0.5f) * 0.5f + 0.5f;
                float nz = 1.0f;
                glm::vec3 n = glm::normalize(glm::vec3((nx - 0.5f) * 0.3f, (ny - 0.5f) * 0.3f, nz));
                Uint32 idx = (y * texSize + x) * 4;
                normalData[idx + 0] = static_cast<Uint8>((n.x * 0.5f + 0.5f) * 255);
                normalData[idx + 1] = static_cast<Uint8>((n.y * 0.5f + 0.5f) * 255);
                normalData[idx + 2] = static_cast<Uint8>((n.z * 0.5f + 0.5f) * 255);
                normalData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_normal1";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = normalData;
        waterNormalMap1 = createTexture(img);
    }

    // Water normal map 2 - different pattern
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> normalData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize * 6.28f;
                float fy = static_cast<float>(y) / texSize * 6.28f;
                float nx = cos(fx * 3.0f - fy * 0.5f) * 0.5f + 0.5f;
                float ny = cos(fy * 3.0f + fx * 0.7f) * 0.5f + 0.5f;
                float nz = 1.0f;
                glm::vec3 n = glm::normalize(glm::vec3((nx - 0.5f) * 0.25f, (ny - 0.5f) * 0.25f, nz));
                Uint32 idx = (y * texSize + x) * 4;
                normalData[idx + 0] = static_cast<Uint8>((n.x * 0.5f + 0.5f) * 255);
                normalData[idx + 1] = static_cast<Uint8>((n.y * 0.5f + 0.5f) * 255);
                normalData[idx + 2] = static_cast<Uint8>((n.z * 0.5f + 0.5f) * 255);
                normalData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_normal2";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = normalData;
        waterNormalMap2 = createTexture(img);
    }

    // Water foam texture - white with noise pattern
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> foamData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize;
                float fy = static_cast<float>(y) / texSize;
                float noise = (sin(fx * 50.0f) * cos(fy * 50.0f) + 1.0f) * 0.5f;
                noise *= (sin(fx * 30.0f + fy * 20.0f) + 1.0f) * 0.5f;
                auto v = static_cast<Uint8>(noise * 200 + 55);
                Uint32 idx = (y * texSize + x) * 4;
                foamData[idx + 0] = v;
                foamData[idx + 1] = v;
                foamData[idx + 2] = v;
                foamData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_foam";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = foamData;
        waterFoamMap = createTexture(img);
    }

    // Water noise texture - Perlin-like noise pattern
    {
        const Uint32 texSize = 256;
        std::vector<Uint8> noiseData(texSize * texSize * 4);
        for (Uint32 y = 0; y < texSize; ++y) {
            for (Uint32 x = 0; x < texSize; ++x) {
                float fx = static_cast<float>(x) / texSize;
                float fy = static_cast<float>(y) / texSize;
                float noise = 0.0f;
                noise += (sin(fx * 20.0f) * cos(fy * 20.0f) + 1.0f) * 0.25f;
                noise += (sin(fx * 40.0f + 0.3f) * cos(fy * 40.0f + 0.7f) + 1.0f) * 0.125f;
                noise += (sin(fx * 80.0f + 1.5f) * cos(fy * 80.0f + 2.1f) + 1.0f) * 0.0625f;
                noise = glm::clamp(noise, 0.0f, 1.0f);
                auto v = static_cast<Uint8>(noise * 255);
                Uint32 idx = (y * texSize + x) * 4;
                noiseData[idx + 0] = v;
                noiseData[idx + 1] = v;
                noiseData[idx + 2] = v;
                noiseData[idx + 3] = 255;
            }
        }
        auto img = std::make_shared<Image>();
        img->uri = "procedural_water_noise";
        img->width = texSize;
        img->height = texSize;
        img->channelCount = 4;
        img->byteArray = noiseData;
        waterNoiseMap = createTexture(img);
    }

    // Create placeholder environment cube map (simple gradient sky)
    {
        const Uint32 faceSize = 64;
        MTL::TextureDescriptor* cubeDesc = MTL::TextureDescriptor::alloc()->init();
        cubeDesc->setTextureType(MTL::TextureTypeCube);
        cubeDesc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        cubeDesc->setWidth(faceSize);
        cubeDesc->setHeight(faceSize);
        cubeDesc->setUsage(MTL::TextureUsageShaderRead);

        environmentCubeMap = NS::TransferPtr(device->newTexture(cubeDesc));

        std::vector<Uint8> faceData(faceSize * faceSize * 4);
        for (Uint32 face = 0; face < 6; ++face) {
            for (Uint32 y = 0; y < faceSize; ++y) {
                for (Uint32 x = 0; x < faceSize; ++x) {
                    float t = static_cast<float>(y) / faceSize;
                    auto r = static_cast<Uint8>((0.4f + t * 0.3f) * 255);
                    auto g = static_cast<Uint8>((0.6f + t * 0.2f) * 255);
                    auto b = static_cast<Uint8>((0.8f + t * 0.1f) * 255);
                    Uint32 idx = (y * faceSize + x) * 4;
                    faceData[idx + 0] = r;
                    faceData[idx + 1] = g;
                    faceData[idx + 2] = b;
                    faceData[idx + 3] = 255;
                }
            }
            environmentCubeMap->replaceRegion(
                MTL::Region(0, 0, 0, faceSize, faceSize, 1), 0, face, faceData.data(), faceSize * 4, 0
            );
        }

        cubeDesc->release();
    }

    // ========================================================================
    // Particle system initialization
    // ========================================================================

    // Create particle compute pipelines
    {
        NS::Error* error = nullptr;
        auto shaderSrc = readFile("shaders/3d_particle.metal");
        auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
        MTL::CompileOptions* options = nullptr;
        auto library = NS::TransferPtr(device->newLibrary(code, options, &error));

        if (!library) {
            fmt::print("Failed to compile particle compute shader: {}\n", error->localizedDescription()->utf8String());
        } else {
            // Create force pipeline
            auto forceFuncName = NS::String::string("particleForce", NS::StringEncoding::UTF8StringEncoding);
            auto forceFunc = library->newFunction(forceFuncName);
            if (forceFunc) {
                particleForcePipeline = NS::TransferPtr(device->newComputePipelineState(forceFunc, &error));
                forceFunc->release();
            }

            // Create integrate pipeline
            auto integrateFuncName = NS::String::string("particleIntegrate", NS::StringEncoding::UTF8StringEncoding);
            auto integrateFunc = library->newFunction(integrateFuncName);
            if (integrateFunc) {
                particleIntegratePipeline = NS::TransferPtr(device->newComputePipelineState(integrateFunc, &error));
                integrateFunc->release();
            }
        }
        code->release();
    }

    // Create particle render pipeline - compile from source file
    {
        NS::Error* error = nullptr;
        auto shaderSource = readFile("shaders/3d_particle.metal");
        auto source = NS::String::string(shaderSource.c_str(), NS::UTF8StringEncoding);
        auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
        auto library = NS::TransferPtr(device->newLibrary(source, options.get(), &error));

        if (!library) {
            fmt::print("Failed to compile particle render shader: {}\n", error->localizedDescription()->utf8String());
        } else {
            auto vertexFunc =
                NS::TransferPtr(library->newFunction(NS::String::string("particleVertex", NS::UTF8StringEncoding)));
            auto fragFunc =
                NS::TransferPtr(library->newFunction(NS::String::string("particleFragment", NS::UTF8StringEncoding)));

            if (!vertexFunc || !fragFunc) {
                fmt::print("Failed to find particle vertex/fragment functions\n");
            } else {
                auto pipelineDesc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
                pipelineDesc->setVertexFunction(vertexFunc.get());
                pipelineDesc->setFragmentFunction(fragFunc.get());

                // Color attachment with additive blending
                auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
                colorAttachment->setPixelFormat(MTL::PixelFormatRGBA16Float);
                colorAttachment->setBlendingEnabled(true);
                colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorOne);
                colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
                colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);

                pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

                particleRenderPipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc.get(), &error));
                if (error) {
                    fmt::print(
                        "Failed to create particle render pipeline: {}\n", error->localizedDescription()->utf8String()
                    );
                }
            }
        }

        // Create depth stencil state (depth test enabled, write disabled)
        auto depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
        depthDesc->setDepthWriteEnabled(false);
        particleDepthStencilState = NS::TransferPtr(device->newDepthStencilState(depthDesc));
        depthDesc->release();
    }

    // Create particle buffers
    // Single particle buffer for persistent state (not triple-buffered)
    size_t particleBufferSize = sizeof(GPUParticleData) * MAX_PARTICLES;
    particleBuffer = NS::TransferPtr(device->newBuffer(particleBufferSize, MTL::ResourceStorageModeShared));

    // Per-frame uniform buffers (triple-buffered).
    // attractor buffer holds MAX_PARTICLE_ATTRACTORS elements.
    particleSimParamsBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    particleAttractorBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        particleSimParamsBuffers[i] =
            NS::TransferPtr(device->newBuffer(sizeof(ParticleSimParams), MTL::ResourceStorageModeShared));
        particleAttractorBuffers[i] =
            NS::TransferPtr(device->newBuffer(sizeof(ParticleAttractor) * MAX_PARTICLE_ATTRACTORS,
                                              MTL::ResourceStorageModeShared));
    }

    // Zero-fill: lifetime=0/age=0 → all slots start as dead and are skipped
    // by the compute shaders. ECS emitters claim slots via claimParticleSlots()
    // and fill them with uploadParticles().
    std::memset(particleBuffer->contents(), 0, particleBufferSize);
    // particleCount starts at 0; updated by claimParticleSlots() high-water mark.

    fmt::print("Particle pool initialized ({} slots, ECS-driven)\n", MAX_PARTICLES);
}

auto Renderer_Metal::stage(std::shared_ptr<RenderScene> scene) -> void {
    ZoneScoped;

    // Lights
    size_t directionalLightsSize = std::max((size_t)1, scene->directionalLights.size());
    directionalLightBuffer = NS::TransferPtr(
        device->newBuffer(directionalLightsSize * sizeof(::DirectionalLight), MTL::ResourceStorageModeManaged)
    );
    if (!scene->directionalLights.empty()) {
        memcpy(
            directionalLightBuffer->contents(),
            scene->directionalLights.data(),
            scene->directionalLights.size() * sizeof(::DirectionalLight)
        );
    }
    directionalLightBuffer->didModifyRange(NS::Range::Make(0, directionalLightBuffer->length()));

    size_t pointLightsSize = std::max((size_t)1, scene->pointLights.size());
    pointLightBuffer = NS::TransferPtr(
        device->newBuffer(pointLightsSize * sizeof(::PointLight), MTL::ResourceStorageModeManaged)
    );
    if (!scene->pointLights.empty()) {
        memcpy(pointLightBuffer->contents(), scene->pointLights.data(), scene->pointLights.size() * sizeof(::PointLight));
    }
    pointLightBuffer->didModifyRange(NS::Range::Make(0, pointLightBuffer->length()));

    size_t rectLightsSize = std::max((size_t)1, scene->rectLights.size());
    rectLightBuffer = NS::TransferPtr(
        device->newBuffer(rectLightsSize * sizeof(::RectLight), MTL::ResourceStorageModeManaged)
    );
    if (!scene->rectLights.empty()) {
        memcpy(rectLightBuffer->contents(), scene->rectLights.data(), scene->rectLights.size() * sizeof(::RectLight));
    }
    rectLightBuffer->didModifyRange(NS::Range::Make(0, rectLightBuffer->length()));

    // Textures
    for (auto& img : scene->images) {
        img->texture = createTexture(img);
    }

    // Pipelines & materials
    if (scene->materials.empty()) {
        // TODO: create default material
    }
    for (auto& mat : scene->materials) {
        // pipelines[mat->pipeline] = createPipeline();
        materialIDs[mat] = nextMaterialID++;
    }
    size_t materialsSize = std::max((size_t)1, scene->materials.size());
    materialDataBuffer = NS::TransferPtr(
        device->newBuffer(materialsSize * sizeof(::MaterialData), MTL::ResourceStorageModeManaged)
    );

    // Buffers
    scene->vertexBuffer = createVertexBuffer(scene->vertices);
    scene->indexBuffer = createIndexBuffer(scene->indices);

    auto cmd = queue->commandBuffer();

    const auto stageMesh = [&](std::shared_ptr<Vapor::Mesh>& mesh) {
        if (m_supportsRaytracing) {
            auto geomDesc = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
            geomDesc->setVertexBuffer(getBuffer(scene->vertexBuffer).get());
            geomDesc->setVertexStride(sizeof(VertexData));
            geomDesc->setVertexFormat(MTL::AttributeFormatFloat3);
            geomDesc->setVertexBufferOffset(mesh->vertexOffset * sizeof(VertexData) + offsetof(VertexData, position));
            geomDesc->setIndexBuffer(getBuffer(scene->indexBuffer).get());
            geomDesc->setIndexType(MTL::IndexTypeUInt32);
            geomDesc->setIndexBufferOffset(mesh->indexOffset * sizeof(Uint32));
            geomDesc->setTriangleCount(mesh->indexCount / 3);
            geomDesc->setOpaque(true);

            auto accelDesc = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
            NS::Object* descriptors[] = { geomDesc.get() };
            auto geomArray = NS::TransferPtr(NS::Array::array(descriptors, 1));
            accelDesc->setGeometryDescriptors(geomArray.get());

            auto accelSizes = device->accelerationStructureSizes(accelDesc.get());
            auto accelStruct = NS::TransferPtr(device->newAccelerationStructure(accelSizes.accelerationStructureSize));
            auto scratchBuffer =
                NS::TransferPtr(device->newBuffer(accelSizes.buildScratchBufferSize, MTL::ResourceStorageModePrivate));

            auto encoder = cmd->accelerationStructureCommandEncoder();
            encoder->buildAccelerationStructure(accelStruct.get(), accelDesc.get(), scratchBuffer.get(), 0);
            encoder->endEncoding();

            BLASs.push_back(accelStruct);
        }

        mesh->materialID = materialIDs[mesh->material];
        mesh->instanceID = nextInstanceID++;
    };

    for (auto& mesh : scene->stagedMeshes) {
        stageMesh(mesh);
    }

    cmd->commit();
}

// ============================================================================
// Frame model (parity with the RHI renderer / IRenderer):
//   beginFrame()          — acquire drawable + command buffer, backend ImGui
//                           NewFrame. Caller then calls ImGui::NewFrame().
//   invokeImGuiCallback() — no-op here: the engine debug panel and the app/
//                           engine callbacks are built inside draw() (main's
//                           data model needs the scene, available in draw()).
//   draw()                — record all render passes (no ImGui pass, no present)
//   (caller: ImGui::Render())
//   endFrame()            — submit ImGui draw data, present, commit.
// ============================================================================
void Renderer_Metal::beginFrame(const CameraRenderData& /*camera*/) {
    // Acquire the drawable (autoreleased; managed by the system AutoreleasePool)
    // and a fresh command buffer for the frame.
    currentDrawable = swapchain->nextDrawable();
    currentCommandBuffer = currentDrawable ? queue->commandBuffer() : nullptr;

    // ImGui backend NewFrame must run before the caller's ImGui::NewFrame().
    // The Metal backend takes a render-pass descriptor to learn the target
    // pixel format; point it at the drawable when we have one.
    auto imguiPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
    if (currentDrawable) {
        imguiPassDesc->colorAttachments()->object(0)->setTexture(currentDrawable->texture());
    }
    ImGui_ImplMetal_NewFrame(imguiPassDesc.get());
    ImGui_ImplSDL3_NewFrame();
}

void Renderer_Metal::invokeImGuiCallback() {
    // Intentionally empty — see beginFrame()/draw() notes above. The panel and
    // callbacks are built during draw() where the staged scene is available.
}

void Renderer_Metal::endFrame() {
    // Submit ImGui draw data on top of the rendered frame, then present.
    // The caller has already called ImGui::Render(); do NOT call it again here.
    if (!currentDrawable || !currentCommandBuffer) {
        return;
    }

    if (ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount > 0) {
        auto imguiPassDesc = NS::TransferPtr(MTL::RenderPassDescriptor::renderPassDescriptor());
        auto imguiPassColorRT = imguiPassDesc->colorAttachments()->object(0);
        imguiPassColorRT->setLoadAction(MTL::LoadActionLoad);
        imguiPassColorRT->setStoreAction(MTL::StoreActionStore);
        imguiPassColorRT->setTexture(currentDrawable->texture());
        auto encoder = currentCommandBuffer->renderCommandEncoder(imguiPassDesc.get());
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), currentCommandBuffer, encoder);
        encoder->endEncoding();
    }

    currentCommandBuffer->presentDrawable(currentDrawable);
    currentCommandBuffer->commit();
    // Do not release the drawable: nextDrawable() returns an autoreleased object
    // retained by presentDrawable() until presentation completes.

    frameNumber++;
    currentFrameInFlight = (currentFrameInFlight + 1) % MAX_FRAMES_IN_FLIGHT;
    currentCommandBuffer = nullptr;
    currentDrawable = nullptr;
}

auto Renderer_Metal::draw(std::shared_ptr<RenderScene> scene, Camera& camera) -> void {
    ZoneScoped;
    FrameMark;

    // The drawable and command buffer are acquired in beginFrame(); ImGui draw
    // data is submitted and the drawable is presented in endFrame(). This
    // function only records the render passes.
    auto surface = currentDrawable;
    auto cmd = currentCommandBuffer;
    if (!surface || !cmd) {
        return;
    }

    // ==========================================================================
    // Prepare frame data
    // ==========================================================================
    auto time = (float)SDL_GetTicks() / 1000.0f;

    auto* frameData = reinterpret_cast<::FrameData*>(frameDataBuffers[currentFrameInFlight]->contents());
    frameData->frameNumber = frameNumber;
    frameData->time = time;
    frameData->deltaTime = 0.016f;// TODO:
    frameDataBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, frameDataBuffers[currentFrameInFlight]->length())
    );

    float near = camera.near();
    float far = camera.far();
    glm::vec3 camPos = camera.getEye();
    glm::mat4 proj = camera.getProjMatrix();
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 invProj = glm::inverse(proj);
    glm::mat4 invView = glm::inverse(view);
    auto* cameraData = reinterpret_cast<::CameraData*>(cameraDataBuffers[currentFrameInFlight]->contents());
    cameraData->proj = proj;
    cameraData->view = view;
    cameraData->invProj = invProj;
    cameraData->invView = invView;
    cameraData->near = near;
    cameraData->far = far;
    cameraData->position = camPos;
    cameraDataBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, cameraDataBuffers[currentFrameInFlight]->length())
    );

    // Reallocate light buffers if the ECS has added lights since stage() was called
    // (LightGatherSystem populates scene->directionalLights / pointLights after staging)
    const size_t dirLightBytes   = std::max(scene->directionalLights.size(), (size_t)1) * sizeof(::DirectionalLight);
    const size_t pointLightBytes = std::max(scene->pointLights.size(),       (size_t)1) * sizeof(::PointLight);
    if (!directionalLightBuffer || directionalLightBuffer->length() < dirLightBytes) {
        directionalLightBuffer = NS::TransferPtr(
            device->newBuffer(dirLightBytes, MTL::ResourceStorageModeManaged));
    }
    if (!pointLightBuffer || pointLightBuffer->length() < pointLightBytes) {
        pointLightBuffer = NS::TransferPtr(
            device->newBuffer(pointLightBytes, MTL::ResourceStorageModeManaged));
    }

    auto* dirLights = reinterpret_cast<::DirectionalLight*>(directionalLightBuffer->contents());
    for (size_t i = 0; i < scene->directionalLights.size(); ++i) {
        dirLights[i].direction = scene->directionalLights[i].direction;
        dirLights[i].color = scene->directionalLights[i].color;
        dirLights[i].intensity = scene->directionalLights[i].intensity;
    }
    directionalLightBuffer->didModifyRange(NS::Range::Make(0, directionalLightBuffer->length()));

    auto* atmosphereData = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
    if (!scene->directionalLights.empty()) {
        const auto& sunLight = scene->directionalLights[0];
        atmosphereData->sunDirection = -glm::normalize(sunLight.direction);
        atmosphereData->sunColor = sunLight.color;
        atmosphereData->sunIntensity = sunLight.intensity;
    }

    auto* pointLights = reinterpret_cast<::PointLight*>(pointLightBuffer->contents());
    for (size_t i = 0; i < scene->pointLights.size(); ++i) {
        pointLights[i].position = scene->pointLights[i].position;
        pointLights[i].color = scene->pointLights[i].color;
        pointLights[i].intensity = scene->pointLights[i].intensity;
        pointLights[i].radius = scene->pointLights[i].radius;
    }
    pointLightBuffer->didModifyRange(NS::Range::Make(0, pointLightBuffer->length()));

    const size_t rectLightBytes = std::max(scene->rectLights.size(), (size_t)1) * sizeof(::RectLight);
    if (!rectLightBuffer || rectLightBuffer->length() < rectLightBytes) {
        rectLightBuffer = NS::TransferPtr(device->newBuffer(rectLightBytes, MTL::ResourceStorageModeManaged));
    }
    if (!scene->rectLights.empty()) {
        memcpy(rectLightBuffer->contents(), scene->rectLights.data(), scene->rectLights.size() * sizeof(::RectLight));
        rectLightBuffer->didModifyRange(NS::Range::Make(0, rectLightBytes));
    }

    auto* materialData = reinterpret_cast<::MaterialData*>(materialDataBuffer->contents());
    for (size_t i = 0; i < scene->materials.size(); ++i) {
        const auto& mat = scene->materials[i];
        materialData[i] = ::MaterialData{ .baseColorFactor = mat->baseColorFactor,
                                        .normalScale = mat->normalScale,
                                        .metallicFactor = mat->metallicFactor,
                                        .roughnessFactor = mat->roughnessFactor,
                                        .occlusionStrength = mat->occlusionStrength,
                                        .emissiveFactor = mat->emissiveFactor,
                                        .emissiveStrength = mat->emissiveStrength,
                                        .subsurface = mat->subsurface,
                                        .specular = mat->specular,
                                        .specularTint = mat->specularTint,
                                        .anisotropic = mat->anisotropic,
                                        .sheen = mat->sheen,
                                        .sheenTint = mat->sheenTint,
                                        .clearcoat = mat->clearcoat,
                                        .clearcoatGloss = mat->clearcoatGloss,
                                        .prototypeUVMode = static_cast<float>(mat->prototypeUVMode),
                                        .uvScale = mat->uvScale,
                                        .iblEnabled = mat->useIBL ? 1.0f : 0.0f };
    }
    materialDataBuffer->didModifyRange(NS::Range::Make(0, materialDataBuffer->length()));

    // Update instance data from ECS (set by draw(registry, scene, camera) before calling this)
    instances.clear();
    accelInstances.clear();
    instanceBatches.clear();
    if (!pendingEcsInstances.empty()) {
        instances = pendingEcsInstances;
        instanceBatches = pendingEcsBatches;
        if (!pendingEcsAccelInstances.empty()) {
            accelInstances = pendingEcsAccelInstances;
        }
    }

    if (instances.size() > MAX_INSTANCES) {// TODO: reallocate when needed
        fmt::print("Warning: Instance count ({}) exceeds MAX_INSTANCES ({})\n", instances.size(), MAX_INSTANCES);
    }
    // TODO: avoid updating the entire instance data buffer every frame
    memcpy(
        instanceDataBuffers[currentFrameInFlight]->contents(), instances.data(), instances.size() * sizeof(::InstanceData)
    );
    instanceDataBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, instanceDataBuffers[currentFrameInFlight]->length())
    );
    memcpy(
        accelInstanceBuffers[currentFrameInFlight]->contents(),
        accelInstances.data(),
        accelInstances.size() * sizeof(MTL::AccelerationStructureInstanceDescriptor)
    );
    accelInstanceBuffers[currentFrameInFlight]->didModifyRange(
        NS::Range::Make(0, accelInstanceBuffers[currentFrameInFlight]->length())
    );

    // ==========================================================================
    // Set up rendering context for passes
    // (currentCommandBuffer / currentDrawable were set in beginFrame())
    // ==========================================================================
    currentScene = scene;
    currentCamera = &camera;
    drawCount = 0;

    // ==========================================================================
    // Update GIBS (Global Illumination Based on Surfels)
    // ==========================================================================
    if (gibsEnabled && gibsManager) {
        // Initialize GI textures if needed (or resize on window resize)
        Uint32 screenW = static_cast<Uint32>(surface->texture()->width());
        Uint32 screenH = static_cast<Uint32>(surface->texture()->height());
        gibsManager->initTextures(screenW, screenH);

        // Begin frame for GIBS
        gibsManager->beginFrame(currentFrameInFlight);

        // Update GIBS data with camera and lighting info
        glm::mat4 viewProj = proj * view;
        glm::mat4 invViewProj = glm::inverse(viewProj);

        // Get sun direction and color from first directional light
        glm::vec3 sunDir = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 sunColor = glm::vec3(1.0f);
        float sunIntensity = 1.0f;
        if (!scene->directionalLights.empty()) {
            const auto& sunLight = scene->directionalLights[0];
            sunDir = glm::normalize(sunLight.direction);
            sunColor = sunLight.color;
            sunIntensity = sunLight.intensity;
        }

        gibsManager->updateGIBSData(viewProj, invViewProj, camPos, sunDir, sunColor, sunIntensity);
    }

    // ==========================================================================
    // Initialize RmlUI if not already initialized (delayed initialization)
    // ==========================================================================
    auto* engineCore = Vapor::EngineCore::Get();
    if (engineCore) {
        auto* rmluiManager = engineCore->getRmlUiManager();
        if (!rmluiManager) {
            // Initialize RmlUI with the LOGICAL window size (points), not the
            // drawable size. RmlUi lays out in logical coordinates and the
            // render projection is logical too (RmlRendererMetal::SetViewport),
            // while the physical framebuffer viewport handles Retina upscaling.
            // Using the drawable (physical, 2x on Retina) here doubled every
            // UI element.
            int width = 0, height = 0;
            SDL_GetWindowSize(window, &width, &height);
            if (engineCore->initRmlUI(width, height)) {
                // Initialize renderer UI support (sets RenderInterface and finalizes RmlUI)
                initUI();
            }
        }
    }

    // ==========================================================================
    // Build ImGui UI (before ImGuiPass executes)
    // ------------------------------------------------------------------------
    // The ImGui backend NewFrame (ImGui_ImplMetal_NewFrame / ImGui_ImplSDL3_
    // NewFrame) runs in beginFrame(); the caller invokes ImGui::NewFrame()
    // between beginFrame() and here. This block only builds the windows.
    // ==========================================================================

    // F1 toggles the engine ImGui overlay on/off.
    if (ImGui::IsKeyPressed(ImGuiKey_F1))
        m_imGuiVisible = !m_imGuiVisible;

    // Per-frame engine hook (recording capture + F2 hotkey). Runs whether or not
    // the overlay is visible so recording keeps working with the UI hidden.
    if (m_imGuiFrameCallback)
        m_imGuiFrameCallback();

    // ImGui::DockSpaceOverViewport();

    if (m_imGuiVisible) {
    ImGui::Begin("Engine");

    if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
        // ImGui::Text("Frame rate: %.3f ms/frame (%.1f FPS)", 1000.0f * deltaTime, 1.0f / deltaTime);
        ImGui::Text(
            "Average frame rate: %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate
        );
        ImGui::ColorEdit3("Clear color", (float*)&clearColor);

        ImGui::Separator();

        // Aspect-correct preview sized for actually diagnosing content issues
        auto rtPreview = [](const char* label, MTL::Texture* tex) {
            if (!tex) return;
            if (ImGui::TreeNode(label)) {
                float aspect = tex->height() > 0 ? float(tex->width()) / float(tex->height()) : 1.0f;
                ImGui::Text("%llu x %llu", (unsigned long long)tex->width(), (unsigned long long)tex->height());
                ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(320, 320 / aspect));
                ImGui::TreePop();
            }
        };

        if (ImGui::TreeNode("RTs")) {
            ImGui::Separator();
            rtPreview("Scene Color RT", colorRT.get());
            rtPreview("Scene Depth RT", depthStencilRT.get());
            // Shadow results consumed by the PBR shader (all screen-space)
            rtPreview("Near Shadow", shadowRTGrayView.get()); // RT-based here; same role as Vulkan's near map
            rtPreview("Contact Shadow (SSCS)", sscsRTGrayView.get());
            rtPreview("Point Shadow", pointShadowDenoisedRTGrayView.get());
            rtPreview("PSSM Shadow", pssmShadowScreenRTGrayView.get());
            rtPreview("Raytraced AO", aoRTGrayView.get()); // grayscale swizzle view (raw R16F renders red)
            rtPreview("Scene Normal RT", normalRT.get());
            rtPreview("Velocity RT", velocityRT.get());
            rtPreview("Light Scattering RT", lightScatteringRT.get());
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Shadow Debug")) {
            ImGui::Separator();

            // --- Light gathering status (answers: did lights reach the renderer?) ---
            ImGui::Text("Scene lights:  dir %zu | point %zu | rect %zu",
                scene->directionalLights.size(), scene->pointLights.size(), scene->rectLights.size());
            ImGui::Text("Raytracing: %s | TLAS: %s",
                m_supportsRaytracing ? "supported" : "OFF",
                (m_supportsRaytracing && TLASBuffers[currentFrameInFlight]) ? "built" : "null");
            ImGui::Text("Frame: %u", frameNumber);

            // --- PSSM cascade splits (read back from shared GPU buffer) ---
            if (pssmDataBuffers[currentFrameInFlight]) {
                const auto* pssmGPU = reinterpret_cast<const uint8_t*>(pssmDataBuffers[currentFrameInFlight]->contents());
                glm::vec4 splits;
                memcpy(&splits, pssmGPU + sizeof(glm::mat4) * 3, sizeof(glm::vec4));
                ImGui::Text("Cascade splits (view depth): RT<%.1f | C1<%.1f | C2<%.1f | C3<%.1f",
                    splits.x, splits.y, splits.z, splits.w);
            }
            ImGui::SliderFloat("Near shadow distance", &pssmRTMaxDist, 5.0f, 200.0f);
            // Skip the directional shadow term (perf/debug). Pushed to the PBR
            // shader at buffer(12) as mainDebugFlags bit 1 — same as Vulkan.
            bool skipShadow = (mainDebugFlags & 2u) != 0u;
            if (ImGui::Checkbox("Skip shadow", &skipShadow))
                mainDebugFlags = (mainDebugFlags & ~2u) | (skipShadow ? 2u : 0u);
            ImGui::Checkbox("Contact shadows (SSCS)", &sscsEnabled);
            if (sscsEnabled) {
                ImGui::SliderFloat("SSCS length", &sscsLength, 0.05f, 2.0f);
                ImGui::SliderFloat("SSCS thickness", &sscsThickness, 0.05f, 2.0f);
            }

            // --- PSSM PCF & blend controls ---
            const char* pcfCounts[] = { "4", "8", "16", "32" };
            const uint32_t pcfValues[] = { 4u, 8u, 16u, 32u };
            int pcfIdx = 2; // default 16
            for (int i = 0; i < 4; i++) if (pssmPcfSampleCount == pcfValues[i]) pcfIdx = i;
            if (ImGui::Combo("PCF samples", &pcfIdx, pcfCounts, 4)) {
                pssmPcfSampleCount = pcfValues[pcfIdx];
            }
            ImGui::SliderFloat("RT<->PSSM blend", &pssmRTBlendScale, 0.0f, 0.25f, "%.3f");
            ImGui::SliderFloat("Cascade blend range", &pssmCascadeBlendRange, 0.0f, 30.0f);
            ImGui::Checkbox("Visualize cascades", &pssmDebugVisualize);
            ImGui::TextWrapped("Cascade colors: green = RT, red = C1, blue = C2, yellow = C3");

            // --- Stochastic point shadow debug mode ---
            const char* psDebugModes[] = { "Visibility (normal)", "Tile light-count heatmap" };
            int psMode = static_cast<int>(pointShadowDebugMode);
            if (ImGui::Combo("Point shadow view", &psMode, psDebugModes, 2)) {
                pointShadowDebugMode = static_cast<uint32_t>(psMode);
            }
            if (pointShadowDebugMode == 1) {
                ImGui::TextWrapped("Heatmap: black = tile has 0 lights (culling problem if lights exist), brighter = more lights (8+ = white). Shown in 'Point Shadow (raw / heatmap)' below.");
            }

            // --- Intermediate shadow textures ---
            // Raw = pre-temporal (also shows the tile heatmap in debug mode)
            rtPreview("Point Shadow (raw / heatmap)", pointShadowRTGrayView.get());
            // Light-space cascade depth maps
            for (uint32_t i = 0; i < PSSM_CASCADE_COUNT; i++) {
                rtPreview(fmt::format("PSSM Cascade {} (light-space depth)", i + 1).c_str(), pssmShadowMapViews[i].get());
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Materials")) {
            ImGui::Separator();
            for (auto m : scene->materials) {
                if (!m) {
                    continue;
                }
                ImGui::PushID(m.get());
                if (ImGui::TreeNode(fmt::format("Mat #{}", m->name).c_str())) {
                    // TODO: show error image if texture is not uploaded
                    if (m->albedoMap) {
                        ImGui::Text("Albedo Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->albedoMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->normalMap) {
                        ImGui::Text("Normal Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->normalMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->metallicMap) {
                        ImGui::Text("Metallic Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->metallicMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->roughnessMap) {
                        ImGui::Text("Roughness Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->roughnessMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->occlusionMap) {
                        ImGui::Text("Occlusion Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->occlusionMap->texture).get(), ImVec2(64, 64));
                    }
                    if (m->emissiveMap) {
                        ImGui::Text("Emissive Map");
                        ImGui::Image((ImTextureID)(intptr_t)getTexture(m->emissiveMap->texture).get(), ImVec2(64, 64));
                    }
                    ImGui::ColorEdit4("Base Color Factor", (float*)&m->baseColorFactor);
                    ImGui::DragFloat("Normal Scale", &m->normalScale, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Roughness Factor", &m->roughnessFactor, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Metallic Factor", &m->metallicFactor, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Occlusion Strength", &m->occlusionStrength, .05f, 0.0f, 5.0f);
                    ImGui::ColorEdit3("Emissive Color Factor", (float*)&m->emissiveFactor);
                    ImGui::DragFloat("Emissive Strength", &m->emissiveStrength, .05f, 0.0f, 5.0f);
                    ImGui::DragFloat("Subsurface", &m->subsurface, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Specular", &m->specular, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Specular Tint", &m->specularTint, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Anisotropic", &m->anisotropic, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Sheen", &m->sheen, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Sheen Tint", &m->sheenTint, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Clearcoat", &m->clearcoat, .01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Clearcoat Gloss", &m->clearcoatGloss, .01f, 0.0f, 1.0f);
                    ImGui::Separator();
                    // Material type (read-only: determines which shader pipeline is used)
                    const char* typeLabel = (m->materialType == Vapor::MaterialType::Iridescent)
                        ? "Iridescent (electroplating)"
                        : "PBR";
                    ImGui::LabelText("Material Type", "%s", typeLabel);
                    // useIBL is editable: changes take effect next frame via materialDataBuffer upload
                    ImGui::Checkbox("Use IBL", &m->useIBL);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Lights")) {
            ImGui::Separator();
            for (auto& l : scene->directionalLights) {
                ImGui::Text("Directional Light");
                ImGui::PushID(&l);
                ImGui::DragFloat3("Direction", (float*)&l.direction, 0.1f);
                ImGui::ColorEdit3("Color", (float*)&l.color);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f);
                ImGui::PopID();
            }
            for (auto& l : scene->pointLights) {
                ImGui::Text("Point Light");
                ImGui::PushID(&l);
                ImGui::DragFloat3("Position", (float*)&l.position, 0.1f);
                ImGui::ColorEdit3("Color", (float*)&l.color);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f);
                ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.0001f);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Atmosphere")) {
            ImGui::Separator();
            auto* atmos = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
            bool atmosChanged = false;

            if (!scene->directionalLights.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Sun synced from first directional light");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "The first directional light in the scene is automatically used as the sun for atmosphere "
                        "rendering."
                    );
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No directional lights - using default sun");
            }
            ImGui::Separator();

            atmosChanged |= ImGui::DragFloat3("Sun Direction", (float*)&atmos->sunDirection, 0.01f, -1.0f, 1.0f);
            if (atmosChanged) {
                atmos->sunDirection = glm::normalize(atmos->sunDirection);
                // Sync back to first directional light if it exists (negate to match convention)
                if (!scene->directionalLights.empty()) {
                    scene->directionalLights[0].direction = -atmos->sunDirection;
                }
            }
            atmosChanged |= ImGui::DragFloat("Sun Intensity", &atmos->sunIntensity, 0.5f, 0.0f, 100.0f);
            if (atmosChanged && !scene->directionalLights.empty()) {
                scene->directionalLights[0].intensity = atmos->sunIntensity;
            }
            atmosChanged |= ImGui::ColorEdit3("Sun Color", (float*)&atmos->sunColor);
            if (atmosChanged && !scene->directionalLights.empty()) {
                scene->directionalLights[0].color = atmos->sunColor;
            }
            atmosChanged |= ImGui::DragFloat("Exposure", &atmos->exposure, 0.01f, 0.01f, 10.0f);
            atmosChanged |= ImGui::ColorEdit3("Ground Color", (float*)&atmos->groundColor);

            if (ImGui::TreeNode("Advanced")) {
                atmosChanged |=
                    ImGui::DragFloat("Planet Radius (m)", &atmos->planetRadius, 1000.0f, 1e3f, 1e8f, "%.0f");
                atmosChanged |=
                    ImGui::DragFloat("Atmosphere Radius (m)", &atmos->atmosphereRadius, 1000.0f, 1e3f, 1e8f, "%.0f");
                atmosChanged |=
                    ImGui::DragFloat("Rayleigh Scale Height", &atmos->rayleighScaleHeight, 100.0f, 100.0f, 50000.0f);
                atmosChanged |= ImGui::DragFloat("Mie Scale Height", &atmos->mieScaleHeight, 100.0f, 100.0f, 10000.0f);
                atmosChanged |=
                    ImGui::DragFloat("Mie Direction (g)", &atmos->miePreferredDirection, 0.01f, -0.999f, 0.999f);

                float rayleighR = atmos->rayleighCoefficients.r * 1e6f;
                float rayleighG = atmos->rayleighCoefficients.g * 1e6f;
                float rayleighB = atmos->rayleighCoefficients.b * 1e6f;
                bool rayleighChanged = false;
                rayleighChanged |= ImGui::DragFloat("Rayleigh R (x1e-6)", &rayleighR, 0.1f, 0.0f, 100.0f);
                rayleighChanged |= ImGui::DragFloat("Rayleigh G (x1e-6)", &rayleighG, 0.1f, 0.0f, 100.0f);
                rayleighChanged |= ImGui::DragFloat("Rayleigh B (x1e-6)", &rayleighB, 0.1f, 0.0f, 100.0f);
                if (rayleighChanged) {
                    atmos->rayleighCoefficients = glm::vec3(rayleighR * 1e-6f, rayleighG * 1e-6f, rayleighB * 1e-6f);
                    atmosChanged = true;
                }

                float mie = atmos->mieCoefficient * 1e6f;
                if (ImGui::DragFloat("Mie Coeff (x1e-6)", &mie, 0.1f, 0.0f, 100.0f)) {
                    atmos->mieCoefficient = mie * 1e-6f;
                    atmosChanged = true;
                }

                if (ImGui::Button("Reset to Earth Defaults")) {
                    atmos->planetRadius = 6371e3f;
                    atmos->atmosphereRadius = 6471e3f;
                    atmos->rayleighScaleHeight = 8500.0f;
                    atmos->mieScaleHeight = 1200.0f;
                    atmos->miePreferredDirection = 0.758f;
                    atmos->rayleighCoefficients = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);
                    atmos->mieCoefficient = 21e-6f;
                    atmosChanged = true;
                }

                ImGui::TreePop();
            }

            ImGui::Separator();
            ImGui::Text("IBL Status: %s", iblNeedsUpdate ? "Pending Update" : "Up to Date");
            if (ImGui::Button("Refresh IBL")) {
                iblNeedsUpdate = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Re-bakes the sky to IBL cubemaps.\nAutomatically triggered when atmosphere parameters change."
                );
            }

            if (atmosChanged) {
                atmosphereDataBuffer->didModifyRange(NS::Range::Make(0, atmosphereDataBuffer->length()));
                iblNeedsUpdate = true;// Trigger IBL update when atmosphere changes
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scene Geometry")) {
            ImGui::Separator();
            ImGui::Text("Total vertices: %zu", scene->vertices.size());
            ImGui::Text("Total indices: %zu", scene->indices.size());
            ImGui::Text("Staged meshes: %zu", scene->stagedMeshes.size());
            for (size_t mi = 0; mi < scene->stagedMeshes.size(); ++mi) {
                auto& mesh = scene->stagedMeshes[mi];
                ImGui::PushID(static_cast<int>(mi));
                if (ImGui::TreeNode(fmt::format("Mesh #{}", mi).c_str())) {
                    ImGui::Text("Vertex count: %u", mesh->vertexCount);
                    ImGui::Text("Vertex offset: %u", mesh->vertexOffset);
                    ImGui::Text("Index count: %u", mesh->indexCount);
                    ImGui::Text("Index offset: %u", mesh->indexOffset);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Water Settings")) {
            ImGui::Separator();
            ImGui::Checkbox("Water Enabled", &waterEnabled);

            if (ImGui::TreeNode("Transform")) {
                ImGui::DragFloat3("Position", (float*)&waterTransform.position, 0.1f);
                ImGui::DragFloat3("Scale", (float*)&waterTransform.scale, 0.1f, 0.1f, 10.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Colors")) {
                ImGui::ColorEdit4("Surface Color", (float*)&waterSettings.surfaceColor);
                ImGui::ColorEdit4("Refraction Color", (float*)&waterSettings.refractionColor);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Wave Parameters")) {
                int waveCount = static_cast<int>(waterSettings.waveCount);
                if (ImGui::SliderInt("Wave Count", &waveCount, 0, 4)) {
                    waterSettings.waveCount = static_cast<Uint32>(waveCount);
                }

                for (Uint32 i = 0; i < waterSettings.waveCount && i < 4; ++i) {
                    ImGui::PushID(i);
                    if (ImGui::TreeNode(fmt::format("Wave {}", i + 1).c_str())) {
                        ImGui::DragFloat3("Direction", (float*)&waterSettings.waves[i].direction, 0.01f, -1.0f, 1.0f);
                        ImGui::DragFloat("Steepness", &waterSettings.waves[i].steepness, 0.01f, 0.0f, 3.0f);
                        ImGui::DragFloat("Wave Length", &waterSettings.waves[i].waveLength, 0.1f, 0.1f, 20.0f);
                        ImGui::DragFloat("Amplitude", &waterSettings.waves[i].amplitude, 0.01f, 0.0f, 5.0f);
                        ImGui::DragFloat("Speed", &waterSettings.waves[i].speed, 0.01f, 0.0f, 5.0f);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Visual Parameters")) {
                ImGui::DragFloat("Roughness", &waterSettings.roughness, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Reflectance", &waterSettings.reflectance, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Spec Intensity", &waterSettings.specIntensity, 1.0f, 0.0f, 500.0f);
                ImGui::DragFloat("Dampening Factor", &waterSettings.dampeningFactor, 0.1f, 0.1f, 20.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Normal Map Scroll")) {
                ImGui::DragFloat2("Scroll Dir 1", (float*)&waterSettings.normalMapScroll, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat2("Scroll Dir 2", (float*)&waterSettings.normalMapScroll.z, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat2("Scroll Speed", (float*)&waterSettings.normalMapScrollSpeed, 0.001f, 0.0f, 0.1f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Refraction")) {
                ImGui::DragFloat("Distortion Factor", &waterSettings.refractionDistortionFactor, 0.001f, 0.0f, 0.2f);
                ImGui::DragFloat("Height Factor", &waterSettings.refractionHeightFactor, 0.1f, 0.0f, 10.0f);
                ImGui::DragFloat("Distance Factor", &waterSettings.refractionDistanceFactor, 0.5f, 1.0f, 100.0f);
                ImGui::DragFloat("Depth Softening", &waterSettings.depthSofteningDistance, 0.01f, 0.01f, 5.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Foam")) {
                ImGui::DragFloat("Height Start", &waterSettings.foamHeightStart, 0.01f, 0.0f, 2.0f);
                ImGui::DragFloat("Fade Distance", &waterSettings.foamFadeDistance, 0.01f, 0.01f, 2.0f);
                ImGui::DragFloat("Tiling", &waterSettings.foamTiling, 0.1f, 0.1f, 10.0f);
                ImGui::DragFloat("Angle Exponent", &waterSettings.foamAngleExponent, 1.0f, 1.0f, 200.0f);
                ImGui::DragFloat("Brightness", &waterSettings.foamBrightness, 0.1f, 0.1f, 10.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("SSR Settings")) {
                ImGui::DragFloat("Step Size", &waterSettings.ssrSettings.x, 0.1f, 0.1f, 2.0f);
                ImGui::DragFloat("Max Steps (0=disabled)", &waterSettings.ssrSettings.y, 1.0f, 0.0f, 100.0f);
                ImGui::DragFloat("Refinement Steps", &waterSettings.ssrSettings.z, 1.0f, 1.0f, 50.0f);
                ImGui::DragFloat("Distance Factor", &waterSettings.ssrSettings.w, 1.0f, 1.0f, 100.0f);
                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Ambient Occlusion")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &aoEnabled);
            if (aoEnabled) {
                ImGui::Combo("Method", &aoMethod, "Ray Traced\0Screen Space\0");
            }
            ImGui::TextDisabled("Attenuates IBL/ambient only; both methods share the denoise chain");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Global Illumination (GIBS)")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &gibsEnabled);
            if (gibsEnabled && gibsManager) {
                ImGui::Text("Active Surfels: %u / %u", gibsManager->getActiveSurfelCount(), gibsManager->getMaxSurfels());
                ImGui::Text("Rays/Surfel: %u", gibsManager->getRaysPerSurfel());
                ImGui::Text("Resolution Scale: %.2fx", gibsManager->getResolutionScale());
                // Raw GPU counters: [0] = this frame's generation budget cursor (reset each
                // frame), [1] = persistent pool cursor. If both stay 0 the generation kernel
                // is not producing surfels (check depth/bounds); if [1] > 0 but Active stays
                // 0 the CPU readback is broken.
                glm::uvec2 rawCounters = gibsManager->getRawCounters();
                ImGui::Text("GPU counters: budget=%u pool=%u", rawCounters.x, rawCounters.y);
                if (ImGui::Button("Reset Surfels")) {
                    gibsManager->resetSurfels();
                }

                int qualityIdx = static_cast<int>(gibsQuality);
                if (ImGui::Combo("Quality", &qualityIdx, "Low\0Medium\0High\0Ultra\0")) {
                    gibsQuality = static_cast<GIBSQuality>(qualityIdx);
                    gibsManager->setQuality(gibsQuality);
                }
            }
            ImGui::TextDisabled("Surfel-based indirect diffuse lighting");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Light Scattering (God Rays)")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &lightScatteringEnabled);

            if (lightScatteringEnabled) {
                ImGui::Separator();

                auto* debugAtmos = reinterpret_cast<AtmosphereData*>(atmosphereDataBuffer->contents());
                glm::vec3 debugSunDir = glm::normalize(debugAtmos->sunDirection);
                glm::vec3 debugCamPos = camera.getEye();
                glm::vec3 debugSunWorldPos = debugCamPos + debugSunDir * 10000.0f;
                glm::mat4 debugViewProj = camera.getProjMatrix() * camera.getViewMatrix();
                glm::vec4 debugSunClip = debugViewProj * glm::vec4(debugSunWorldPos, 1.0f);

                if (debugSunClip.w <= 0.0f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Sun behind camera");
                }

                ImGui::Text("Ray Marching");
                int numSamples = static_cast<int>(lightScatteringSettings.numSamples);
                if (ImGui::SliderInt("Samples", &numSamples, 8, 128)) {
                    lightScatteringSettings.numSamples = static_cast<Uint32>(numSamples);
                }
                ImGui::DragFloat("Max Distance", &lightScatteringSettings.maxDistance, 0.01f, 0.1f, 2.0f);

                ImGui::Separator();
                ImGui::Text("Scattering Properties");
                ImGui::DragFloat("Density", &lightScatteringSettings.density, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Weight", &lightScatteringSettings.weight, 0.001f, 0.001f, 0.1f);
                ImGui::DragFloat("Decay", &lightScatteringSettings.decay, 0.001f, 0.9f, 1.0f);
                ImGui::DragFloat("Exposure", &lightScatteringSettings.exposure, 0.01f, 0.0f, 2.0f);

                ImGui::Separator();
                ImGui::Text("Light Properties");
                ImGui::DragFloat("Sun Intensity", &lightScatteringSettings.sunIntensity, 0.1f, 0.0f, 10.0f);
                ImGui::DragFloat("Mie G (Phase)", &lightScatteringSettings.mieG, 0.01f, -0.99f, 0.99f);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Mie scattering direction:\n< 0: backscatter\n= 0: isotropic\n> 0: forward scatter (sun glare)"
                    );
                }

                ImGui::Separator();
                ImGui::Text("Advanced");
                ImGui::DragFloat(
                    "Depth Threshold", &lightScatteringSettings.depthThreshold, 0.0001f, 0.99f, 1.0f, "%.4f"
                );
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Depth value above which pixels are considered 'sky'.\nHigher = only sky contributes to rays."
                    );
                }
                ImGui::DragFloat("Temporal Jitter", &lightScatteringSettings.jitter, 0.01f, 0.0f, 1.0f);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Jitter amount for temporal anti-aliasing.\nReduces banding artifacts.");
                }

                if (ImGui::Button("Reset to Defaults")) {
                    lightScatteringSettings.density = 1.0f;
                    lightScatteringSettings.weight = 0.01f;
                    lightScatteringSettings.decay = 0.97f;
                    lightScatteringSettings.exposure = 0.3f;
                    lightScatteringSettings.numSamples = 64;
                    lightScatteringSettings.maxDistance = 1.0f;
                    lightScatteringSettings.sunIntensity = 1.0f;
                    lightScatteringSettings.mieG = 0.76f;
                    lightScatteringSettings.depthThreshold = 0.9999f;
                    lightScatteringSettings.jitter = 0.5f;
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Height Fog")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &volumetricFogEnabled);

            if (volumetricFogEnabled) {
                ImGui::Separator();
                ImGui::Text("Fog Parameters");
                ImGui::DragFloat("Density", &volumetricFogSettings.fogDensity, 0.001f, 0.0f, 0.5f);
                ImGui::DragFloat("Height Falloff", &volumetricFogSettings.fogHeightFalloff, 0.01f, 0.001f, 1.0f);
                ImGui::DragFloat("Base Height", &volumetricFogSettings.fogBaseHeight, 1.0f, -100.0f, 100.0f);
                ImGui::DragFloat("Max Height", &volumetricFogSettings.fogMaxHeight, 10.0f, 0.0f, 500.0f);

                ImGui::Separator();
                ImGui::Text("Scattering");
                ImGui::DragFloat("Anisotropy", &volumetricFogSettings.anisotropy, 0.01f, -0.99f, 0.99f);
                ImGui::DragFloat("Ambient Intensity", &volumetricFogSettings.ambientIntensity, 0.01f, 0.0f, 2.0f);

                if (ImGui::Button("Reset to Defaults")) {
                    volumetricFogSettings.fogDensity = 0.02f;
                    volumetricFogSettings.fogHeightFalloff = 0.1f;
                    volumetricFogSettings.fogBaseHeight = 0.0f;
                    volumetricFogSettings.fogMaxHeight = 100.0f;
                    volumetricFogSettings.anisotropy = 0.6f;
                    volumetricFogSettings.ambientIntensity = 0.3f;
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Volumetric Clouds")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &volumetricCloudsEnabled);

            if (volumetricCloudsEnabled) {
                ImGui::Separator();
                ImGui::Text("Cloud Layer");
                ImGui::DragFloat("Bottom (m)", &volumetricCloudSettings.cloudLayerBottom, 100.0f, 0.0f, 10000.0f);
                ImGui::DragFloat("Top (m)", &volumetricCloudSettings.cloudLayerTop, 100.0f, 0.0f, 15000.0f);
                ImGui::DragFloat("Coverage", &volumetricCloudSettings.cloudCoverage, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Density", &volumetricCloudSettings.cloudDensity, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Type (Stratus-Cumulus)", &volumetricCloudSettings.cloudType, 0.01f, 0.0f, 1.0f);

                ImGui::Separator();
                ImGui::Text("Lighting");
                ImGui::DragFloat("Ambient", &volumetricCloudSettings.ambientIntensity, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Silver Lining", &volumetricCloudSettings.silverLiningIntensity, 0.01f, 0.0f, 2.0f);
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Sun Flare (Lens Flare)")) {
            ImGui::Separator();
            ImGui::Checkbox("Enabled", &sunFlareEnabled);

            ImGui::DragFloat("Sun Intensity", &sunFlareSettings.sunIntensity, 0.1f, 0.0f, 100.0f);
            ImGui::ColorEdit3("Sun Color", &sunFlareSettings.sunColor[0]);
            ImGui::DragFloat("Fade Edge", &sunFlareSettings.fadeEdge, 0.01f, 0.0f, 1.0f);

            ImGui::Separator();
            ImGui::Text("Glow");
            ImGui::DragFloat("Glow Intensity", &sunFlareSettings.glowIntensity, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Glow Falloff", &sunFlareSettings.glowFalloff, 0.1f, 0.1f, 20.0f);
            ImGui::DragFloat("Glow Size", &sunFlareSettings.glowSize, 0.01f, 0.0f, 2.0f);

            ImGui::Separator();
            ImGui::Text("Halo");
            ImGui::DragFloat("Halo Intensity", &sunFlareSettings.haloIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Halo Radius", &sunFlareSettings.haloRadius, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Halo Width", &sunFlareSettings.haloWidth, 0.01f, 0.0f, 0.5f);
            ImGui::DragFloat("Halo Falloff", &sunFlareSettings.haloFalloff, 0.01f, 0.0f, 1.0f);

            ImGui::Separator();
            ImGui::Text("Ghosts");
            int count = static_cast<int>(sunFlareSettings.ghostCount);
            if (ImGui::SliderInt("Ghost Count", &count, 0, 10)) {
                sunFlareSettings.ghostCount = static_cast<Uint32>(count);
            }
            ImGui::DragFloat("Ghost Spacing", &sunFlareSettings.ghostSpacing, 0.01f, -1.0f, 1.0f);
            ImGui::DragFloat("Ghost Intensity", &sunFlareSettings.ghostIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Ghost Size", &sunFlareSettings.ghostSize, 0.01f, 0.0f, 0.5f);
            ImGui::DragFloat("Ghost Chromatic", &sunFlareSettings.ghostChromaticOffset, 0.001f, 0.0f, 0.05f);
            ImGui::DragFloat("Ghost Falloff", &sunFlareSettings.ghostFalloff, 0.1f, 0.1f, 10.0f);

            ImGui::Separator();
            ImGui::Text("Streak");
            ImGui::DragFloat("Streak Intensity", &sunFlareSettings.streakIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Streak Length", &sunFlareSettings.streakLength, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Streak Falloff", &sunFlareSettings.streakFalloff, 0.1f, 0.1f, 100.0f);

            ImGui::Separator();
            ImGui::Text("Starburst");
            ImGui::DragFloat("Starburst Intensity", &sunFlareSettings.starburstIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Starburst Size", &sunFlareSettings.starburstSize, 0.01f, 0.0f, 2.0f);
            int points = static_cast<int>(sunFlareSettings.starburstPoints);
            if (ImGui::SliderInt("Starburst Points", &points, 0, 16)) {
                sunFlareSettings.starburstPoints = static_cast<Uint32>(points);
            }
            ImGui::DragFloat("Starburst Rotation", &sunFlareSettings.starburstRotation, 0.01f, -3.14f, 3.14f);

            ImGui::Separator();
            ImGui::Text("Dirt");
            ImGui::DragFloat("Dirt Intensity", &sunFlareSettings.dirtIntensity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Dirt Scale", &sunFlareSettings.dirtScale, 0.1f, 0.1f, 20.0f);

            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("GPU Pass Timings")) {
        if (!gpuTimingSupported) {
            ImGui::TextDisabled("Not supported on this device");
        } else {
            ImGui::Checkbox("Enable##gpu_timing", &gpuTimingEnabled);
            if (gpuTimingEnabled) {
                std::lock_guard<std::mutex> lock(gpuTimingMutex);
                double totalMs = 0.0;
                double maxMs = 0.001;
                for (auto& t : gpuPassTimings) {
                    totalMs += t.gpuTimeMs;
                    if (t.gpuTimeMs > maxMs) maxMs = t.gpuTimeMs;
                }
                ImGui::Text("Total GPU: %.3f ms", totalMs);
                ImGui::Separator();
                if (ImGui::BeginTable(
                        "##gpu_pass_timings", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit
                    )) {
                    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("~GB/s", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    for (auto& t : gpuPassTimings) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(t.name.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%.3f", t.gpuTimeMs);
                        ImGui::TableSetColumnIndex(2);
                        // Effective bandwidth = estimated minimum traffic / measured time.
                        // Compare against the device's peak; a pass near peak is bandwidth-bound.
                        // Hide when the measured time is too small to divide meaningfully or
                        // the result exceeds any plausible device bandwidth (bad timestamp).
                        double gbps = (t.estimatedBytes > 0 && t.gpuTimeMs > 0.005)
                            ? static_cast<double>(t.estimatedBytes) / (t.gpuTimeMs * 1e6) : 0.0;
                        if (gbps > 0.0 && gbps < 2000.0) {
                            ImGui::Text("%.0f", gbps);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("~%.1f MB attachment/reported traffic", t.estimatedBytes / 1e6);
                            }
                        } else {
                            ImGui::TextDisabled("-");
                        }
                        ImGui::TableSetColumnIndex(3);
                        ImGui::ProgressBar(
                            static_cast<float>(t.gpuTimeMs / maxMs), ImVec2(-1.0f, 0.0f), ""
                        );
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    if (m_engineWindowCallback)
        m_engineWindowCallback();

    ImGui::End();

    if (m_imGuiCallback) {
        m_imGuiCallback();
    }
    } // if (m_imGuiVisible)

    // ==========================================================================
    // Execute all render passes
    // ==========================================================================
    graph.execute(
        currentCommandBuffer,
        (gpuTimingEnabled && gpuTimerSampleBuffer) ? gpuTimerSampleBuffer.get() : nullptr
    );

    // ==========================================================================
    // Present and cleanup
    // ==========================================================================

    // Process pending screenshots
    if (!m_pendingScreenshots.empty()) {
        MTL::Texture* texture = surface->texture();
        uint32_t width = static_cast<uint32_t>(texture->width());
        uint32_t height = static_cast<uint32_t>(texture->height());
        uint32_t bytesPerPixel = 4;
        uint32_t bytesPerRow = width * bytesPerPixel;
        uint32_t totalBytes = bytesPerRow * height;

        for (auto& callback : m_pendingScreenshots) {
            NS::SharedPtr<MTL::Buffer> cpuBuffer =
                NS::TransferPtr(device->newBuffer(totalBytes, MTL::ResourceStorageModeShared));
            MTL::BlitCommandEncoder* blitEncoder = cmd->blitCommandEncoder();
            blitEncoder->copyFromTexture(
                texture,
                0,
                0,
                MTL::Origin(0, 0, 0),
                MTL::Size(width, height, 1),
                cpuBuffer.get(),
                0,
                bytesPerRow,
                totalBytes
            );
            blitEncoder->endEncoding();

            cmd->addCompletedHandler([callback, cpuBuffer, width, height, totalBytes](MTL::CommandBuffer* buffer) {
                GpuImageData imageData;
                imageData.width = width;
                imageData.height = height;
                imageData.channelCount = 4;
                imageData.data.resize(totalBytes);
                memcpy(imageData.data.data(), cpuBuffer->contents(), totalBytes);
                callback(imageData);
            });
        }
        m_pendingScreenshots.clear();
    }

    // Resolve GPU pass timings asynchronously after the command buffer completes
    if (gpuTimingEnabled && gpuTimerSampleBuffer && !graph.passTimingInfo.empty()) {
        auto capturedInfo = graph.passTimingInfo;
        auto capturedBuf  = gpuTimerSampleBuffer; // retain via SharedPtr copy
        // Slots are laid out as: [frame-start, end0, end1, ..., endN-1].
        // beginIdx[K] == endIdx[K-1] by construction, so a plain end-begin delta is correct.
        NS::UInteger sampleCount = static_cast<NS::UInteger>(capturedInfo.back().endIdx + 1);
        cmd->addCompletedHandler([this, capturedInfo, capturedBuf, sampleCount](MTL::CommandBuffer*) {
            NS::Data* data = capturedBuf->resolveCounterRange(NS::Range::Make(0, sampleCount));
            if (!data) return;
            auto* timestamps = reinterpret_cast<const MTL::CounterResultTimestamp*>(data->mutableBytes());
            std::lock_guard<std::mutex> lock(gpuTimingMutex);
            gpuPassTimings.clear();
            gpuPassTimings.reserve(capturedInfo.size());
            for (size_t i = 0; i < capturedInfo.size(); i++) {
                auto& info = capturedInfo[i];
                uint64_t begin = timestamps[info.beginIdx].timestamp;
                uint64_t end   = timestamps[info.endIdx].timestamp;
                // 0 or MTLCounterErrorValue (~0) means the GPU never wrote the sample —
                // typically an encoder type that doesn't support stage-boundary sampling.
                // Without this check, an unwritten begin slot makes the delta look like a
                // raw absolute timestamp (nanosecond-scale garbage in the panel).
                bool beginValid = begin != 0 && begin != ~0ull;
                bool endValid   = end != 0 && end != ~0ull;
                double ms = (beginValid && endValid && end >= begin)
                    ? static_cast<double>(end - begin) / 1e6 : 0.0;
                // A slot that stopped being refreshed (encoder no longer writes its
                // sample) holds a stale absolute timestamp; the delta then grows by
                // one frame time per frame and reads as seconds. No real pass takes
                // over a second — treat it as a stale-slot artifact, not a timing.
                if (ms > 1000.0) {
                    static std::mutex staleMutex;
                    static std::set<std::string> staleReported;
                    std::lock_guard<std::mutex> staleLock(staleMutex);
                    if (staleReported.insert(info.name).second) {
                        fmt::print("[gpu-timing] pass '{}' delta {:.0f}ms — begin slot {} is stale "
                                   "(previous pass stopped refreshing its end sample)\n",
                                   info.name, ms, static_cast<uint64_t>(info.beginIdx));
                    }
                    ms = 0.0;
                }
                if (!endValid) {
                    // The pass that failed to write is THIS one (its end slot is empty);
                    // log once per pass name so the culprit encoder is identifiable.
                    static std::mutex logMutex;
                    static std::set<std::string> reported;
                    std::lock_guard<std::mutex> logLock(logMutex);
                    if (reported.insert(info.name).second) {
                        fmt::print("[gpu-timing] pass '{}' did not write its end timestamp (slot {})\n",
                                   info.name, static_cast<uint64_t>(info.endIdx));
                    }
                }
                gpuPassTimings.push_back({info.name, ms, info.estimatedBytes});
            }
        });
    }

    // NOTE: ImGui draw-data submission and presentDrawable()/commit() happen in
    // endFrame(), after the caller's ImGui::Render(). The frame counters are
    // advanced there too. Do not present or advance counters here.
}


auto Renderer_Metal::createPipeline(const std::string& filename, bool isHDR, bool isColorOnly, Uint32 sampleCount)
    -> NS::SharedPtr<MTL::RenderPipelineState> {
    auto shaderSrc = readFile(filename);

    auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* error = nullptr;
    MTL::CompileOptions* options = nullptr;
    MTL::Library* library = device->newLibrary(code, options, &error);
    if (!library) {
        throw std::runtime_error(
            fmt::format("Could not compile shader! Error: {}\n", error->localizedDescription()->utf8String())
        );
    }
    // fmt::print("Shader compiled successfully. Shader: {}\n", code->cString(NS::StringEncoding::UTF8StringEncoding));

    auto vertexFuncName = NS::String::string("vertexMain", NS::StringEncoding::UTF8StringEncoding);
    auto vertexMain = library->newFunction(vertexFuncName);

    auto fragmentFuncName = NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding);
    auto fragmentMain = library->newFunction(fragmentFuncName);

    auto pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    pipelineDesc->setVertexFunction(vertexMain);
    pipelineDesc->setFragmentFunction(fragmentMain);

    // auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

    // auto layout = vertexDesc->layouts()->object(0);
    // layout->setStride(sizeof(VertexData));
    // layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
    // layout->setStepRate(1);

    // auto attributes = vertexDesc->attributes();

    // auto posAttr = attributes->object(0);
    // posAttr->setFormat(MTL::VertexFormatFloat3);
    // posAttr->setOffset(offsetof(VertexData, position));
    // posAttr->setBufferIndex(2);

    // auto uvAttr = attributes->object(1);
    // uvAttr->setFormat(MTL::VertexFormatFloat2);
    // uvAttr->setOffset(offsetof(VertexData, uv));
    // uvAttr->setBufferIndex(2);

    // auto normalAttr = attributes->object(2);
    // normalAttr->setFormat(MTL::VertexFormatFloat3);
    // normalAttr->setOffset(offsetof(VertexData, normal));
    // normalAttr->setBufferIndex(2);

    // auto tangentAttr = attributes->object(3);
    // tangentAttr->setFormat(MTL::VertexFormatFloat4);
    // tangentAttr->setOffset(offsetof(VertexData, tangent));
    // tangentAttr->setBufferIndex(2);

    // pipelineDesc->setVertexDescriptor(vertexDesc);

    auto colorAttachment = pipelineDesc->colorAttachments()->object(0);
    if (isHDR) {
        colorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float);// HDR format
    } else {
        colorAttachment->setPixelFormat(swapchain->pixelFormat());
    }
    // TODO: optinal blending for particles
    // colorAttachment->setBlendingEnabled(true);
    // colorAttachment->setAlphaBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setRgbBlendOperation(MTL::BlendOperation::BlendOperationAdd);
    // colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
    // colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    // colorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    if (!isColorOnly) {
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    } else {
        pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
    }
    pipelineDesc->setSampleCount(static_cast<NS::UInteger>(sampleCount));

    auto pipeline = NS::TransferPtr(device->newRenderPipelineState(pipelineDesc, &error));
    if (!pipeline) {
        std::string errorMsg = error ? error->localizedDescription()->utf8String() : "Unknown error";
        throw std::runtime_error(fmt::format("Could not create pipeline! Error: {}\nShader: {}\n", errorMsg, filename));
    }

    code->release();
    library->release();
    vertexMain->release();
    fragmentMain->release();
    // vertexDesc->release();
    pipelineDesc->release();

    return pipeline;
}

auto Renderer_Metal::createComputePipeline(const std::string& filename) -> NS::SharedPtr<MTL::ComputePipelineState> {
    return createComputePipeline(filename, "computeMain");
}

auto Renderer_Metal::createComputePipeline(const std::string& filename, const std::string& functionName) -> NS::SharedPtr<MTL::ComputePipelineState> {
    auto shaderSrc = readFile(filename);

    auto code = NS::String::string(shaderSrc.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* error = nullptr;
    MTL::Library* library = device->newLibrary(code, nullptr, &error);
    if (!library) {
        throw std::runtime_error(
            fmt::format("Could not compile shader! Error: {}\n", error->localizedDescription()->utf8String())
        );
    }

    auto computeFuncName = NS::String::string(functionName.c_str(), NS::StringEncoding::UTF8StringEncoding);
    auto computeFunc = library->newFunction(computeFuncName);
    if (!computeFunc) {
        throw std::runtime_error(
            fmt::format("Could not find compute function '{}' in shader '{}'\n", functionName, filename)
        );
    }

    auto pipeline = NS::TransferPtr(device->newComputePipelineState(computeFunc, &error));
    if (!pipeline) {
        throw std::runtime_error(
            fmt::format("Could not create compute pipeline for '{}': {}\n", functionName, error->localizedDescription()->utf8String())
        );
    }

    code->release();
    library->release();
    computeFunc->release();

    return pipeline;
}

auto Renderer_Metal::createTexture(const std::shared_ptr<Image>& img) -> TextureHandle {
    if (img) {
        MTL::PixelFormat pixelFormat = MTL::PixelFormat::PixelFormatRGBA8Unorm;
        switch (img->channelCount) {
        case 1:
            pixelFormat = MTL::PixelFormat::PixelFormatR8Unorm;
            break;
        case 2:
            pixelFormat = MTL::PixelFormat::PixelFormatRG8Unorm;
            break;
        case 3:
        case 4:
            pixelFormat = MTL::PixelFormat::PixelFormatRGBA8Unorm;
            break;
        default:
            throw std::runtime_error(fmt::format(
                "Unknown texture format at {} (channelCount={}, width={}, height={}, byteArraySize={})\n",
                img->uri,
                img->channelCount,
                img->width,
                img->height,
                img->byteArray.size()
            ));
            break;
        }
        int numLevels = calculateMipmapLevelCount(img->width, img->height);

        auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        textureDesc->setPixelFormat(pixelFormat);
        textureDesc->setTextureType(MTL::TextureType::TextureType2D);
        textureDesc->setWidth(NS::UInteger(img->width));
        textureDesc->setHeight(NS::UInteger(img->height));
        textureDesc->setMipmapLevelCount(numLevels);
        textureDesc->setSampleCount(1);
        textureDesc->setStorageMode(MTL::StorageMode::StorageModeManaged);
        textureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

        auto texture = NS::TransferPtr(device->newTexture(textureDesc.get()));
        if (img->channelCount == 3) {
            // Convert RGB to RGBA by adding alpha channel
            std::vector<Uint8> rgbaData;
            rgbaData.reserve(img->width * img->height * 4);
            for (size_t i = 0; i < img->byteArray.size(); i += 3) {
                rgbaData.push_back(img->byteArray[i]);// R
                rgbaData.push_back(img->byteArray[i + 1]);// G
                rgbaData.push_back(img->byteArray[i + 2]);// B
                rgbaData.push_back(255);// A (opaque)
            }
            texture->replaceRegion(
                MTL::Region(0, 0, 0, img->width, img->height, 1), 0, rgbaData.data(), img->width * 4
            );
        } else {
            size_t bytesPerPixel = img->channelCount;
            texture->replaceRegion(
                MTL::Region(0, 0, 0, img->width, img->height, 1), 0, img->byteArray.data(), img->width * bytesPerPixel
            );
        }

        if (numLevels > 1) {
            auto cmdBlit = NS::TransferPtr(queue->commandBuffer());
            auto enc = NS::TransferPtr(cmdBlit->blitCommandEncoder());
            enc->generateMipmaps(texture.get());
            enc->endEncoding();
            cmdBlit->commit();
        }

        textures[nextTextureID] = texture;

        return TextureHandle{ nextTextureID++ };
    } else {
        throw std::runtime_error(fmt::format("Failed to create texture at {}!\n", img->uri));
    }
}

void Renderer_Metal::updateTexture(TextureHandle handle, const std::shared_ptr<Image>& img) {
    if (!img) {
        return;
    }
    auto it = textures.find(handle.id);
    if (it == textures.end() || !it->second) {
        fmt::print(stderr, "[Metal] updateTexture: invalid texture handle {}\n", handle.id);
        return;
    }
    MTL::Texture* texture = it->second.get();

    if (img->channelCount == 3) {
        // Convert RGB to RGBA by adding an opaque alpha channel.
        std::vector<Uint8> rgbaData;
        rgbaData.reserve(static_cast<size_t>(img->width) * img->height * 4);
        for (size_t i = 0; i + 2 < img->byteArray.size(); i += 3) {
            rgbaData.push_back(img->byteArray[i]);
            rgbaData.push_back(img->byteArray[i + 1]);
            rgbaData.push_back(img->byteArray[i + 2]);
            rgbaData.push_back(255);
        }
        texture->replaceRegion(
            MTL::Region(0, 0, 0, img->width, img->height, 1), 0, rgbaData.data(), img->width * 4
        );
    } else {
        size_t bytesPerPixel = img->channelCount;
        texture->replaceRegion(
            MTL::Region(0, 0, 0, img->width, img->height, 1), 0, img->byteArray.data(), img->width * bytesPerPixel
        );
    }

    if (texture->mipmapLevelCount() > 1) {
        auto cmdBlit = NS::TransferPtr(queue->commandBuffer());
        auto enc = NS::TransferPtr(cmdBlit->blitCommandEncoder());
        enc->generateMipmaps(texture);
        enc->endEncoding();
        cmdBlit->commit();
    }
}

// ===== Render-to-Texture Implementation =====

RenderTextureHandle Renderer_Metal::createRenderTexture(const RenderTextureDesc& desc) {
    RenderTextureData rtData;
    rtData.width = desc.width;
    rtData.height = desc.height;
    rtData.hdr = desc.isHDR;
    rtData.sampleCount = desc.sampleCount;

    // Create color texture
    auto colorDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    colorDesc->setTextureType(MTL::TextureType2D);
    colorDesc->setPixelFormat(desc.isHDR ? MTL::PixelFormatRGBA16Float : MTL::PixelFormatRGBA8Unorm);
    colorDesc->setWidth(desc.width);
    colorDesc->setHeight(desc.height);
    colorDesc->setMipmapLevelCount(1);
    colorDesc->setSampleCount(desc.sampleCount);
    colorDesc->setStorageMode(MTL::StorageModePrivate);
    colorDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);

    rtData.colorTexture = NS::TransferPtr(device->newTexture(colorDesc.get()));
    if (!rtData.colorTexture) {
        return RenderTextureHandle{};
    }

    // Create depth texture if requested
    if (desc.hasDepth) {
        auto depthDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        depthDesc->setTextureType(MTL::TextureType2D);
        depthDesc->setPixelFormat(MTL::PixelFormatDepth32Float);
        depthDesc->setWidth(desc.width);
        depthDesc->setHeight(desc.height);
        depthDesc->setMipmapLevelCount(1);
        depthDesc->setSampleCount(desc.sampleCount);
        depthDesc->setStorageMode(MTL::StorageModePrivate);
        depthDesc->setUsage(MTL::TextureUsageRenderTarget);

        rtData.depthTexture = NS::TransferPtr(device->newTexture(depthDesc.get()));
    }

    // Create temp texture for ping-pong post-processing (same format as color)
    auto tempDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    tempDesc->setTextureType(MTL::TextureType2D);
    tempDesc->setPixelFormat(desc.isHDR ? MTL::PixelFormatRGBA16Float : MTL::PixelFormatRGBA8Unorm);
    tempDesc->setWidth(desc.width);
    tempDesc->setHeight(desc.height);
    tempDesc->setMipmapLevelCount(1);
    tempDesc->setSampleCount(1);
    tempDesc->setStorageMode(MTL::StorageModePrivate);
    tempDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    rtData.tempTexture = NS::TransferPtr(device->newTexture(tempDesc.get()));

    // Store the color texture as a regular texture handle for sampling
    textures[nextTextureID] = rtData.colorTexture;
    rtData.textureHandle = TextureHandle{ nextTextureID++ };

    // Store render texture data
    Uint32 rtID = nextRenderTextureID++;
    renderTextures[rtID] = std::move(rtData);

    return RenderTextureHandle{ rtID };
}

void Renderer_Metal::destroyRenderTexture(RenderTextureHandle handle) {
    auto it = renderTextures.find(handle.id);
    if (it != renderTextures.end()) {
        // Remove from regular textures as well
        textures.erase(it->second.textureHandle.id);
        renderTextures.erase(it);
    }
}

TextureHandle Renderer_Metal::getRenderTextureAsTexture(RenderTextureHandle handle) {
    auto it = renderTextures.find(handle.id);
    if (it != renderTextures.end()) {
        return it->second.textureHandle;
    }
    return TextureHandle{};
}

glm::uvec2 Renderer_Metal::getRenderTextureSize(RenderTextureHandle handle) {
    auto it = renderTextures.find(handle.id);
    if (it != renderTextures.end()) {
        return glm::uvec2(it->second.width, it->second.height);
    }
    return glm::uvec2(0);
}

void Renderer_Metal::renderToTexture(
    RenderTextureHandle target, std::shared_ptr<RenderScene> scene, Camera& camera, const glm::vec4& clearColor
) {
    auto it = renderTextures.find(target.id);
    if (it == renderTextures.end() || !scene) {
        return;
    }

    const RenderTextureData& rtData = it->second;

    // Create a command buffer for this render
    auto cmdBuffer = NS::TransferPtr(queue->commandBuffer());

    // Create render pass descriptor
    auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());

    // Color attachment
    auto colorAttachment = passDesc->colorAttachments()->object(0);
    colorAttachment->setTexture(rtData.colorTexture.get());
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setClearColor(MTL::ClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a));

    // Depth attachment
    if (rtData.depthTexture) {
        auto depthAttachment = passDesc->depthAttachment();
        depthAttachment->setTexture(rtData.depthTexture.get());
        depthAttachment->setLoadAction(MTL::LoadActionClear);
        depthAttachment->setStoreAction(MTL::StoreActionDontCare);
        depthAttachment->setClearDepth(1.0);
    }

    // Get render encoder
    auto encoder = cmdBuffer->renderCommandEncoder(passDesc.get());

    // Set viewport
    MTL::Viewport viewport;
    viewport.originX = 0.0;
    viewport.originY = 0.0;
    viewport.width = static_cast<double>(rtData.width);
    viewport.height = static_cast<double>(rtData.height);
    viewport.znear = 0.0;
    viewport.zfar = 1.0;
    encoder->setViewport(viewport);

    // Update camera data for this render texture's aspect ratio
    float aspect = static_cast<float>(rtData.width) / static_cast<float>(rtData.height);
    camera.updateAspectRatio(aspect);

    // Update camera data buffer
    ::CameraData cameraData;
    cameraData.proj = camera.getProjMatrix();
    cameraData.view = camera.getViewMatrix();
    cameraData.invProj = glm::inverse(cameraData.proj);
    cameraData.invView = glm::inverse(cameraData.view);
    cameraData.near = camera.near();
    cameraData.far = camera.far();
    cameraData.position = camera.getEye();
    auto frustumPlanes = camera.getFrustumPlanes();
    for (int i = 0; i < 6; i++) {
        cameraData.frustumPlanes[i] = frustumPlanes[i];
    }

    // Create temporary camera buffer for this render
    auto tempCameraBuffer = NS::TransferPtr(device->newBuffer(sizeof(::CameraData), MTL::ResourceStorageModeShared));
    memcpy(tempCameraBuffer->contents(), &cameraData, sizeof(::CameraData));

    // Set pipeline state
    encoder->setRenderPipelineState(drawPipeline.get());
    encoder->setDepthStencilState(depthStencilState.get());
    encoder->setCullMode(MTL::CullModeBack);
    encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);

    glm::vec2 screenSize = glm::vec2(rtData.width, rtData.height);
    glm::uvec3 gridSize = glm::uvec3(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
    float time = static_cast<float>(SDL_GetTicks()) / 1000.0f;

    // Bind vertex buffers (matching MainRenderPass layout)
    encoder->setVertexBuffer(tempCameraBuffer.get(), 0, 0);
    encoder->setVertexBuffer(materialDataBuffer.get(), 0, 1);
    encoder->setVertexBuffer(instanceDataBuffers[currentFrameInFlight].get(), 0, 2);
    encoder->setVertexBuffer(getBuffer(scene->vertexBuffer).get(), 0, 3);

    // Bind fragment buffers
    encoder->setFragmentBuffer(directionalLightBuffer.get(), 0, 0);
    encoder->setFragmentBuffer(pointLightBuffer.get(), 0, 1);
    encoder->setFragmentBuffer(clusterBuffers[currentFrameInFlight].get(), 0, 2);
    encoder->setFragmentBuffer(tempCameraBuffer.get(), 0, 3);
    encoder->setFragmentBytes(&screenSize, sizeof(glm::vec2), 4);
    encoder->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
    encoder->setFragmentBytes(&time, sizeof(float), 6);
    encoder->setFragmentBuffer(rectLightBuffer.get(), 0, 7);
    uint32_t rtRectLightCount = static_cast<uint32_t>(scene->rectLights.size());
    encoder->setFragmentBytes(&rtRectLightCount, sizeof(uint32_t), 8);
    encoder->setFragmentBuffer(pssmDataBuffers[currentFrameInFlight].get(), 0, 9);
    // Main-view AO is the wrong view for a render texture, but the shader
    // requires the binding; misaligned ambient attenuation is acceptable here
    encoder->setFragmentTexture(aoEnabled ? aoRT.get() : batch2DWhiteTexture.get(), 6);
    encoder->setFragmentTexture(
        rectLightVideoTexture ? rectLightVideoTexture.get() : getTexture(defaultAlbedoTexture).get(), 11
    );

    // Render using instance batches (same as MainRenderPass)
    for (const auto& [material, meshes] : instanceBatches) {
        // Bind material textures
        encoder->setFragmentTexture(
            getTexture(material->albedoMap ? material->albedoMap->texture : defaultAlbedoTexture).get(), 0
        );
        encoder->setFragmentTexture(
            getTexture(material->normalMap ? material->normalMap->texture : defaultNormalTexture).get(), 1
        );
        encoder->setFragmentTexture(
            getTexture(material->metallicMap ? material->metallicMap->texture : defaultORMTexture).get(), 2
        );
        encoder->setFragmentTexture(
            getTexture(material->roughnessMap ? material->roughnessMap->texture : defaultORMTexture).get(), 3
        );
        encoder->setFragmentTexture(
            getTexture(material->occlusionMap ? material->occlusionMap->texture : defaultORMTexture).get(), 4
        );
        encoder->setFragmentTexture(
            getTexture(material->emissiveMap ? material->emissiveMap->texture : defaultEmissiveTexture).get(), 5
        );
        encoder->setFragmentTexture(shadowRT.get(), 7);
        encoder->setFragmentTexture(sscsEnabled ? sscsRT.get() : batch2DWhiteTexture.get(), 15);

        // IBL textures
        encoder->setFragmentTexture(irradianceMap.get(), 8);
        encoder->setFragmentTexture(prefilterMap.get(), 9);
        encoder->setFragmentTexture(brdfLUT.get(), 10);

        // PSSM shadow maps (data buffer bound once before this loop, at buffer 9)
        encoder->setFragmentTexture(pssmShadowMaps.get(), 12);
        encoder->setFragmentTexture(pointShadowDenoisedRT.get(), 13);

        for (const auto& draw : meshes) {
            // Frustum culling with render texture camera
            if (!camera.isVisible(instances[draw.instanceIndex].boundingSphere)) {
                continue;
            }

            encoder->setVertexBytes(&draw.instanceIndex, sizeof(Uint32), 4);
            encoder->drawIndexedPrimitives(
                MTL::PrimitiveTypeTriangle,
                draw.mesh->indexCount,
                MTL::IndexTypeUInt32,
                getBuffer(scene->indexBuffer).get(),
                draw.mesh->indexOffset * sizeof(Uint32)
            );
        }
    }

    encoder->endEncoding();
    cmdBuffer->commit();
    cmdBuffer->waitUntilCompleted();
}

Uint64 Renderer_Metal::registerRenderTextureForUI(RenderTextureHandle handle) {
    auto it = renderTextures.find(handle.id);
    if (it == renderTextures.end()) {
        return 0;
    }

    // Get the RmlUI renderer
    if (!m_uiRenderer) {
        return 0;
    }

    auto* uiRenderer = static_cast<Vapor::RmlRendererMetal*>(m_uiRenderer);
    return uiRenderer->registerExternalTexture(it->second.colorTexture.get(), it->second.width, it->second.height);
}

// ===== Render Texture Post-Processing Implementation =====

void Renderer_Metal::applyBloom(RenderTextureHandle target, float threshold, float strength) {
    auto it = renderTextures.find(target.id);
    if (it == renderTextures.end()) {
        return;
    }

    RenderTextureData& rtData = it->second;
    auto cmdBuffer = NS::TransferPtr(queue->commandBuffer());

    // Step 1: Extract bright pixels to temp texture
    {
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(rtData.tempTexture.get());
        colorAttachment->setLoadAction(MTL::LoadActionClear);
        colorAttachment->setStoreAction(MTL::StoreActionStore);
        colorAttachment->setClearColor(MTL::ClearColor(0, 0, 0, 1));

        auto encoder = cmdBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(bloomBrightnessPipeline.get());

        MTL::Viewport viewport = { 0, 0, (double)rtData.width, (double)rtData.height, 0, 1 };
        encoder->setViewport(viewport);

        // Bind source texture and uniforms
        encoder->setFragmentTexture(rtData.colorTexture.get(), 0);
        encoder->setFragmentBytes(&threshold, sizeof(float), 0);

        // Draw fullscreen quad
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        encoder->endEncoding();
    }

    // Step 2: Composite bloom back to color texture
    {
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(rtData.colorTexture.get());
        colorAttachment->setLoadAction(MTL::LoadActionLoad);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        auto encoder = cmdBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(bloomCompositePipeline.get());

        MTL::Viewport viewport = { 0, 0, (double)rtData.width, (double)rtData.height, 0, 1 };
        encoder->setViewport(viewport);

        encoder->setFragmentTexture(rtData.tempTexture.get(), 0);
        encoder->setFragmentBytes(&strength, sizeof(float), 0);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        encoder->endEncoding();
    }

    cmdBuffer->commit();
    cmdBuffer->waitUntilCompleted();
}

void Renderer_Metal::applyToneMapping(RenderTextureHandle target, float exposure) {
    auto it = renderTextures.find(target.id);
    if (it == renderTextures.end()) {
        return;
    }

    RenderTextureData& rtData = it->second;
    auto cmdBuffer = NS::TransferPtr(queue->commandBuffer());

    // Copy color to temp first
    {
        auto blitEncoder = cmdBuffer->blitCommandEncoder();
        blitEncoder->copyFromTexture(
            rtData.colorTexture.get(),
            0,
            0,
            MTL::Origin(0, 0, 0),
            MTL::Size(rtData.width, rtData.height, 1),
            rtData.tempTexture.get(),
            0,
            0,
            MTL::Origin(0, 0, 0)
        );
        blitEncoder->endEncoding();
    }

    // Apply tone mapping from temp to color
    {
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(rtData.colorTexture.get());
        colorAttachment->setLoadAction(MTL::LoadActionDontCare);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        auto encoder = cmdBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(postProcessPipeline.get());

        MTL::Viewport viewport = { 0, 0, (double)rtData.width, (double)rtData.height, 0, 1 };
        encoder->setViewport(viewport);

        struct GPUPostProcessParams {
            float chromaticAberrationStrength;
            float chromaticAberrationFalloff;
            float vignetteStrength;
            float vignetteRadius;
            float vignetteSoftness;
            float saturation;
            float contrast;
            float brightness;
            float temperature;
            float tint;
            float exposure;
        } params = {
            0.0f, // chromaticAberrationStrength
            0.0f, // chromaticAberrationFalloff
            0.0f, // vignetteStrength
            0.0f, // vignetteRadius
            0.0f, // vignetteSoftness
            1.0f, // saturation
            1.0f, // contrast
            1.0f, // brightness
            0.0f, // temperature
            0.0f, // tint
            exposure // exposure
        };

        encoder->setFragmentTexture(rtData.tempTexture.get(), 0);
        encoder->setFragmentBytes(&params, sizeof(GPUPostProcessParams), 0);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        encoder->endEncoding();
    }

    cmdBuffer->commit();
    cmdBuffer->waitUntilCompleted();
}

void Renderer_Metal::applyVignette(RenderTextureHandle target, float strength, float radius) {
    auto it = renderTextures.find(target.id);
    if (it == renderTextures.end()) {
        return;
    }

    RenderTextureData& rtData = it->second;
    auto cmdBuffer = NS::TransferPtr(queue->commandBuffer());

    // Copy color to temp
    {
        auto blitEncoder = cmdBuffer->blitCommandEncoder();
        blitEncoder->copyFromTexture(
            rtData.colorTexture.get(),
            0,
            0,
            MTL::Origin(0, 0, 0),
            MTL::Size(rtData.width, rtData.height, 1),
            rtData.tempTexture.get(),
            0,
            0,
            MTL::Origin(0, 0, 0)
        );
        blitEncoder->endEncoding();
    }

    // Apply vignette from temp to color
    {
        auto passDesc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
        auto colorAttachment = passDesc->colorAttachments()->object(0);
        colorAttachment->setTexture(rtData.colorTexture.get());
        colorAttachment->setLoadAction(MTL::LoadActionDontCare);
        colorAttachment->setStoreAction(MTL::StoreActionStore);

        auto encoder = cmdBuffer->renderCommandEncoder(passDesc.get());
        encoder->setRenderPipelineState(postProcessPipeline.get());

        MTL::Viewport viewport = { 0, 0, (double)rtData.width, (double)rtData.height, 0, 1 };
        encoder->setViewport(viewport);

        struct GPUPostProcessParams {
            float chromaticAberrationStrength;
            float chromaticAberrationFalloff;
            float vignetteStrength;
            float vignetteRadius;
            float vignetteSoftness;
            float saturation;
            float contrast;
            float brightness;
            float temperature;
            float tint;
            float exposure;
        } params = {
            0.0f, // chromaticAberrationStrength
            0.0f, // chromaticAberrationFalloff
            strength, // vignetteStrength
            radius, // vignetteRadius
            0.15f, // vignetteSoftness
            1.0f, // saturation
            1.0f, // contrast
            1.0f, // brightness
            0.0f, // temperature
            0.0f, // tint
            1.0f // exposure
        };

        encoder->setFragmentTexture(rtData.tempTexture.get(), 0);
        encoder->setFragmentBytes(&params, sizeof(GPUPostProcessParams), 0);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        encoder->endEncoding();
    }

    cmdBuffer->commit();
    cmdBuffer->waitUntilCompleted();
}

// ===== Font Rendering Implementation =====

auto Renderer_Metal::loadFont(const std::string& path, float baseSize) -> FontHandle {
    // Load font using FontManager
    FontHandle fontHandle = m_fontManager.loadFont(path, baseSize);
    if (!fontHandle.isValid()) {
        return fontHandle;
    }

    // Get atlas data and create Metal texture
    const FontManager::AtlasData* atlasData = m_fontManager.getAtlasData(fontHandle);
    if (!atlasData) {
        m_fontManager.unloadFont(fontHandle);
        return FontHandle{};
    }

    // Create texture from atlas data
    auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    textureDesc->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA8Unorm);
    textureDesc->setTextureType(MTL::TextureType::TextureType2D);
    textureDesc->setWidth(NS::UInteger(atlasData->width));
    textureDesc->setHeight(NS::UInteger(atlasData->height));
    textureDesc->setMipmapLevelCount(1);
    textureDesc->setSampleCount(1);
    textureDesc->setStorageMode(MTL::StorageMode::StorageModeManaged);
    textureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);

    auto texture = NS::TransferPtr(device->newTexture(textureDesc.get()));
    texture->replaceRegion(
        MTL::Region(0, 0, 0, atlasData->width, atlasData->height, 1),
        0,
        atlasData->rgbaData.data(),
        atlasData->width * 4
    );

    // Store texture and create handle
    textures[nextTextureID] = texture;
    TextureHandle texHandle{ nextTextureID++ };

    // Associate texture handle with font
    m_fontManager.setFontTextureHandle(fontHandle, texHandle);

    return fontHandle;
}

void Renderer_Metal::unloadFont(FontHandle handle) {
    if (!handle.isValid()) return;

    // Get texture handle before unloading
    TextureHandle texHandle = m_fontManager.getFontTexture(handle);
    if (texHandle.id != UINT32_MAX) {
        textures.erase(texHandle.id);
    }

    m_fontManager.unloadFont(handle);
}

void Renderer_Metal::drawText2D(
    FontHandle fontHandle, const std::string& text, const glm::vec2& position, float scale, const glm::vec4& color
) {
    Font* font = m_fontManager.getFont(fontHandle);
    if (!font || font->textureHandle.id == UINT32_MAX) return;

    float cursorX = position.x;
    float cursorY = position.y;

    for (char c : text) {
        const Glyph* glyph = m_fontManager.getGlyph(fontHandle, static_cast<int>(c));
        if (!glyph) continue;

        float drawX = cursorX + glyph->xOffset * scale;
        float drawY = cursorY + glyph->yOffset * scale + font->ascent * scale;
        float drawW = glyph->width * scale;
        float drawH = glyph->height * scale;

        if (drawW > 0 && drawH > 0) {
            // Adjust for centered quad rendering (batchQuadPositions uses -0.5 to 0.5)
            float finalX = drawX + drawW * 0.5f;
            float finalY = drawY + drawH * 0.5f;

            // Create UV coordinates for this glyph
            glm::vec2 uvs[4] = {
                { glyph->u0, glyph->v0 },// top-left
                { glyph->u1, glyph->v0 },// top-right
                { glyph->u1, glyph->v1 },// bottom-right
                { glyph->u0, glyph->v1 }// bottom-left
            };

            glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(finalX, finalY, 0.0f));
            transform = glm::scale(transform, glm::vec3(drawW, drawH, 1.0f));
            drawQuad2D(transform, font->textureHandle, uvs, color);
        }

        cursorX += glyph->advance * scale;
    }
}

void Renderer_Metal::drawText3D(
    FontHandle fontHandle, const std::string& text, const glm::vec3& worldPosition, float scale, const glm::vec4& color
) {
    Font* font = m_fontManager.getFont(fontHandle);
    if (!font || font->textureHandle.id == UINT32_MAX) return;

    // For 3D text, we draw at the world position
    // The text will be rendered as billboards facing the camera
    float cursorX = 0.0f;

    for (char c : text) {
        const Glyph* glyph = m_fontManager.getGlyph(fontHandle, static_cast<int>(c));
        if (!glyph) continue;

        float drawX = cursorX + glyph->xOffset * scale;
        float drawY = glyph->yOffset * scale + font->ascent * scale;
        float drawW = glyph->width * scale;
        float drawH = glyph->height * scale;

        if (drawW > 0 && drawH > 0) {
            // Adjust for centered quad rendering (batchQuadPositions uses -0.5 to 0.5)
            float finalX = drawX + drawW * 0.5f;
            float finalY = drawY + drawH * 0.5f;

            // Create UV coordinates for this glyph
            glm::vec2 uvs[4] = {
                { glyph->u0, glyph->v0 },// top-left
                { glyph->u1, glyph->v0 },// top-right
                { glyph->u1, glyph->v1 },// bottom-right
                { glyph->u0, glyph->v1 }// bottom-left
            };

            // Create transform in world space
            glm::mat4 transform = glm::translate(glm::mat4(1.0f), worldPosition);
            transform = glm::translate(transform, glm::vec3(finalX, finalY, 0.0f));
            transform = glm::scale(transform, glm::vec3(drawW, drawH, 1.0f));
            drawQuad3D(transform, font->textureHandle, uvs, color);
        }

        cursorX += glyph->advance * scale;
    }
}

auto Renderer_Metal::measureText(FontHandle fontHandle, const std::string& text, float scale) -> glm::vec2 {
    return m_fontManager.measureText(fontHandle, text, scale);
}

auto Renderer_Metal::getFontLineHeight(FontHandle fontHandle, float scale) -> float {
    Font* font = m_fontManager.getFont(fontHandle);
    return font ? font->lineHeight * scale : 0.0f;
}

auto Renderer_Metal::createVertexBuffer(const std::vector<VertexData>& vertices) -> BufferHandle {
    auto stagingBuffer =
        NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), vertices.data(), vertices.size() * sizeof(VertexData));

    auto buffer =
        NS::TransferPtr(device->newBuffer(vertices.size() * sizeof(VertexData), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, vertices.size() * sizeof(VertexData));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return BufferHandle{ nextBufferID++ };
}

auto Renderer_Metal::createIndexBuffer(const std::vector<Uint32>& indices) -> BufferHandle {
    auto stagingBuffer =
        NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModeShared));
    memcpy(stagingBuffer->contents(), indices.data(), indices.size() * sizeof(Uint32));

    auto buffer = NS::TransferPtr(device->newBuffer(indices.size() * sizeof(Uint32), MTL::ResourceStorageModePrivate));

    auto cmd = queue->commandBuffer();
    auto blitEncoder = cmd->blitCommandEncoder();
    blitEncoder->copyFromBuffer(stagingBuffer.get(), 0, buffer.get(), 0, indices.size() * sizeof(Uint32));
    blitEncoder->endEncoding();
    cmd->commit();

    buffers[nextBufferID] = buffer;

    return BufferHandle{ nextBufferID++ };
}

auto Renderer_Metal::getBuffer(BufferHandle handle) const -> NS::SharedPtr<MTL::Buffer> {
    if (handle.id == UINT32_MAX || buffers.find(handle.id) == buffers.end()) return nullptr;
    return buffers.at(handle.id);
}

auto Renderer_Metal::getTexture(TextureHandle handle) const -> NS::SharedPtr<MTL::Texture> {
    if (handle.id == UINT32_MAX || textures.find(handle.id) == textures.end()) return nullptr;
    return textures.at(handle.id);
}

auto Renderer_Metal::getPipeline(PipelineHandle handle) const -> NS::SharedPtr<MTL::RenderPipelineState> {
    if (handle.id == UINT32_MAX || pipelines.find(handle.id) == pipelines.end()) return nullptr;
    return pipelines.at(handle.id);
}

// ===== 2D/3D Batch Rendering Implementation =====

void Renderer_Metal::beginBatch2D() {
    if (batch2DActive) return;
    batch2DSubBatches.clear();
    batch2DVertices.clear();
    batch2DIndices.clear();
    batch2DTextureSlots[0] = batch2DWhiteTextureHandle;
    batch2DTextureSlotIndex = 1;
    batch2DActive = true;
}

void Renderer_Metal::splitBatch2D() {
    if (batch2DVertices.empty()) return;
    Batch2DSubBatch sub;
    sub.vertices        = std::move(batch2DVertices);
    sub.indices         = std::move(batch2DIndices);
    sub.textureSlots    = batch2DTextureSlots;
    sub.textureSlotCount = batch2DTextureSlotIndex;
    batch2DSubBatches.push_back(std::move(sub));
    // Reset in-progress batch (slot 0 stays white)
    batch2DVertices.clear();
    batch2DIndices.clear();
    batch2DTextureSlots[0] = batch2DWhiteTextureHandle;
    batch2DTextureSlotIndex = 1;
}

void Renderer_Metal::endBatch2D() {
    batch2DActive = false;
}

void Renderer_Metal::beginBatch3D() {
    if (batch3DActive) return;
    batch3DVertices.clear();
    batch3DIndices.clear();
    batch3DTextureSlots[0] = batch2DWhiteTextureHandle;
    batch3DTextureSlotIndex = 1;
    batch3DActive = true;
}

void Renderer_Metal::endBatch3D() {
    batch3DActive = false;
}

void Renderer_Metal::flush2D() {
    // Will be rendered by CanvasPass
    endBatch2D();
}

void Renderer_Metal::flush3D() {
    // Will be rendered by WorldCanvasPass
    endBatch3D();
}

// Helper to find or add a texture slot
static auto findOrAddTextureSlot(
    std::array<TextureHandle, 16>& slots, Uint32& slotIndex, TextureHandle texture, TextureHandle whiteTexture
) -> float {
    if (texture.id == UINT32_MAX || texture.id == whiteTexture.id) {
        return 0.0f;
    }

    for (Uint32 i = 1; i < slotIndex; i++) {
        if (slots[i].id == texture.id) {
            return static_cast<float>(i);
        }
    }

    if (slotIndex >= 16) {
        return -1.0f;// Slots full — caller must call splitBatch2D() and retry
    }

    auto texIndex = static_cast<float>(slotIndex);
    slots[slotIndex] = texture;
    slotIndex++;
    return texIndex;
}

void Renderer_Metal::drawQuad2D(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color) {
    drawQuad2D(glm::vec3(position, 0.0f), size, color);
}

void Renderer_Metal::drawQuad2D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, color);
}

void Renderer_Metal::drawQuad2D(
    const glm::vec2& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor
) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, texture, batchQuadTexCoords, tintColor);
}

void Renderer_Metal::drawQuad2D(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    drawQuad2D(transform, batch2DWhiteTextureHandle, batchQuadTexCoords, color, entityID);
}

void Renderer_Metal::drawQuad2D(
    const glm::mat4& transform,
    TextureHandle texture,
    const glm::vec2* texCoords,
    const glm::vec4& tintColor,
    int entityID
) {
    beginBatch2D();// Auto-start batch
    if (batch2DIndices.size() >= BatchMaxIndices) {
        return;// Batch full
    }

    float textureIndex =
        findOrAddTextureSlot(batch2DTextureSlots, batch2DTextureSlotIndex, texture, batch2DWhiteTextureHandle);
    if (textureIndex < 0.0f) {
        // Texture slots exhausted — flush current batch and start a new one
        splitBatch2D();
        textureIndex = findOrAddTextureSlot(
            batch2DTextureSlots, batch2DTextureSlotIndex, texture, batch2DWhiteTextureHandle
        );
    }
    auto vertexOffset = static_cast<Uint32>(batch2DVertices.size());

    const glm::vec2* uvs = texCoords ? texCoords : batchQuadTexCoords;

    // Add 4 vertices
    for (int i = 0; i < 4; i++) {
        Batch2DVertex vertex;
        vertex.position = transform * batchQuadPositions[i];
        vertex.color = tintColor;
        vertex.uv = uvs[i];
        vertex.texIndex = textureIndex;
        vertex.entityID = static_cast<float>(entityID);
        batch2DVertices.push_back(vertex);
    }

    // Add 6 indices (2 triangles)
    batch2DIndices.push_back(vertexOffset + 0);
    batch2DIndices.push_back(vertexOffset + 1);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 3);
    batch2DIndices.push_back(vertexOffset + 0);

    batch2DStats.quadCount++;
}

void Renderer_Metal::drawRotatedQuad2D(
    const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color
) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f))
                          * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, color);
}

void Renderer_Metal::drawRotatedQuad2D(
    const glm::vec2& position, const glm::vec2& size, float rotation, TextureHandle texture, const glm::vec4& tintColor
) {
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(position, 0.0f))
                          * glm::rotate(glm::mat4(1.0f), rotation, glm::vec3(0.0f, 0.0f, 1.0f))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad2D(transform, texture, batchQuadTexCoords, tintColor);
}

void Renderer_Metal::drawLine2D(const glm::vec2& p0, const glm::vec2& p1, const glm::vec4& color, float thickness) {
    beginBatch2D();// Auto-start batch

    glm::vec2 direction = p1 - p0;
    float length = glm::length(direction);
    if (length < 0.0001f) return;

    glm::vec2 normalized = direction / length;
    glm::vec2 perpendicular(-normalized.y, normalized.x);
    float halfThickness = thickness * 0.5f;

    // Four corners of the line quad
    glm::vec3 v0 = glm::vec3(p0 - perpendicular * halfThickness, 0.0f);
    glm::vec3 v1 = glm::vec3(p1 - perpendicular * halfThickness, 0.0f);
    glm::vec3 v2 = glm::vec3(p1 + perpendicular * halfThickness, 0.0f);
    glm::vec3 v3 = glm::vec3(p0 + perpendicular * halfThickness, 0.0f);

    if (batch2DIndices.size() >= BatchMaxIndices) return;

    glm::vec2 defaultUV(0.5f, 0.5f);
    auto vertexOffset = static_cast<Uint32>(batch2DVertices.size());

    Batch2DVertex vertex;
    vertex.color = color;
    vertex.uv = defaultUV;
    vertex.texIndex = 0.0f;
    vertex.entityID = -1.0f;

    vertex.position = v0;
    batch2DVertices.push_back(vertex);
    vertex.position = v1;
    batch2DVertices.push_back(vertex);
    vertex.position = v2;
    batch2DVertices.push_back(vertex);
    vertex.position = v3;
    batch2DVertices.push_back(vertex);

    batch2DIndices.push_back(vertexOffset + 0);
    batch2DIndices.push_back(vertexOffset + 1);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 3);
    batch2DIndices.push_back(vertexOffset + 0);

    batch2DStats.lineCount++;
}

// ===== 3D Batch Drawing (world space with depth) =====

void Renderer_Metal::drawQuad3D(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad3D(transform, color);
}

void Renderer_Metal::drawQuad3D(
    const glm::vec3& position, const glm::vec2& size, TextureHandle texture, const glm::vec4& tintColor
) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.0f));
    drawQuad3D(transform, texture, batchQuadTexCoords, tintColor);
}

void Renderer_Metal::drawQuad3D(const glm::mat4& transform, const glm::vec4& color, int entityID) {
    drawQuad3D(transform, batch2DWhiteTextureHandle, batchQuadTexCoords, color, entityID);
}

void Renderer_Metal::drawQuad3D(
    const glm::mat4& transform,
    TextureHandle texture,
    const glm::vec2* texCoords,
    const glm::vec4& tintColor,
    int entityID
) {
    beginBatch3D();// Auto-start batch
    if (batch3DIndices.size() >= BatchMaxIndices) return;

    float textureIndex =
        findOrAddTextureSlot(batch3DTextureSlots, batch3DTextureSlotIndex, texture, batch2DWhiteTextureHandle);
    auto vertexOffset = static_cast<Uint32>(batch3DVertices.size());

    const glm::vec2* uvs = texCoords ? texCoords : batchQuadTexCoords;

    for (int i = 0; i < 4; i++) {
        Batch2DVertex vertex;
        vertex.position = transform * batchQuadPositions[i];
        vertex.color = tintColor;
        vertex.uv = uvs[i];
        vertex.texIndex = textureIndex;
        vertex.entityID = static_cast<float>(entityID);
        batch3DVertices.push_back(vertex);
    }

    batch3DIndices.push_back(vertexOffset + 0);
    batch3DIndices.push_back(vertexOffset + 1);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 3);
    batch3DIndices.push_back(vertexOffset + 0);

    batch3DStats.quadCount++;
}

void Renderer_Metal::drawLine3D(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, float thickness) {
    beginBatch3D();// Auto-start batch

    glm::vec3 direction = p1 - p0;
    float length = glm::length(direction);
    if (length < 0.0001f) return;

    glm::vec3 normalized = direction / length;
    // For 3D lines, we need a perpendicular that works in 3D space
    glm::vec3 up = (std::abs(normalized.y) < 0.999f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::vec3 perpendicular = glm::normalize(glm::cross(normalized, up));
    float halfThickness = thickness * 0.5f;

    glm::vec3 v0 = p0 - perpendicular * halfThickness;
    glm::vec3 v1 = p1 - perpendicular * halfThickness;
    glm::vec3 v2 = p1 + perpendicular * halfThickness;
    glm::vec3 v3 = p0 + perpendicular * halfThickness;

    if (batch3DIndices.size() >= BatchMaxIndices) return;

    glm::vec2 defaultUV(0.5f, 0.5f);
    auto vertexOffset = static_cast<Uint32>(batch3DVertices.size());

    Batch2DVertex vertex;
    vertex.color = color;
    vertex.uv = defaultUV;
    vertex.texIndex = 0.0f;
    vertex.entityID = -1.0f;

    vertex.position = v0;
    batch3DVertices.push_back(vertex);
    vertex.position = v1;
    batch3DVertices.push_back(vertex);
    vertex.position = v2;
    batch3DVertices.push_back(vertex);
    vertex.position = v3;
    batch3DVertices.push_back(vertex);

    batch3DIndices.push_back(vertexOffset + 0);
    batch3DIndices.push_back(vertexOffset + 1);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 2);
    batch3DIndices.push_back(vertexOffset + 3);
    batch3DIndices.push_back(vertexOffset + 0);

    batch3DStats.lineCount++;
}

void Renderer_Metal::drawRect2D(
    const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, float thickness
) {
    glm::vec2 topLeft = position;
    glm::vec2 topRight = position + glm::vec2(size.x, 0.0f);
    glm::vec2 bottomRight = position + size;
    glm::vec2 bottomLeft = position + glm::vec2(0.0f, size.y);

    drawLine2D(topLeft, topRight, color, thickness);
    drawLine2D(topRight, bottomRight, color, thickness);
    drawLine2D(bottomRight, bottomLeft, color, thickness);
    drawLine2D(bottomLeft, topLeft, color, thickness);
}

void Renderer_Metal::drawCircle2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    float angleStep = 2.0f * glm::pi<float>() / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        float angle0 = angleStep * i;
        float angle1 = angleStep * (i + 1);

        glm::vec2 p0 = center + glm::vec2(std::cos(angle0) * radius, std::sin(angle0) * radius);
        glm::vec2 p1 = center + glm::vec2(std::cos(angle1) * radius, std::sin(angle1) * radius);

        drawLine2D(p0, p1, color, 1.0f);
    }
    batch2DStats.circleCount++;
}

void Renderer_Metal::drawCircleFilled2D(const glm::vec2& center, float radius, const glm::vec4& color, int segments) {
    float angleStep = 2.0f * glm::pi<float>() / static_cast<float>(segments);

    for (int i = 0; i < segments; ++i) {
        float angle0 = angleStep * i;
        float angle1 = angleStep * (i + 1);

        glm::vec2 p0 = center;
        glm::vec2 p1 = center + glm::vec2(std::cos(angle0) * radius, std::sin(angle0) * radius);
        glm::vec2 p2 = center + glm::vec2(std::cos(angle1) * radius, std::sin(angle1) * radius);

        drawTriangleFilled2D(p0, p1, p2, color);
    }
    batch2DStats.circleCount++;
}

void Renderer_Metal::drawTriangle2D(
    const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color
) {
    drawLine2D(p0, p1, color, 1.0f);
    drawLine2D(p1, p2, color, 1.0f);
    drawLine2D(p2, p0, color, 1.0f);
}

void Renderer_Metal::drawTriangleFilled2D(
    const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color
) {
    if (batch2DIndices.size() >= BatchMaxIndices) return;

    glm::vec2 defaultUV(0.5f, 0.5f);
    auto vertexOffset = static_cast<Uint32>(batch2DVertices.size());

    Batch2DVertex vertex;
    vertex.color = color;
    vertex.uv = defaultUV;
    vertex.texIndex = 0.0f;
    vertex.entityID = -1.0f;

    vertex.position = glm::vec3(p0, 0.0f);
    batch2DVertices.push_back(vertex);
    vertex.position = glm::vec3(p1, 0.0f);
    batch2DVertices.push_back(vertex);
    vertex.position = glm::vec3(p2, 0.0f);
    batch2DVertices.push_back(vertex);
    // Degenerate 4th vertex
    vertex.position = glm::vec3(p2, 0.0f);
    batch2DVertices.push_back(vertex);

    batch2DIndices.push_back(vertexOffset + 0);
    batch2DIndices.push_back(vertexOffset + 1);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 2);
    batch2DIndices.push_back(vertexOffset + 3);
    batch2DIndices.push_back(vertexOffset + 0);

    batch2DStats.triangleCount++;
}


void Renderer_Metal::readPixelsAsync(ScreenshotCallback callback) {
    m_pendingScreenshots.push_back(callback);
}

void Renderer_Metal::uploadRectLightVideoTexture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return;

    if (!rectLightVideoTexture
        || rectLightVideoTexture->width()  != width
        || rectLightVideoTexture->height() != height) {
        auto desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        desc->setWidth(width);
        desc->setHeight(height);
        desc->setMipmapLevelCount(1);
        desc->setUsage(MTL::TextureUsageShaderRead);
        desc->setStorageMode(MTL::StorageModeManaged);
        rectLightVideoTexture = NS::TransferPtr(device->newTexture(desc.get()));
    }
    rectLightVideoTexture->replaceRegion(
        MTL::Region::Make2D(0, 0, width, height),
        /*mipmapLevel=*/0,
        rgba,
        /*bytesPerRow=*/static_cast<NS::UInteger>(width * 4));
}

extern "C" auto getMetalDevice(void* renderer) -> void* {
    if (renderer) {
        auto* metalRenderer = static_cast<Renderer_Metal*>(renderer);
        return static_cast<void*>(metalRenderer->getDevice());
    }
    return nullptr;
}


void Renderer_Metal::draw(entt::registry& registry, std::shared_ptr<RenderScene> scene, Camera& camera) {
    // Build ECS instance data; draw(scene, camera) will clear instances/instanceBatches from Nodes,
    // so store them here and inject after Node traversal via pendingEcsInstances.
    pendingEcsInstances.clear();
    pendingEcsBatches.clear();
    pendingEcsAccelInstances.clear();
    auto view = registry.view<::Vapor::TransformComponent, ::Vapor::MeshRendererComponent>();
    for (auto entity : view) {
        auto& transform = view.get<::Vapor::TransformComponent>(entity);
        auto& meshRenderer = view.get<::Vapor::MeshRendererComponent>(entity);
        if (!meshRenderer.visible) continue;
        for (auto& mesh : meshRenderer.meshes) {
            // Compute world AABB from local AABB and current ECS worldTransform.
            const glm::mat4& worldMat = transform.worldTransform;
            const glm::vec3& lMin = mesh->localAABBMin;
            const glm::vec3& lMax = mesh->localAABBMax;
            glm::vec3 wMin(FLT_MAX), wMax(-FLT_MAX);
            for (int cx = 0; cx < 2; ++cx)
                for (int cy = 0; cy < 2; ++cy)
                    for (int cz = 0; cz < 2; ++cz) {
                        glm::vec3 corner(cx ? lMax.x : lMin.x, cy ? lMax.y : lMin.y, cz ? lMax.z : lMin.z);
                        glm::vec3 w = glm::vec3(worldMat * glm::vec4(corner, 1.0f));
                        wMin = glm::min(wMin, w);
                        wMax = glm::max(wMax, w);
                    }
            mesh->worldAABBMin = wMin;
            mesh->worldAABBMax = wMax;

            glm::vec3 bsCenter = (wMin + wMax) * 0.5f;
            float bsRadius = glm::length(wMax - bsCenter);
            auto instanceIdx = static_cast<uint32_t>(pendingEcsInstances.size());
            pendingEcsInstances.push_back({
                .model = worldMat,
                .color = glm::vec4(1.0f),
                .vertexOffset = mesh->vertexOffset,
                .indexOffset = mesh->indexOffset,
                .vertexCount = mesh->vertexCount,
                .indexCount = mesh->indexCount,
                .materialID = mesh->materialID,
                .primitiveMode = static_cast<::PrimitiveMode>(static_cast<int>(mesh->primitiveMode)),
                .AABBMin = wMin,
                .AABBMax = wMax,
                .boundingSphere = glm::vec4(bsCenter, bsRadius),
            });
            if (mesh->material) {
                pendingEcsBatches[mesh->material].push_back(MeshDraw{ mesh, instanceIdx });
            }

            if (m_supportsRaytracing) {
                // Zero-initialize: options and intersectionFunctionTableOffset were
                // previously stack garbage. Garbage options bits (e.g. NonOpaque,
                // winding/culling flags) make rays miss or misclassify whole
                // instances — visible as shadows losing coverage with bright bands —
                // and nondeterministically, since stack contents change run to run.
                // Garbage bytes also destabilized the change-detection memcmp that
                // drives the TLAS rebuild/skip logic.
                MTL::AccelerationStructureInstanceDescriptor accelDesc{};
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 3; ++j)
                        accelDesc.transformationMatrix.columns[i][j] = worldMat[i][j];
                accelDesc.accelerationStructureIndex = mesh->instanceID;
                accelDesc.mask = 0xFF;
                accelDesc.options = MTL::AccelerationStructureInstanceOptionOpaque;
                accelDesc.intersectionFunctionTableOffset = 0;
                pendingEcsAccelInstances.push_back(accelDesc);
            }
        }
    }
    // Staged meshes (GLTF scenes, static) — add after ECS entities
    for (size_t i = 0; i < scene->stagedMeshes.size(); ++i) {
        auto& mesh = scene->stagedMeshes[i];
        const glm::mat4& worldMat =
            i < scene->stagedMeshTransforms.size() ? scene->stagedMeshTransforms[i] : glm::identity<glm::mat4>();
        const glm::vec3& lMin = mesh->localAABBMin;
        const glm::vec3& lMax = mesh->localAABBMax;
        glm::vec3 wMin(FLT_MAX), wMax(-FLT_MAX);
        for (int cx = 0; cx < 2; ++cx)
            for (int cy = 0; cy < 2; ++cy)
                for (int cz = 0; cz < 2; ++cz) {
                    glm::vec3 corner(cx ? lMax.x : lMin.x, cy ? lMax.y : lMin.y, cz ? lMax.z : lMin.z);
                    glm::vec3 w = glm::vec3(worldMat * glm::vec4(corner, 1.0f));
                    wMin = glm::min(wMin, w);
                    wMax = glm::max(wMax, w);
                }
        glm::vec3 bsCenter = (wMin + wMax) * 0.5f;
        float bsRadius = glm::length(wMax - bsCenter);
        auto instanceIdx = static_cast<uint32_t>(pendingEcsInstances.size());
        pendingEcsInstances.push_back({
            .model = worldMat,
            .color = glm::vec4(1.0f),
            .vertexOffset = mesh->vertexOffset,
            .indexOffset = mesh->indexOffset,
            .vertexCount = mesh->vertexCount,
            .indexCount = mesh->indexCount,
            .materialID = mesh->materialID,
            .primitiveMode = static_cast<::PrimitiveMode>(static_cast<int>(mesh->primitiveMode)),
            .AABBMin = wMin,
            .AABBMax = wMax,
            .boundingSphere = glm::vec4(bsCenter, bsRadius),
        });
        if (mesh->material) {
            pendingEcsBatches[mesh->material].push_back(MeshDraw{ mesh, instanceIdx });
        }
    }
    draw(scene, camera);
    pendingEcsInstances.clear();
    pendingEcsBatches.clear();
    pendingEcsAccelInstances.clear();
}

auto Renderer_Metal::loadHDRI(const std::string& path) -> void {
    // Load equirectangular HDR image on the CPU
    auto img = AssetManager::loadHDRI(path);

    // Create (or recreate) the 2D RGBA32Float texture for the equirect data
    auto texDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    texDesc->setTextureType(MTL::TextureType2D);
    texDesc->setPixelFormat(MTL::PixelFormatRGBA32Float);
    texDesc->setWidth(img->width);
    texDesc->setHeight(img->height);
    texDesc->setMipmapLevelCount(1);
    texDesc->setUsage(MTL::TextureUsageShaderRead);
    texDesc->setStorageMode(MTL::StorageModeManaged);

    equirectHDRITexture = NS::TransferPtr(device->newTexture(texDesc.get()));
    equirectHDRITexture->replaceRegion(
        MTL::Region(0, 0, img->width, img->height),
        0,
        img->floatArray.data(),
        img->width * 4 * sizeof(float)
    );

    iblSource = IBLSource::HDRI;
    iblNeedsUpdate = true;
    fmt::print("HDRI loaded: {} ({}x{})\n", path, img->width, img->height);
}

// ============================================================================
// ECS Particle Integration API (Metal backend)
// ============================================================================

void Renderer_Metal::ensureParticleFreeList() {
    if (!m_particleFreeListInitialized) {
        m_particleSlotFreeList.push_back({0u, MAX_PARTICLES});
        m_particleFreeListInitialized = true;
    }
}

uint32_t Renderer_Metal::allocParticleSlots(uint32_t count) {
    ensureParticleFreeList();
    for (size_t i = 0; i < m_particleSlotFreeList.size(); ++i) {
        auto& r = m_particleSlotFreeList[i];
        if (r.count >= count) {
            uint32_t begin = r.begin;
            r.begin += count;
            r.count -= count;
            if (r.count == 0)
                m_particleSlotFreeList.erase(m_particleSlotFreeList.begin() + i);
            return begin;
        }
    }
    return ~0u; // pool exhausted
}

void Renderer_Metal::freeParticleSlots(uint32_t slotBegin, uint32_t count) {
    if (count == 0) return;
    m_particleSlotFreeList.push_back({slotBegin, count});
    std::sort(m_particleSlotFreeList.begin(), m_particleSlotFreeList.end(),
              [](const ParticleSlotRange& a, const ParticleSlotRange& b) {
                  return a.begin < b.begin;
              });
    for (size_t i = 0; i + 1 < m_particleSlotFreeList.size(); ) {
        auto& cur = m_particleSlotFreeList[i];
        auto& nxt = m_particleSlotFreeList[i + 1];
        if (cur.begin + cur.count == nxt.begin) {
            cur.count += nxt.count;
            m_particleSlotFreeList.erase(m_particleSlotFreeList.begin() + i + 1);
        } else {
            ++i;
        }
    }
}

uint32_t Renderer_Metal::claimParticleSlots(uint32_t count) {
    uint32_t begin = allocParticleSlots(count);
    if (begin != ~0u)
        particleCount = std::max(particleCount, begin + count); // expand dispatch range
    return begin;
}

void Renderer_Metal::releaseParticleSlots(uint32_t slotBegin, uint32_t count) {
    freeParticleSlots(slotBegin, count);
    // Zero-clear the released GPU slots so freed particles vanish immediately
    // (age=0 >= lifetime=0 → the compute passes skip them). Without this a
    // freed mid-buffer range keeps rendering stale data.
    if (count > 0 && particleBuffer && slotBegin + count <= MAX_PARTICLES) {
        auto* dst = reinterpret_cast<GPUParticleData*>(particleBuffer->contents()) + slotBegin;
        std::memset(dst, 0, count * sizeof(GPUParticleData));
    }
    // Recompute the high-water mark: the tail must be entirely free (see the
    // Vulkan Renderer::releaseParticleSlots comment).
    uint32_t hw = MAX_PARTICLES;
    for (const auto& r : m_particleSlotFreeList) {
        if (r.begin + r.count == MAX_PARTICLES) { hw = r.begin; break; }
    }
    particleCount = hw;
}

void Renderer_Metal::uploadParticles(uint32_t slotBegin,
                                     const std::vector<GPUParticleData>& particles) {
    if (slotBegin == ~0u || particles.empty()) return;
    if (slotBegin + particles.size() > MAX_PARTICLES) return;
    if (!particleBuffer) return;
    auto* dst = reinterpret_cast<GPUParticleData*>(particleBuffer->contents()) + slotBegin;
    std::memcpy(dst, particles.data(), particles.size() * sizeof(GPUParticleData));
    // StorageModeShared — no explicit flush needed; GPU reads after CPU writes are coherent.
}

void Renderer_Metal::setParticleForceField(const ParticleForceField& field) {
    m_forceField = field;
    if (m_forceField.attractors.size() > MAX_PARTICLE_ATTRACTORS)
        m_forceField.attractors.resize(MAX_PARTICLE_ATTRACTORS);
}
