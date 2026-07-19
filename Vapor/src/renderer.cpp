#include "renderer.hpp"
#include "stats_log.hpp"
#include "rhi_vulkan.hpp"
#include <chrono>
#ifdef __APPLE__
#include "rhi_metal.hpp"
#endif
#include "helper.hpp"
#include "components.hpp"
#include "engine_core.hpp"
#include "asset_manager.hpp"  // AssetManager::loadHDRI
#include "rmlui_manager.hpp"
#include "graphics_gibs.hpp"  // Surfel/GIBSData/param structs (Metal-layout asserted)
#include "Vapor/rml_renderer_rhi.hpp"
#include <RmlUi/Core.h>
#include <SDL3/SDL_video.h>
#include <fmt/core.h>
#include <map>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <random>
#include <cmath>
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

using namespace Vapor;

namespace {
// Froxel fog grid dimensions — MUST match FROXEL_SIZE_X/Y/Z in
// 3d_volumetric_fog.metal (and any GLSL twin). Injection dispatches one thread
// per froxel; integration one thread per (x,y) column.
constexpr Uint32 FROXEL_GRID_X = 160;
constexpr Uint32 FROXEL_GRID_Y = 90;
constexpr Uint32 FROXEL_GRID_Z = 64;

// GIBS spatial-hash grid derivation, matching native GIBSManager::calculateGridSize
// (gibs_manager.cpp) EXACTLY so the RHI surfel hash resolves at the same cell
// granularity: start from a 1m cell, take ceil(worldSize / cell) per axis,
// clamp to 256, then snap the cell size back to the resulting grid. With the
// native ±64 default bounds this yields a 128^3 grid at 1.0m cells (NOT the
// 64^3 the RHI previously hardcoded). Returned by reference to keep the buffer
// allocation and the per-frame gibsPass() in lockstep.
inline void computeGibsGrid(const glm::vec3& worldMin, const glm::vec3& worldMax,
                            glm::uvec3& gridSize, Uint32& totalCells, float& cellSize) {
    glm::vec3 worldSize = worldMax - worldMin;
    cellSize = 1.0f;  // native's member default before recompute
    gridSize.x = std::min(static_cast<Uint32>(std::ceil(worldSize.x / cellSize)), 256u);
    gridSize.y = std::min(static_cast<Uint32>(std::ceil(worldSize.y / cellSize)), 256u);
    gridSize.z = std::min(static_cast<Uint32>(std::ceil(worldSize.z / cellSize)), 256u);
    totalCells = gridSize.x * gridSize.y * gridSize.z;
    cellSize = std::max({worldSize.x / gridSize.x,
                         worldSize.y / gridSize.y,
                         worldSize.z / gridSize.z});
}
}  // namespace

// ============================================================================
// Initialization
// ============================================================================

void Renderer::initialize(std::unique_ptr<RHI> rhiPtr, GraphicsBackend backendType) {
    rhi = std::move(rhiPtr);
    backend = backendType;

    // Snapshot backend capabilities; the render graph uses them to skip
    // passes the backend can't run (e.g. raytracing passes on Vulkan).
    capabilities = rhi->getCapabilities();
    // Bisect switch: VAPOR_DISABLE_RT=1 forces the non-RT path (no BLAS/TLAS,
    // no RT pipelines, every RequiresRaytracing pass auto-skips) — for
    // isolating RT-chain issues from the rest of the frame on RT hardware.
    if (std::getenv("VAPOR_DISABLE_RT")) {
        capabilities.raytracing = false;
        fmt::print("VAPOR_DISABLE_RT set: forcing raytracing off\n");
    }
    setupDefaultRenderGraph();

    // Per-frame buffer slotting keeps exactly one buffer slot per in-flight
    // frame, so beginFrame() never rewrites a slot the GPU may still be reading:
    // the slot index wraps every frameSlotCount frames and the CPU runs at most
    // getMaxFramesInFlight() frames ahead. Deriving the count from the backend
    // here (rather than a renderer-side constant) makes it the single source of
    // truth — it cannot drift out of sync. Must be set before the first
    // createFrameSlottedBuffer() call below.
    frameSlotCount = rhi->getMaxFramesInFlight();

    // Create uniform buffers. Everything rewritten per frame goes through
    // createFrameSlottedBuffer (see renderer.hpp: frames-in-flight slotting).
    BufferDesc cameraBufferDesc;
    cameraBufferDesc.size = sizeof(CameraRenderData);
    cameraBufferDesc.usage = BufferUsage::Uniform;
    cameraBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(cameraUniformBuffer, cameraBufferDesc);

    // Material data buffer - stores array of all materials (used by shader at
    // binding 1). Load-time content, not rewritten per frame — unslotted.
    BufferDesc materialBufferDesc;
    materialBufferDesc.size = sizeof(Vapor::MaterialData) * MAX_INSTANCES;  // Reserve space for max materials
    materialBufferDesc.usage = BufferUsage::Uniform;
    materialBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    materialUniformBuffer = rhi->createBuffer(materialBufferDesc);

    BufferDesc dirLightBufferDesc;
    dirLightBufferDesc.size = sizeof(DirectionalLightData) * maxDirectionalLights;
    dirLightBufferDesc.usage = BufferUsage::Uniform;
    dirLightBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(directionalLightBuffer, dirLightBufferDesc);

    BufferDesc pointLightBufferDesc;
    pointLightBufferDesc.size = sizeof(PointLightData) * maxPointLights;
    pointLightBufferDesc.usage = BufferUsage::Uniform;
    pointLightBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(pointLightBuffer, pointLightBufferDesc);

    BufferDesc spotLightBufferDesc;
    spotLightBufferDesc.size = sizeof(Vapor::SpotLight) * maxSpotLights;
    spotLightBufferDesc.usage = BufferUsage::Uniform;
    spotLightBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(spotLightBuffer, spotLightBufferDesc);

    BufferDesc instanceDataBufferDesc;
    instanceDataBufferDesc.size = sizeof(Vapor::InstanceData) * MAX_INSTANCES;
    instanceDataBufferDesc.usage = BufferUsage::Uniform;
    instanceDataBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(instanceDataBuffer, instanceDataBufferDesc);

    // GPU-driven rendering: indirect draw args produced by the cull compute pass
    // (one DrawCommand per instance). Frame-slotted like instanceDataBuffer so
    // the compute writes and indirect draw of a frame use the same slot.
    BufferDesc gpuCullArgsBufferDesc;
    gpuCullArgsBufferDesc.size = sizeof(Vapor::DrawCommand) * MAX_INSTANCES;
    gpuCullArgsBufferDesc.usage = BufferUsage::Indirect;
    gpuCullArgsBufferDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(gpuCullArgsBuffer, gpuCullArgsBufferDesc);
    // Frustum-only cull output for the GPU-driven pre-pass (same layout).
    createFrameSlottedBuffer(prepassCullArgsBuffer, gpuCullArgsBufferDesc);

    // Render-to-texture view: its own camera/instance buffers (see renderer.hpp
    // note — the shared ones are overwritten by the main draw later in the same
    // frame, before the GPU executes anything).
    BufferDesc rttCamDesc;
    rttCamDesc.size = sizeof(CameraRenderData);
    rttCamDesc.usage = BufferUsage::Uniform;
    rttCamDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(rttCameraBuffer, rttCamDesc);

    BufferDesc rttInstDesc;
    rttInstDesc.size = sizeof(Vapor::InstanceData) * MAX_INSTANCES;
    rttInstDesc.usage = BufferUsage::Uniform;
    rttInstDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    createFrameSlottedBuffer(rttInstanceBuffer, rttInstDesc);

    // Shader-contract buffers (clusters / rect lights / PSSM), neutral-filled.
    // See the "Full PBR shader contract" note in renderer.hpp.
    {
        BufferDesc clusterDesc;
        clusterDesc.size = sizeof(Vapor::Cluster) * clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;
        clusterDesc.usage = BufferUsage::Storage;
        clusterDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        // Zero-fill: the PBR shaders read cluster lightCounts before the first
        // TileCulling dispatch lands — garbage counts loop garbage lights
        // (NaN-black frames on Metal). GPU-written per frame by TileCulling —
        // slotted like native's clusterBuffers[frameInFlight].
        {
            std::vector<Uint8> clusterZeros(clusterDesc.size, 0);
            createFrameSlottedBuffer(clusterBuffer, clusterDesc, clusterZeros.data(), clusterDesc.size);
        }

        BufferDesc rectDesc;
        rectDesc.size = sizeof(Vapor::RectLight) * maxRectLights;
        rectDesc.usage = BufferUsage::Storage;
        rectDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        {
            std::vector<Vapor::RectLight> zeroRects(maxRectLights, Vapor::RectLight{});
            createFrameSlottedBuffer(rectLightBuffer, rectDesc, zeroRects.data(), rectDesc.size);
        }

        BufferDesc pssmDesc;
        pssmDesc.size = sizeof(PSSMRenderData);
        pssmDesc.usage = BufferUsage::Uniform;
        pssmDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        PSSMRenderData neutralPSSM;
        createFrameSlottedBuffer(pssmDataBuffer, pssmDesc, &neutralPSSM, sizeof(neutralPSSM));

        // Written once here (runtime atmosphere params travel via push bytes);
        // unslotted.
        BufferDesc atmoDesc;
        atmoDesc.size = sizeof(AtmosphereRenderData);
        atmoDesc.usage = BufferUsage::Uniform;
        atmoDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        atmosphereDataBuffer = rhi->createBuffer(atmoDesc);
        rhi->updateBuffer(atmosphereDataBuffer, &atmosphereData, 0, sizeof(atmosphereData));

        // Gradient sky colors (SkyType::Gradient); re-uploaded by setSky. Same
        // unslotted single-buffer pattern as the atmosphere params above.
        BufferDesc gradDesc;
        gradDesc.size = sizeof(GradientRenderData);
        gradDesc.usage = BufferUsage::Uniform;
        gradDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        gradientDataBuffer = rhi->createBuffer(gradDesc);
        rhi->updateBuffer(gradientDataBuffer, &gradientData, 0, sizeof(gradientData));

        // Night-sky (stars + moon) visuals; re-uploaded by setSky. Same pattern.
        BufferDesc nsDesc;
        nsDesc.size = sizeof(NightSkyRenderData);
        nsDesc.usage = BufferUsage::Uniform;
        nsDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        nightSkyDataBuffer = rhi->createBuffer(nsDesc);
        rhi->updateBuffer(nightSkyDataBuffer, &nightSkyData, 0, sizeof(nightSkyData));

        BufferDesc lsDesc;
        lsDesc.size = sizeof(LightScatteringRenderData);
        lsDesc.usage = BufferUsage::Uniform;
        lsDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        LightScatteringRenderData lsDefaults;
        createFrameSlottedBuffer(lightScatteringDataBuffer, lsDesc, &lsDefaults, sizeof(lsDefaults));

        BufferDesc fogDesc;
        fogDesc.size = sizeof(FogRenderData);
        fogDesc.usage = BufferUsage::Uniform;
        fogDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        FogRenderData fogDefaults;
        createFrameSlottedBuffer(fogDataBuffer, fogDesc, &fogDefaults, sizeof(fogDefaults));

        // Fog volume array (global + bounded AABB banks) blended by the Vulkan
        // raymarch and the froxel inject kernel. Bound as a storage buffer.
        BufferDesc fogVolDesc;
        fogVolDesc.size = sizeof(VolumetricFogVolumeGPU) * kMaxFogVolumes;
        fogVolDesc.usage = BufferUsage::Uniform;
        fogVolDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        createFrameSlottedBuffer(fogVolumeBuffer, fogVolDesc);

        // Froxel-fog globals (VolumetricFogData layout — camera/sun/grid/temporal),
        // read by the froxel inject + composite. Distinct from fogDataBuffer, which
        // is the FogRenderData layout the fullscreen raymarch reads.
        BufferDesc froxelGlobalsDesc;
        froxelGlobalsDesc.size = sizeof(VolumetricFogData);
        froxelGlobalsDesc.usage = BufferUsage::Uniform;
        froxelGlobalsDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        VolumetricFogData froxelGlobalsDefaults;
        createFrameSlottedBuffer(fogFroxelGlobalsBuffer, froxelGlobalsDesc,
                                 &froxelGlobalsDefaults, sizeof(froxelGlobalsDefaults));

        // Froxel 3D textures: the injection grid (in-scatter.rgb + extinction) and
        // the integrated volume (accumulated scattering.rgb + transmittance). Both
        // RGBA16F, storage-written by the compute kernels and sampled by the
        // composite. Only used on the Metal backend for now (Vulkan keeps the
        // raymarch); harmless to allocate on both.
        {
            TextureDesc froxelTexDesc;
            froxelTexDesc.width = FROXEL_GRID_X;
            froxelTexDesc.height = FROXEL_GRID_Y;
            froxelTexDesc.depth = FROXEL_GRID_Z;
            froxelTexDesc.format = PixelFormat::RGBA16_FLOAT;
            froxelTexDesc.usage = TextureUsage::Storage | TextureUsage::Sampled;
            fogFroxelGridTexture = rhi->createTexture(froxelTexDesc);
            fogIntegratedVolumeTexture = rhi->createTexture(froxelTexDesc);
        }

        BufferDesc hfogDesc;
        hfogDesc.size = sizeof(HeightFogRenderData);
        hfogDesc.usage = BufferUsage::Uniform;
        hfogDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        HeightFogRenderData hfogDefaults;
        createFrameSlottedBuffer(heightFogDataBuffer, hfogDesc, &hfogDefaults, sizeof(hfogDefaults));

        BufferDesc volDesc;
        volDesc.size = sizeof(VolumeRenderData);
        volDesc.usage = BufferUsage::Uniform;
        volDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        createFrameSlottedBuffer(volumeDataBuffer, volDesc, &volumeSettings, sizeof(volumeSettings));

        // Procedural 64^3 test density grid (radial-falloff smoke puff shaped
        // by trilinear value noise) so the volume raymarch pass has something
        // to draw before real EmberGen exports arrive (import is a future PR).
        {
            constexpr Uint32 N = 64;
            auto hash3 = [](int x, int y, int z) {
                Uint32 h = Uint32(x) * 73856093u ^ Uint32(y) * 19349663u ^ Uint32(z) * 83492791u;
                h = (h ^ (h >> 13)) * 0x85ebca6bu;
                return float((h ^ (h >> 16)) & 0xFFFFFFu) / float(0xFFFFFFu);
            };
            auto valueNoise = [&](float x, float y, float z) {
                int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
                float fx = x - xi, fy = y - yi, fz = z - zi;
                auto lerp3 = [](float a, float b, float t) { return a + (b - a) * t; };
                float c00 = lerp3(hash3(xi, yi, zi),     hash3(xi + 1, yi, zi),     fx);
                float c10 = lerp3(hash3(xi, yi + 1, zi), hash3(xi + 1, yi + 1, zi), fx);
                float c01 = lerp3(hash3(xi, yi, zi + 1),     hash3(xi + 1, yi, zi + 1),     fx);
                float c11 = lerp3(hash3(xi, yi + 1, zi + 1), hash3(xi + 1, yi + 1, zi + 1), fx);
                return lerp3(lerp3(c00, c10, fy), lerp3(c01, c11, fy), fz);
            };
            std::vector<Uint8> voxels(N * N * N);
            for (Uint32 z = 0; z < N; z++) {
                for (Uint32 y = 0; y < N; y++) {
                    for (Uint32 x = 0; x < N; x++) {
                        glm::vec3 q(x / float(N - 1) * 2.0f - 1.0f,
                                    y / float(N - 1) * 2.0f - 1.0f,
                                    z / float(N - 1) * 2.0f - 1.0f);
                        float r = glm::length(q);
                        glm::vec3 s = (q + 1.0f) * 4.0f;  // noise domain
                        float fbm = 0.6f * valueNoise(s.x, s.y, s.z)
                                  + 0.3f * valueNoise(s.x * 2.0f, s.y * 2.0f, s.z * 2.0f)
                                  + 0.1f * valueNoise(s.x * 4.0f, s.y * 4.0f, s.z * 4.0f);
                        float d = std::clamp(fbm * 1.5f - r * 1.1f, 0.0f, 1.0f);
                        voxels[x + y * N + z * N * N] = Uint8(d * 255.0f + 0.5f);
                    }
                }
            }
            volumeTestTexture = createVolumeTexture(N, N, N, voxels.data(), voxels.size());
            volumeDensityTexture = volumeTestTexture;
        }

        BufferDesc prevVPDesc;
        prevVPDesc.size = sizeof(glm::mat4);
        prevVPDesc.usage = BufferUsage::Uniform;
        prevVPDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        glm::mat4 identityVP(1.0f);
        createFrameSlottedBuffer(prevViewProjBuffer, prevVPDesc, &identityVP, sizeof(identityVP));

        BufferDesc aoTemporalDesc;
        aoTemporalDesc.size = sizeof(AOTemporalRenderData);
        aoTemporalDesc.usage = BufferUsage::Uniform;
        aoTemporalDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        AOTemporalRenderData aoTemporalDefaults;
        createFrameSlottedBuffer(aoTemporalDataBuffer, aoTemporalDesc, &aoTemporalDefaults, sizeof(aoTemporalDefaults));

        BufferDesc cloudDesc;
        cloudDesc.size = sizeof(VolumetricCloudRenderData);
        cloudDesc.usage = BufferUsage::Uniform;
        cloudDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        createFrameSlottedBuffer(cloudDataBuffer, cloudDesc, &cloudSettings, sizeof(cloudSettings));

        BufferDesc fdDesc;
        fdDesc.size = sizeof(FrameData);
        fdDesc.usage = BufferUsage::Uniform;
        fdDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        FrameData fdInit{};
        createFrameSlottedBuffer(frameDataBuffer, fdDesc, &fdInit, sizeof(fdInit));

        BufferDesc lcDesc;
        lcDesc.size = sizeof(Vapor::LightCullData);
        lcDesc.usage = BufferUsage::Storage;
        lcDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        Vapor::LightCullData lcInit{};
        createFrameSlottedBuffer(lightCullDataBuffer, lcDesc, &lcInit, sizeof(lcInit));

        BufferDesc sfDesc;
        sfDesc.size = sizeof(SunFlareRenderData);
        sfDesc.usage = BufferUsage::Uniform;
        sfDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        createFrameSlottedBuffer(sunFlareDataBuffer, sfDesc, &sunFlareSettings, sizeof(sunFlareSettings));

        // Post-process tunables (chromatic aberration / vignette / color grade
        // / exposure). The Vulkan PostProcess.frag reads them at set1 b0; Metal
        // pushes the same struct via setFragmentBytes.
        BufferDesc pppDesc;
        pppDesc.size = sizeof(PostProcessParams);
        pppDesc.usage = BufferUsage::Storage;
        pppDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        createFrameSlottedBuffer(postProcessParamsBuffer, pppDesc, &postProcessParams, sizeof(postProcessParams));

        // IBL capture slots: 6 sky faces + 6 irradiance + 5x6 prefilter = 42
        // entries, each draw binds its own offset (a single rewritten buffer
        // would race — host-visible updates are immediate, the GPU runs later).
        BufferDesc iblDesc;
        iblDesc.size = sizeof(IBLCaptureRenderData) * 42;
        iblDesc.usage = BufferUsage::Uniform;
        iblDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        iblCaptureDataBuffer = rhi->createBuffer(iblDesc);
    }

    // IBL cubemaps + BRDF LUT (RGBA16F; sizes match the native chain).
    {
        TextureDesc cd;
        cd.format = PixelFormat::RGBA16_FLOAT;
        cd.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        cd.isCube = true;
        cd.arrayLayers = 6;

        cd.width = cd.height = 512;
        cd.mipLevels = 10;  // full chain for prefilter source sampling
        environmentCubemap = rhi->createTexture(cd);

        cd.width = cd.height = 32;
        cd.mipLevels = 1;
        irradianceMap = rhi->createTexture(cd);

        cd.width = cd.height = 128;
        cd.mipLevels = PREFILTER_MIP_LEVELS;
        prefilterMap = rhi->createTexture(cd);

        TextureDesc bd;
        bd.width = bd.height = 512;
        bd.format = PixelFormat::RGBA16_FLOAT;
        bd.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        brdfLUTTex = rhi->createTexture(bd);

        // IBL debug: 2D equirect unwrap of environmentCubemap for ImGui.
        TextureDesc pd;
        pd.width = 512;
        pd.height = 256;
        pd.format = PixelFormat::RGBA16_FLOAT;
        pd.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        iblPreviewRT = rhi->createTexture(pd);
    }

    // Empty TLAS on RT-capable backends; instances are refit/rebuilt per frame
    // by buildAccelerationStructures() from the visible drawables.
    if (capabilities.raytracing) {
        AccelStructDesc td;
        td.type = AccelStructType::TopLevel;
        td.allowUpdate = true;
        sceneTLAS = rhi->createAccelerationStructure(td);

        // GIBS surfel pool + spatial hash + counters + per-frame data. Only on
        // RT backends (the surfel pool alone is 64MB at Medium quality).
        BufferDesc bd;
        bd.usage = BufferUsage::Storage;
        bd.memoryUsage = MemoryUsage::GPU;
        bd.size = size_t(gibsMaxSurfels) * sizeof(Surfel);
        surfelBuffer = rhi->createBuffer(bd);
        // Spatial-hash cell heads: one u32 per grid cell. Sized from native's
        // grid derivation (128^3 at the ±64 default) so the buffer, GIBSData,
        // and the surfelClear dispatch in gibsPass() all agree on totalCells.
        glm::uvec3 gibsGrid; Uint32 gibsTotalCells; float gibsCellSize;
        computeGibsGrid(gibsWorldMin, gibsWorldMax, gibsGrid, gibsTotalCells, gibsCellSize);
        bd.size = size_t(gibsTotalCells) * sizeof(Uint32);
        cellHeadBuffer = rhi->createBuffer(bd);
        bd.size = size_t(gibsMaxSurfels) * sizeof(Uint32);
        surfelNextBuffer = rhi->createBuffer(bd);
        // Free-list: indices of surfel slots surfelUpdate has evicted, popped by
        // generation for reuse so the pool self-recycles (holds at most maxSurfels).
        surfelFreeListBuffer = rhi->createBuffer(bd);

        BufferDesc cb;
        cb.usage = BufferUsage::Storage;
        cb.memoryUsage = MemoryUsage::CPUtoGPU;  // CPU reads active count back
        cb.size = 4 * sizeof(Uint32);
        surfelCounterBuffer = rhi->createBuffer(cb);
        Uint32 zeros[4] = {};
        rhi->updateBuffer(surfelCounterBuffer, zeros, 0, sizeof(zeros));

        BufferDesc gd;
        gd.usage = BufferUsage::Uniform;
        gd.memoryUsage = MemoryUsage::CPUtoGPU;
        gd.size = sizeof(GIBSData);
        createFrameSlottedBuffer(gibsDataBuffer, gd);  // rewritten each GIBS frame
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

    // Register the renderer-side telemetry sources. Driven once per frame by
    // StatsLog::tick() in beginFrame; the RHI backend registers its own "VK"/
    // "MTL" source, so --stats prints one grouped line per subsystem.
    registerStatsSources();
}

// Renderer-side stat sources (the map counts live above the RHI). The backend
// contributes "VK"/"MTL" from its own constructor.
void Renderer::registerStatsSources() {
    auto& log = Vapor::StatsLog::get();

    log.addSource("R", [this](Vapor::StatLine& s) {
        s.add("textures", textures.size());
        s.add("texCache", textureCache.size());
        s.add("imguiTexCache", imguiTextureCache.size());
        s.add("meshes", meshes.size());
        s.add("materials", materials.size());
        s.add("renderTextures", renderTextures.size());
        s.add("drawables", frameDrawables.size());
        s.add("instances", totalInstanceCount);
    });

    // RT diagnostics: only re-emitted when a count changes (was VAPOR_RT_DEBUG).
    log.addSource("RT", [this](Vapor::StatLine& s) {
        Uint32 validBLAS = 0;
        for (const auto& b : meshBLAS) if (b.isValid()) ++validBLAS;
        s.add("raytracing", capabilities.raytracing ? 1u : 0u);
        s.add("tlasValid", sceneTLAS.isValid() ? 1u : 0u);
        s.add("blasValid", validBLAS);
        s.add("blasTotal", static_cast<Uint32>(meshBLAS.size()));
        s.add("visible", static_cast<Uint32>(visibleDrawables.size()));
        s.add("tlasInst", static_cast<Uint32>(tlasInstances.size()));
    }, Vapor::StatsLog::Mode::OnChange);

    // Tile-cull histogram: reads back the (host-visible) cluster buffer via the
    // shared helper. The waitIdle only runs on emit frames while --stats is on.
    log.addSource("CULL", [this](Vapor::StatLine& s) {
        Uint32 mn, avg, mx, nonEmpty;
        if (!sampleClusterHistogram(mn, avg, mx, nonEmpty)) return;
        s.add("tiles", clusterGridSizeX * clusterGridSizeY);
        s.add("pointLights", static_cast<Uint32>(pointLights.size()));
        s.add("min", mn);
        s.add("avg", avg);
        s.add("max", mx);
        s.add("nonEmpty", nonEmpty);
    });

    // GPU-driven cull diagnostics (was the always-on [MDI diag] fmt::print in the
    // MDI draw branch). Covers every indirect submission mode (plain Indirect /
    // MDI / Bindless MDI), not just MDI. Periodic: the host-visible readback runs
    // at most once per interval, and only while --stats is enabled (tick() is a
    // no-op otherwise, so zero cost in normal runs).
    //
    // Reads back gpuCullArgsBuffer, which the cull compute wrote LAST frame (this
    // frame's cull hasn't run at tick() time — sane for a steady scene). Reads
    // cull output so a blank frame can be diagnosed:
    //   instCount all 0 -> cull dropped everything (frustum/camera)
    //   instCount 1, sane idxCount/firstIdx -> geometry/vertex issue
    //   garbage idxCount -> cull didn't run / wrong binding
    log.addSource("GPUDRV", [this](Vapor::StatLine& s) {
        if (!gpuDrivenIndirect() || totalInstanceCount == 0) return;
        s.add("mode", lastFrameStats.mainPath);
        s.add("instances", totalInstanceCount);
        s.add("ranges", static_cast<Uint32>(m_materialRanges.size()));
        s.add("mergedVtx", static_cast<Uint32>(m_mergedVertices.size()));
        s.add("mergedIdx", static_cast<Uint32>(m_mergedIndices.size()));
        // Metal's Bindless-MDI encodes into sceneICB, not gpuCullArgsBuffer, so
        // the args readback (visible + cmd0) isn't meaningful there.
        const bool icbMetal = backend == GraphicsBackend::Metal && gpuDrivenBindless() &&
                              capabilities.indirectCommandBuffers && sceneICB.isValid();
        if (icbMetal || !gpuCullArgsBuffer.isValid()) { s.add("args", "icb"); return; }
        if (void* mapped = rhi->mapBuffer(gpuCullArgsBuffer)) {
            const auto* cmds = static_cast<const Vapor::DrawCommand*>(mapped);
            Uint32 visible = 0;
            const Uint32 n = std::min<Uint32>(totalInstanceCount, MAX_INSTANCES);
            for (Uint32 i = 0; i < n; ++i) visible += (cmds[i].instanceCount != 0) ? 1u : 0u;
            const Vapor::DrawCommand& c = cmds[0];  // instance 0 = first material range's start
            s.add("visible", visible);
            s.add("idxCount", c.indexCount);
            s.add("instCount", c.instanceCount);
            s.add("firstIdx", c.firstIndex);
            s.add("vtxOff", c.vertexOffset);
            s.add("firstInst", c.firstInstance);
            rhi->unmapBuffer(gpuCullArgsBuffer);
        }
    });
}

// Returns a cached grayscale / single-layer view of `src` for ImGui previews,
// rebuilding it when the source RT is recreated (resize gives it a new id). The
// view is non-owning, so destroying it never touches the source's image.
TextureHandle Renderer::debugView(const char* key, TextureHandle src,
                                  TextureSwizzle swizzle, Uint32 layer) {
    if (!src.isValid()) return {};
    auto& pv = previewViews[key];
    if (pv.srcId != src.id) {
        if (pv.view.isValid()) rhi->destroyTexture(pv.view);
        TextureViewDesc vd;
        vd.source = src;
        vd.swizzle = swizzle;
        vd.baseArrayLayer = layer;
        vd.layerCount = 1;
        pv.view = rhi->createTextureView(vd);
        pv.srcId = src.id;
    }
    return pv.view;
}

// Reads the culled cluster buffer (host-visible) and reduces the 2D tile grid to
// min/avg/max/non-empty light counts. Contains a waitIdle so the read sees the
// GPU's writes — callers must throttle (StatsLog interval, or panel %N).
bool Renderer::sampleClusterHistogram(Uint32& mn, Uint32& avg, Uint32& mx, Uint32& nonEmpty) {
    if (!clusterBuffer.isValid()) return false;
    rhi->waitIdle();
    const Uint32 tileCount = clusterGridSizeX * clusterGridSizeY;  // 2D grid
    auto* clusters = static_cast<const Vapor::Cluster*>(rhi->mapBuffer(clusterBuffer));
    if (!clusters) return false;
    Uint32 lo = ~0u, hi = 0u, sum = 0u, ne = 0u;
    for (Uint32 i = 0; i < tileCount; ++i) {
        Uint32 c = clusters[i].lightCount;
        lo = std::min(lo, c); hi = std::max(hi, c); sum += c;
        if (c > 0) ++ne;
    }
    rhi->unmapBuffer(clusterBuffer);
    mn = tileCount ? lo : 0u;
    avg = tileCount ? sum / tileCount : 0u;
    mx = hi;
    nonEmpty = ne;
    return true;
}

// One GPU buffer per frame slot; `alias` always names the current frame's
// slot (rotated in beginFrame), so bind/update sites read like a single
// buffer while in-flight frames keep their own copy.
void Renderer::createFrameSlottedBuffer(BufferHandle& alias, const BufferDesc& desc,
                                        const void* initData, size_t initSize) {
    FrameSlottedBuffer sb;
    sb.alias = &alias;
    sb.slots.resize(frameSlotCount);
    for (Uint32 i = 0; i < frameSlotCount; i++) {
        sb.slots[i] = rhi->createBuffer(desc);
        if (initData && initSize > 0) {
            rhi->updateBuffer(sb.slots[i], initData, 0, initSize);
        }
    }
    // Lazily-created buffers (e.g. particles) join mid-run: start on the
    // current frame's slot so the first rotation doesn't reuse it.
    alias = sb.slots[frameSlotIndex];
    frameSlottedBuffers.push_back(sb);
}

// Out-of-line so the unique_ptr<RmlRendererRHI> deleter is instantiated here,
// where rml_renderer_rhi.hpp provides the complete type.
Renderer::~Renderer() = default;

void Renderer::shutdown() {
    if (rhi) {
        // These sources' fills capture `this`
        auto& statsLog = Vapor::StatsLog::get();
        for (const char* tag : { "R", "RT", "CULL", "GPUDRV" }) {
            statsLog.removeSource(tag);
        }

        // GPU may still be executing the last frame; ImGui backend shutdown
        // and resource destruction below require it to be finished.
        rhi->waitIdle();

        // RmlUI renderer (RmlUi itself was already shut down by EngineCore).
        m_uiRenderer.reset();
        m_uiContext = nullptr;

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

        // Destroy buffers. Slotted buffers own one handle per frame slot — the
        // alias members point at one of them, so only the registry is walked.
        for (auto& sb : frameSlottedBuffers) {
            for (auto& slot : sb.slots) {
                if (slot.isValid()) {
                    rhi->destroyBuffer(slot);
                }
            }
            *sb.alias = {};
        }
        frameSlottedBuffers.clear();
        if (materialUniformBuffer.isValid()) {
            rhi->destroyBuffer(materialUniformBuffer);
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

    // Accumulate into the merged scene geometry used by single-call MDI. Indices
    // stay mesh-local; the per-mesh vertexOffset rebases them at draw time. These
    // offsets are only consumed in MDI mode (InstanceData keeps 0 otherwise), so
    // this is inert for the default / per-object-indirect paths.
    mesh.vertexOffset = static_cast<Uint32>(m_mergedVertices.size());
    mesh.indexOffset = static_cast<Uint32>(m_mergedIndices.size());
    m_mergedVertices.insert(m_mergedVertices.end(), vertices.begin(), vertices.end());
    m_mergedIndices.insert(m_mergedIndices.end(), indices.begin(), indices.end());
    m_mergedGeometryDirty = true;

    MeshId id = static_cast<MeshId>(meshes.size());
    meshes.push_back(mesh);

    // One BLAS per mesh on RT-capable backends. VertexData begins with the
    // position, so the buffer can be consumed directly at stride sizeof(VertexData).
    AccelStructHandle blas;
    if (capabilities.raytracing && mesh.vertexBuffer.isValid() && mesh.indexBuffer.isValid()) {
        AccelStructDesc bd;
        bd.type = AccelStructType::BottomLevel;
        AccelStructGeometry geom;
        geom.vertexBuffer = mesh.vertexBuffer;
        geom.vertexCount = mesh.vertexCount;
        geom.vertexStride = sizeof(Vapor::VertexData);
        geom.indexBuffer = mesh.indexBuffer;
        geom.indexCount = mesh.indexCount;
        bd.geometries.push_back(geom);
        blas = rhi->createAccelerationStructure(bd);
        if (blas.isValid()) rhi->buildAccelerationStructure(blas);
    }
    meshBLAS.push_back(blas);  // index-aligned with `meshes` (invalid when no RT)
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
    material.transmission = materialData.transmission;
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
        // Neutral ORM (metallic .b = 0), NOT white (which reads metallic = 1
        // and turns every un-mapped surface into flat, diffuse-less metal).
        material.metallicTexture = defaultORMTexture;
    }

    if (materialData.roughnessMap) {
        material.roughnessTexture = getOrCreateTexture(materialData.roughnessMap);
        flags |= HAS_ROUGHNESS_MAP;
    } else {
        material.roughnessTexture = defaultORMTexture;
    }

    if (materialData.occlusionMap) {
        material.occlusionTexture = getOrCreateTexture(materialData.occlusionMap);
        flags |= HAS_OCCLUSION_MAP;
    } else {
        material.occlusionTexture = defaultORMTexture;
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
    params.transmission = material.transmission;

    rhi->updateBuffer(material.parameterBuffer, &params, 0, sizeof(Vapor::MaterialData));

    MaterialId id = static_cast<MaterialId>(materials.size());
    materials.push_back(material);
    m_bindlessTableDirty = true;  // ICB mode's texture table must pick this up
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

    // Rotate every frames-in-flight buffer slot BEFORE anything writes frame
    // data: all named aliases (cameraUniformBuffer, instanceDataBuffer, ...)
    // now point at a buffer no in-flight frame reads.
    frameSlotIndex = (frameSlotIndex + 1) % frameSlotCount;
    for (auto& sb : frameSlottedBuffers) {
        *sb.alias = sb.slots[frameSlotIndex];
    }
    batch2D.nextFrame();
    batch3D.nextFrame();

    // Begin RHI frame (get drawable, create command buffer). The Vulkan
    // backend recreates an out-of-date/resized swapchain here.
    rhi->beginFrame();

    // Per-frame telemetry (--stats). Drives every registered source — renderer
    // "R"/"RT"/"CULL" and the backend's "VK"/"MTL" — emitting one grouped line
    // each to stderr and vapor_stats.log. No-op when --stats is off.
    Vapor::StatsLog::get().tick(frameNumber);

    // Window resized: the swapchain extent changed under us — rebuild every
    // swapchain-sized render target before anything records against them.
    // (Old targets are destroy-deferred; nothing has been recorded yet this
    // frame beyond the upload stream.)
    Uint32 sw = rhi->getSwapchainWidth();
    Uint32 sh = rhi->getSwapchainHeight();
    if (sw != lastRTWidth || sh != lastRTHeight) {
        if (lastRTWidth != 0) {  // skip the very first frame (initialize() built them)
            rhi->waitIdle();
            destroyRenderTargets();
            createRenderTargets();
            fmt::print("createRenderTargets: resized to {}x{}\n", sw, sh);
        }
        lastRTWidth = sw;
        lastRTHeight = sh;
    }

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
    spotLights.clear();

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
    // CPU frame timing (single-threaded submission). m_cpuPreGraphMs is the
    // cull + sort + buffer-upload cost; the per-pass recording cost is measured
    // inside renderGraph.execute (see getPassCpuTimings). Their sum is the CPU
    // cost of a frame's rendering work — compare against total GPU time to see
    // whether you are CPU- or GPU-bound.
    const auto _cpuFrameStart = std::chrono::high_resolution_clock::now();

    // Option A: when the pre-pass runs fully GPU-driven (the indirect MDI
    // pre-pass), EVERY geometry pass that consumed
    // visibleDrawables is now GPU-driven or iterates the full frameDrawables set
    // (shadows, TLAS). The CPU frustum cull would just produce an unused list, so
    // skip it — that's the point. visibleDrawables stays empty; the HUD reports
    // totalInstanceCount as the visible count in this mode.
    if (gpuDrivenPrePassActive()) {
        visibleDrawables.clear();
    } else {
        performCulling();
    }
    sortDrawables();
    updateBuffers();

    m_cpuPreGraphMs = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - _cpuFrameStart).count();

    // Execute the frame's passes. Which passes run is decided by the graph:
    // disabled passes and passes whose capability requirements the backend
    // doesn't meet (PassFlags vs RHICapabilities) are skipped — no backend
    // checks here.
    renderGraph.execute(*this, capabilities);

    // Frame stats for the Engine window (read next frame, before the clear)
    lastFrameStats.totalDrawables = static_cast<Uint32>(frameDrawables.size());
    // In Option A the CPU cull is skipped (visibleDrawables empty); the GPU
    // decides visibility per instance. Report the submitted instance count as the
    // "visible" figure rather than a misleading 0 (true GPU-visible count would
    // need a readback of the cull args).
    lastFrameStats.visibleDrawables = gpuDrivenPrePassActive()
        ? totalInstanceCount
        : static_cast<Uint32>(visibleDrawables.size());
    lastFrameStats.directionalLights = static_cast<Uint32>(directionalLights.size());
    lastFrameStats.pointLights = static_cast<Uint32>(pointLights.size());
    lastFrameStats.rectLights = static_cast<Uint32>(rectLights.size());
    lastFrameStats.spotLights = static_cast<Uint32>(spotLights.size());
}

// The engine's default frame composition. Gameplay code is free to modify
// this through getRenderGraph(): append custom CallbackPass lambdas, remove
// or reorder built-ins, or clear() and rebuild the frame from scratch.
void Renderer::setupDefaultRenderGraph() {
    renderGraph.clear();

    // Geometry prep + lighting passes (raytraced passes only run on backends
    // that report RHICapabilities::raytracing — i.e. Metal today).
    // IBL-from-sky capture (runs once, refreshed via iblNeedsUpdate).
    renderGraph.addPass("IBLCapture",
        [](Renderer& r) { r.iblCapturePass(); });
    renderGraph.addPass("IBLPreview",
        [](Renderer& r) { r.iblPreviewPass(); });

    renderGraph.addPass("BuildAccelStructures",
        [](Renderer& r) { r.buildAccelerationStructures(); }, PassFlags::RequiresRaytracing);
    // Frustum-only cull that feeds the GPU-driven (indirect) pre-pass. No-op
    // unless a GPU-driven MDI layout is active; the pass early-outs otherwise.
    // Must run before PrePass (which consumes its args) and can't use Hi-Z
    // occlusion (the Hi-Z is built from the pre-pass depth that doesn't exist yet).
    renderGraph.addPass("PreCull",
        [](Renderer& r) { r.prePassCullPass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("PrePass",
        [](Renderer& r) { r.prePass(); });
    // Camera-motion velocity from the pre-pass depth — consumed by the RT
    // temporal denoisers below (and future TAA).
    renderGraph.addPass("Velocity",
        [](Renderer& r) { r.velocityPass(); });
    renderGraph.addPass("NormalResolve",
        [](Renderer& r) { r.normalResolvePass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("TileCulling",
        [](Renderer& r) { r.tileCullingPass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("RaytraceShadow",
        [](Renderer& r) { r.raytraceShadowPass(); }, PassFlags::RequiresRaytracing);
    // AO chain runs on BOTH backends now: the raygen stage picks RT AO or SSAO
    // by aoMethod/capability (Metal compute; Vulkan fullscreen fragment twins),
    // so the passes gate on compute, not raytracing. Internal pipeline-validity
    // guards keep them no-ops where the pipelines don't exist.
    renderGraph.addPass("RaytraceAO",
        [](Renderer& r) { r.raytraceAOPass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("AOTemporal",
        [](Renderer& r) { r.aoTemporalPass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("AODenoise",
        [](Renderer& r) { r.aoDenoisePass(); }, PassFlags::RequiresCompute);
    renderGraph.addPass("StochasticShadows",
        [](Renderer& r) { r.stochasticShadowPass(); }, PassFlags::RequiresRaytracing);
    renderGraph.addPass("StochasticShadowTemporal",
        [](Renderer& r) { r.stochasticShadowTemporalPass(); }, PassFlags::RequiresRaytracing);
    // Edge-aware spatial filter over the accumulated result (reads post-swap
    // history, writes the display copy the PBR samples) — must follow Temporal.
    renderGraph.addPass("StochasticShadowDenoise",
        [](Renderer& r) { r.stochasticShadowDenoisePass(); }, PassFlags::RequiresRaytracing);
    // GIBS surfel GI (generation -> hash -> RT -> temporal -> gather).
    renderGraph.addPass("GIBS",
        [](Renderer& r) { r.gibsPass(); }, PassFlags::RequiresRaytracing);
    // Mirror reflections trace against the TLAS and shade hits from the GIBS
    // surfel cache — must run after GIBS so this frame's surfel state is live.
    renderGraph.addPass("RaytraceReflection",
        [](Renderer& r) { r.raytraceReflectionPass(); }, PassFlags::RequiresRaytracing);

    // RT refractions (KHR_materials_transmission): same TLAS + surfel-cache
    // shading as reflections, refracted ray (IOR 1.5). Skipped while no scene
    // material transmits.
    renderGraph.addPass("RaytraceRefraction",
        [](Renderer& r) { r.raytraceRefractionPass(); }, PassFlags::RequiresRaytracing);

    // Main geometry pass: renders to colorRT when a post-process pipeline
    // Directional shadow depth (single cascade) before the main lighting pass.
    renderGraph.addPass("Shadow",
        [](Renderer& r) { r.shadowPass(); });

    // Hi-Z depth pyramid from the PrePass depth (feeds occlusion culling below).
    // No-op unless both a GPU-driven mode and gpuOcclusionCulling are set. Must
    // sit after PrePass (needs its depth) and before GpuCull (samples the Hi-Z).
    renderGraph.addPass("HiZBuild",
        [](Renderer& r) { r.hizBuildPass(); });

    // GPU-driven frustum (+ optional Hi-Z occlusion) cull -> indirect draw args.
    // No-op unless the Indirect GPU-driven mode is active; the pass early-outs otherwise.
    renderGraph.addPass("GpuCull",
        [](Renderer& r) { r.gpuCullPass(); }, PassFlags::RequiresCompute);

    // Screen-space contact shadows (Metal compute / Vulkan fullscreen frag).
    // Reads the pre-pass depth, so it runs after depth is available and before
    // Main consumes the result.
    renderGraph.addPass("SSCS",
        [](Renderer& r) { r.sscsPass(); });

    // exists, directly to the swapchain otherwise (decided inside the pass).
    renderGraph.addPass("Main",
        [](Renderer& r) { r.mainRenderPass(); });

    // Sky/atmosphere fills the background (depth == far) before bloom, so the
    // bright sky and sun disk feed the bloom pyramid.
    renderGraph.addPass("SkyAtmosphere",
        [](Renderer& r) { r.skyAtmospherePass(); });

    // GPU particles (simulate + instanced billboards) into colorRT, after sky
    // so they composite over it and get fogged like the rest of the scene.
    renderGraph.addPass("Particles",
        [](Renderer& r) { r.particlePass(); });

    // Cheap analytic height fog before bloom (so the fogged scene feeds bloom/
    // god rays); swaps colorRT with tempColorRT internally. On by default.
    renderGraph.addPass("HeightFog",
        [](Renderer& r) { r.heightFogPass(); });

    // Expensive per-light volumetric fog (opt-in, ECS-driven); same colorRT/
    // tempColorRT swap, runs after the cheap height fog so both can compose.
    renderGraph.addPass("VolumetricFog",
        [](Renderer& r) { r.volumetricFogPass(); });

    // Heterogeneous volume raymarch (EmberGen density grids) right after the
    // global fog, same colorRT/tempColorRT swap. Off by default until the
    // panel enables it (test grid) or real volume data is set.
    renderGraph.addPass("VolumeRaymarch",
        [](Renderer& r) { r.volumeRaymarchPass(); });

    // Volumetric clouds (quarter-res raymarch + temporal + composite), after
    // fog and before bloom, matching the Metal graph. Off by default.
    renderGraph.addPass("VolumetricClouds",
        [](Renderer& r) { r.volumetricCloudPass(); });

    // God rays (screen-space light scattering), composited in PostProcess.
    // Runs before the canvas passes (native order) so HUD sprites never feed
    // the scattering, but before bloom below so the scene result is ready.
    renderGraph.addPass("LightScattering",
        [](Renderer& r) { r.lightScatteringPass(); });

    // World-space batch quads (3D canvas) into the HDR scene, depth-tested
    // against the scene, after sky/fog/scattering — native WorldCanvasPass.
    renderGraph.addPass("WorldCanvas",
        [](Renderer& r) {
            if (r.batch3D.quadCount == 0 || !r.colorRT.isValid() || !r.depthStencilRT.isValid()) return;
            RenderPassDesc rp;
            rp.name = "WorldCanvas";
            rp.colorAttachments.push_back(r.colorRT);
            rp.loadColor.push_back(true);
            rp.depthAttachment = r.depthStencilRT;
            rp.loadDepth = true;
            r.rhi->beginRenderPass(rp);
            r.flush3D();
            r.rhi->endRenderPass();
        });

    // 2D screen-space canvas (HUD sprites/quads) into the HDR scene, before
    // bloom — native CanvasPass: sprites get bloom/tonemap/post effects, and
    // the sky can't overwrite them because it already rendered. Depth is
    // attached only to satisfy the pipeline's format contract (test/write
    // are disabled on the 2D pipeline).
    renderGraph.addPass("Canvas2D",
        [](Renderer& r) {
            if (r.batch2D.quadCount == 0 || !r.colorRT.isValid() || !r.depthStencilRT.isValid()) return;
            RenderPassDesc rp;
            rp.name = "Canvas2D";
            rp.colorAttachments.push_back(r.colorRT);
            rp.loadColor.push_back(true);
            rp.depthAttachment = r.depthStencilRT;
            rp.loadDepth = true;
            r.rhi->beginRenderPass(rp);
            r.flush2D();
            r.rhi->endRenderPass();
        });

    // Pyramid bloom: brightness extract -> downsample chain -> tent-filter
    // upsample chain (accumulates into pyramid[0]); composited in PostProcess.
    // No-ops until the bloom pipelines/targets exist.
    renderGraph.addPass("BloomBrightness",
        [](Renderer& r) { r.bloomBrightnessPass(); });
    renderGraph.addPass("BloomDownsample",
        [](Renderer& r) { r.bloomDownsamplePass(); });
    renderGraph.addPass("BloomUpsample",
        [](Renderer& r) { r.bloomUpsamplePass(); });

    // Sun/lens flare, additive over the HDR scene (off by default, like native).
    renderGraph.addPass("SunFlare",
        [](Renderer& r) { r.sunFlarePass(); });

    // Fullscreen post-process to swapchain; no-op until a post-process
    // pipeline is created.
    renderGraph.addPass("PostProcess",
        [](Renderer& r) { r.postProcessPass(); });

    // RmlUI overlay onto the swapchain (no-op until initUI() succeeds).
    // ImGui renders after the graph, in endFrame().
    renderGraph.addPass("RmlUi",
        [](Renderer& r) { r.renderUI(); });
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
    // Populate the camera's frustum planes so the GPU cull pass can test bounds
    // against them (object cull in Indirect mode).
    // Only when a GPU-driven mode is active, to keep the default path's camera
    // data byte-identical.
    if (gpuDrivenActive()) {
        Frustum f = extractFrustum(currentCamera.proj * currentCamera.view);
        for (int i = 0; i < 6; ++i) {
            currentCamera.frustumPlanes[i] = f.planes[i];
        }
    }

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
            // Foliage-style cutout: only MASK materials carry a cutoff; 0
            // disables the shader-side discard for OPAQUE/BLEND.
            data.alphaCutoff = mat.alphaMode == AlphaMode::MASK ? mat.alphaCutoff : 0.0f;
            data.emissiveStrength = mat.emissiveStrength;
            data.subsurface = mat.subsurface;
            data.specular = mat.specular;
            data.specularTint = mat.specularTint;
            data.anisotropic = mat.anisotropic;
            data.sheen = mat.sheen;
            data.sheenTint = mat.sheenTint;
            data.clearcoat = mat.clearcoat;
            data.clearcoatGloss = mat.clearcoatGloss;
            data.iblEnabled = mat.useIBL ? 1.0f : 0.0f;  // panel "Use IBL"
            data.transmission = mat.transmission;
            materialDataArray.push_back(data);
        }
        rhi->updateBuffer(materialUniformBuffer, materialDataArray.data(), 0,
                          materialDataArray.size() * sizeof(Vapor::MaterialData));
        // Gates the RT refraction pass: skip the trace entirely while nothing
        // in the scene transmits (the panel slider can flip this any frame).
        sceneHasTransmission = std::any_of(materials.begin(), materials.end(),
            [](const RenderMaterial& m) { return m.transmission > 0.0f; });
    }

    // The directional light IS the sun: derive the atmosphere/sky sun direction
    // from it so the sky disc, god rays, fog, GIBS and clouds all agree with the
    // surface shading and the PSSM shadows (which read the light directly).
    // Conventions differ — DirectionalLight.direction points the way light
    // TRAVELS (away from the sun), atmosphere sunDirection points TOWARD the sun
    // — so negate. Without this the two were independent and started misaligned,
    // making the lit direction and the sky sun disagree (and, on Metal, the RT
    // sun shadow followed a different sun than the Vulkan PSSM shadow). Scenes
    // with no directional light keep the panel-set sun.
    if (!directionalLights.empty()) {
        glm::vec3 d = directionalLights[0].direction;
        if (glm::dot(d, d) > 1e-8f) atmosphereData.sunDirection = -glm::normalize(d);
    }
    // Atmosphere tunables are panel-editable; re-upload every frame so edits
    // reach the sky/fog/IBL consumers (the buffer is tiny; staged-ring cost
    // is negligible, and it keeps every frame slot coherent).
    if (atmosphereDataBuffer.isValid()) {
        rhi->updateBuffer(atmosphereDataBuffer, &atmosphereData, 0, sizeof(atmosphereData));
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

    // Update spot lights
    if (!spotLights.empty()) {
        rhi->updateBuffer(spotLightBuffer, spotLights.data(), 0,
                          spotLights.size() * sizeof(Vapor::SpotLight));
    }

    // Update instance data — for EVERY submitted drawable, not just the
    // camera-visible set. Shadow consumers (PSSM cascade draws, the TLAS the
    // RT kernels trace) must include casters OUTSIDE the camera frustum, or
    // objects lose their shadows the moment they leave the screen. Visible
    // drawables are packed FIRST (ids 0..V-1) so the main/pre-pass indexing
    // and buffer-range binds keep their existing meaning; culled drawables'
    // instances follow after.
    drawableToInstanceID.clear();
    std::vector<Vapor::InstanceData> instanceData;
    instanceData.reserve(frameDrawables.size());
    Uint32 instanceID = 0;

    // MDI mode (Vulkan): instances address the merged scene buffers, so they need
    // real per-mesh offsets and must be grouped into contiguous per-material
    // ranges. Any other mode uses per-mesh buffers -> offsets stay 0.
    // Bindless MDI shares the MDI instance layout (merged offsets, material-
    // sorted) — its draws index the merged buffers the same way. Availability:
    // Metal needs ICB support (GPU-encoded commands), Vulkan needs native MDI +
    // descriptor indexing (one vkCmdDrawIndexedIndirect + set-3 texture array).
    const bool bindlessMDI = gpuDrivenBindless() &&
                             capabilities.bindlessTextures &&
                             (backend == GraphicsBackend::Metal
                                  ? capabilities.indirectCommandBuffers
                                  : capabilities.multiDrawIndirect);
    const bool mdi = mdiLayoutActive();  // == bindlessMDI || plain-MDI (see helper)
    m_mdiInstanceLayout = mdi;  // pre-pass/shadow must match this layout (Metal)
    // MDI addresses the merged vertex buffer, so build it (self-gates on
    // m_mergedGeometryDirty).
    if (mdi) ensureMergedGeometry();
    // Bindless material table for the Bindless MDI mode (self-gates on dirty + caps).
    if (bindlessMDI) ensureBindlessMaterialTable();
    m_materialRanges.clear();

    auto appendInstance = [&](Uint32 drawableIdx) {
        const Drawable& drawable = frameDrawables[drawableIdx];
        const RenderMesh& mesh = meshes[drawable.mesh];

        Vapor::InstanceData instance;
        instance.model = drawable.transform;
        instance.color = drawable.color;
        instance.vertexOffset = mdi ? mesh.vertexOffset : 0;  // into merged buffer (MDI) or 0 (per-mesh)
        instance.indexOffset = mdi ? mesh.indexOffset : 0;
        instance.rtVertexOffset = mesh.vertexOffset;  // always valid: RT hit shading
        instance.rtIndexOffset = mesh.indexOffset;    //   fetches merged geometry
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
    };
    if (mdi) {
        // Submit ALL drawables, sorted by material so each material occupies a
        // contiguous instance range -> one drawIndexedIndirect per material.
        std::vector<Uint32> order(frameDrawables.size());
        for (Uint32 i = 0; i < frameDrawables.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [this](Uint32 a, Uint32 b) {
            return frameDrawables[a].material < frameDrawables[b].material;
        });
        MaterialId curMat = UINT32_MAX;
        Uint32 rangeStart = 0;
        for (Uint32 idx : order) {
            if (instanceID >= MAX_INSTANCES) break;
            MaterialId m = frameDrawables[idx].material;
            if (m != curMat) {
                if (curMat != UINT32_MAX)
                    m_materialRanges.push_back({curMat, {rangeStart, instanceID - rangeStart}});
                curMat = m;
                rangeStart = instanceID;
            }
            appendInstance(idx);
        }
        if (curMat != UINT32_MAX)
            m_materialRanges.push_back({curMat, {rangeStart, instanceID - rangeStart}});
    } else {
        for (Uint32 drawableIdx : visibleDrawables) appendInstance(drawableIdx);
        for (Uint32 i = 0; i < frameDrawables.size() && instanceID < MAX_INSTANCES; i++) {
            if (drawableToInstanceID.find(i) == drawableToInstanceID.end()) appendInstance(i);
        }
    }
    totalInstanceCount = instanceID;

    if (!instanceData.empty()) {
        rhi->updateBuffer(instanceDataBuffer, instanceData.data(), 0,
                          instanceData.size() * sizeof(Vapor::InstanceData));
    }

    // (No CPU-side cluster upload: the TileCulling compute pass produces the
    // whole cluster buffer on the GPU every frame — like the native renderer,
    // which never touches cluster data from the CPU. The old "fill every tile
    // with every light" path here predated that compute port; it uploaded a
    // ~6MB buffer whenever the light count changed and only fought the GPU
    // cull that immediately overwrote it.)

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
    // Early-Z from the pre-pass: when rendering into colorRT we share the
    // pre-pass's depthStencilRT, so LOAD its depth (LoadActionLoad) instead of
    // re-clearing. Paired with the main pipeline's CompareOp::LessOrEqual, the
    // occluded fragments the pre-pass already resolved are rejected before the
    // (expensive) PBR fragment shader runs — no overdraw, matching native's
    // LoadActionLoad + CompareFunctionLessEqual. The swapchain-fallback path
    // uses swapchainDepthBuffer, which the pre-pass never wrote, so it must
    // still clear.
    renderPassDesc.loadDepth = usePostProcess;

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
    // Binding 2: InstanceData array. Range = ALL uploaded instances, not just
    // the CPU-visible prefix: the GPU-driven paths (Indirect per-object + MDI)
    // index instances[] up to totalInstanceCount, and a visible-only range makes
    // those reads out-of-bounds on Vulkan (updateBuffers uploads every drawable).
    rhi->setVertexBuffer(2, instanceDataBuffer, 0,
                         sizeof(Vapor::InstanceData) * std::max<Uint32>(1, totalInstanceCount));

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
    // Materials for the Metal PBR fragment's per-fragment fetch (buffer 11).
    // The shader reads materials[materialID] instead of taking the material
    // through inter-stage (Metal per-vertex-output overflow at 112 bytes). This
    // no-ops on Vulkan (binding 11 >= BINDINGS_PER_SET; RHIMain.frag reads
    // materials from set0 b1 in the fragment already).
    rhi->setFragmentBuffer(19, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);

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
    Uint32 gibsOn = (gibsEnabled && capabilities.raytracing && giResultTexture.isValid()) ? 1u : 0u;
    rhi->setFragmentBytes(&gibsOn, sizeof(Uint32), 10);

    // Directional-light count for RHIMain.frag's dir loop (it supports N
    // directional lights; Metal's shader hardcodes the single sun and declares
    // no buffer(11), so this write is inert there). This used to be a uvec2
    // whose .y carried the point-light count — dead since the tile-cull port
    // (the point loop reads per-cluster counts), so push just the one uint.
    // Binding 11 -> Vulkan push-constant offset 64 + (11%4)*16 = 112.
    Uint32 dirLightCount = static_cast<Uint32>(directionalLights.size());
    rhi->setFragmentBytes(&dirLightCount, sizeof(Uint32), 11);

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

    if (backend == GraphicsBackend::Vulkan) {
        // PSSM cascaded shadow (Vulkan binding budget is 8 slots/set, so the
        // Metal contract slots 9/12 above are no-ops here): cascade data at
        // set1 b2, cascade depth array at set2 b6. Vulkan ONLY — on Metal,
        // fragment buffer(2) is the CLUSTER buffer: rebinding PSSM data there
        // made the PBR shader read shadow matrices as tile light counts
        // (billions of loop iterations per pixel -> GPU hang in the Main pass,
        // machine-freezing on repeat).
        if (pssmDataBuffer.isValid()) rhi->setFragmentBuffer(2, pssmDataBuffer, 0, sizeof(PSSMRenderData));
        // RHIMain.frag reads the cascade array at set2 b6. (Metal's PBR shader
        // uses slot 6 for texAO — don't clobber it there; the cascade array is
        // already bound at the Metal contract slot 12 above.)
        if (pssmShadowArrayTexture.isValid()) rhi->setTexture(0, 6, pssmShadowArrayTexture, shadowSampler);
        // Tiled point-light culling inputs (set1 b4 clusters, b5 dimensions).
        if (clusterBuffer.isValid()) rhi->setFragmentBuffer(4, clusterBuffer);
        if (lightCullDataBuffer.isValid()) rhi->setFragmentBuffer(5, lightCullDataBuffer, 0, sizeof(Vapor::LightCullData));
        // SSAO chain output at set2 b7 (RHIMain.frag texAO; whiteTex = neutral
        // when AO is off — the defaults loop above already bound white there).
        if (aoEnabled && aoRT.isValid()) rhi->setTexture(0, 7, aoRT, clampSampler);
        // Screen-space contact shadow at set2 b8 (RHIMain.frag sscsTex; whiteTex
        // = neutral/lit when disabled). Uses the bumped 10-binding texture set.
        rhi->setTexture(0, 8, (sscsEnabled && sscsRT.isValid()) ? sscsRT : whiteTex, clampSampler);
        // Independent near-field shadow map at set2 b9 (RHIMain.frag nearShadowTex).
        // Always bind a 2D texture here: the defaults loop above put a cubemap in
        // b8/b9 (dead Metal contract), and b8/b9 are sampler2D on this path.
        rhi->setTexture(0, 9, nearShadowMap.isValid() ? nearShadowMap : whiteTex, shadowSampler);
        // IBL maps (set2 b10/b11/b12): the sky bake fills them on Vulkan now.
        // Sampled only when the material's iblEnabled is set; bind always so the
        // descriptor is valid whenever a material does enable it.
        if (irradianceMap.isValid()) rhi->setTexture(0, 10, irradianceMap, clampSampler);
        if (prefilterMap.isValid()) rhi->setTexture(0, 11, prefilterMap, clampSampler);
        if (brdfLUTTex.isValid())    rhi->setTexture(0, 12, brdfLUTTex, clampSampler);
        // Perf-isolation debug flags -> RHIMain.frag push offset 96 (binding 2).
        // Vulkan-only: on Metal, fragment buffer(2) is the CLUSTER buffer, so
        // pushing here would corrupt it (the old billions-of-iterations hang).
        rhi->setFragmentBytes(&mainDebugFlags, sizeof(Uint32), 2);
        // Spot lights: buffer at set1 b6; rect area lights at set1 b7 (analytic
        // eval in RHIMain.frag, unshadowed — the RT area shadow is Metal-only).
        // Loop bounds travel together at push offset 80 (binding 1).
        // (IBL maps already bound at b10/11/12 above.)
        if (spotLightBuffer.isValid())
            rhi->setFragmentBuffer(6, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
        if (rectLightBuffer.isValid()) rhi->setFragmentBuffer(7, rectLightBuffer);
        glm::uvec2 spotRectCounts(static_cast<Uint32>(spotLights.size()),
                                  static_cast<Uint32>(rectLights.size()));
        rhi->setFragmentBytes(&spotRectCounts, sizeof(glm::uvec2), 1);
    } else {
        // Perf-isolation debug flags for the Metal PBR shader at buffer(12)
        // (buffer(2) is the cluster buffer here — see the Vulkan note above —
        // and buffer(11) is dirLightCount, so 12 is the free slot). Same bits as
        // the Vulkan path: bit0 skip point-light loop, bit1 skip shadow.
        rhi->setFragmentBytes(&mainDebugFlags, sizeof(Uint32), 12);
        // Metal-via-RHI: real IBL outputs replace the neutral blacks —
        // irradiance(8), prefilter(9), brdfLUT(10).
        if (m_iblReady) {
            if (irradianceMap.isValid()) rhi->setTexture(0, 8, irradianceMap, clampSampler);
            if (prefilterMap.isValid()) rhi->setTexture(0, 9, prefilterMap, clampSampler);
            if (brdfLUTTex.isValid()) rhi->setTexture(0, 10, brdfLUTTex, clampSampler);
        }
        // AO output replaces the neutral white at texAO(6) whenever the AO
        // chain ran — with RT (RTAO) or without (SSAO shares the chain).
        if (aoEnabled && aoRT.isValid()) rhi->setTexture(0, 6, aoRT, clampSampler);
        // 3d_pbr_normal_mapped.metal min()'s the sun shadow with texSSCS(15) — it
        // MUST be bound (an unbound sample reads 0 -> whole scene black). Use the
        // computed contact RT when SSCS is on, else white (min() no-op).
        rhi->setTexture(0, 15, (sscsEnabled && sscsRT.isValid()) ? sscsRT : whiteTex, clampSampler);
        if (capabilities.raytracing) {
            // RT kernel outputs replace the neutral whites —
            // texShadow(7), texPointShadow(13), gibsGI(14).
            if (shadowRT.isValid()) rhi->setTexture(0, 7, shadowRT, clampSampler);
            // texPointShadow (RGB point/rect/spot channels) only when stochastic
            // shadows are on AND the chain has produced at least one frame;
            // otherwise the neutral white bound above keeps point/rect/spot
            // unshadowed — matching the RT-less Vulkan path. Prefer the
            // edge-aware filtered copy when the denoise pass ran this frame.
            if (stochasticShadowsEnabled && stochasticShadowHistoryWritten && stochasticShadowHistoryRT.isValid()) {
                TextureHandle shadowTex = (stochasticShadowDenoiseRan && stochasticShadowDenoisedRT.isValid())
                                              ? stochasticShadowDenoisedRT : stochasticShadowHistoryRT;
                rhi->setTexture(0, 13, shadowTex, clampSampler);
            }
            if (gibsEnabled && giResultTexture.isValid()) rhi->setTexture(0, 14, giResultTexture, clampSampler);
        }
        // RT mirror reflections: texture(16) + params at buffer(17). The shader
        // samples the texture only behind the runtime x > 0.5 check (same
        // contract as gibsGI). On the bound path the result is a direct texture
        // arg (texReflectionArg@16); the bindless path reads it from the system
        // table instead (see the ICB block below). Params live at buffer(17) —
        // free on both paths (13/14 are the bindless tables, 16 is spotLights).
        bool reflOn = rtReflectionsEnabled && capabilities.raytracing && reflectionRT.isValid();
        if (reflOn) rhi->setTexture(0, 16, reflectionRT, clampSampler);
        // Spot lights (buffer 16). buffer 14 is the bindless SystemTexs table's
        // slot, so spot lights use 16 (see 3d_pbr_normal_mapped.metal).
        rhi->setFragmentBuffer(16, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
        // RT refraction result (texture 17), same runtime gate as reflection.
        bool refrOn = rtRefractionsEnabled && sceneHasTransmission &&
                      capabilities.raytracing && refractionRT.isValid();
        if (refrOn) rhi->setTexture(0, 17, refractionRT, clampSampler);
        // RT reflection/refraction composite params + the spot-count/stochastic
        // flag are read by the MSL PBR from fragment buffers 15/17/18. On Vulkan
        // setFragmentBytes(N) writes push-constant offset 64+(N%4)*16, so 15/17/18
        // ALIAS dirLightCount(binding 11 -> 112), spotRectCounts(binding 1 -> 80)
        // and mainDebugFlags(binding 2 -> 96) — clobbering real lighting fields
        // (corrupt directional/spot counts -> green ghosting when RT reflection is
        // on). RHIMain.frag doesn't read these params (RT composite is Metal-only;
        // the GLSL spot count comes from spotRectCounts@80), so push them on Metal
        // only. The stochastic-shadow flag bit0 says whether the point-shadow
        // texture carries the RGB channel format (R point / G rect / B spot).
        if (backend == GraphicsBackend::Metal) {
            glm::vec2  reflParams(reflOn ? 1.0f : 0.0f, rtReflectionIntensity);
            glm::uvec2 spotRectParams(static_cast<Uint32>(spotLights.size()),
                                      (capabilities.raytracing && stochasticShadowsEnabled) ? 1u : 0u);
            glm::vec2  refrParams(refrOn ? 1.0f : 0.0f, rtRefractionIntensity);
            rhi->setFragmentBytes(&reflParams, sizeof(glm::vec2), 17);
            rhi->setFragmentBytes(&spotRectParams, sizeof(glm::uvec2), 15);
            rhi->setFragmentBytes(&refrParams, sizeof(glm::vec2), 18);
        }
    }

    // GPU-driven path is used only when enabled AND the cull pipeline/args
    // buffer actually exist — otherwise fall back to the CPU path entirely
    // (avoids drawing from an unwritten indirect args buffer).
    const bool useGpuDriven = gpuDrivenIndirect() && gpuCullPipeline.isValid() &&
                              gpuCullArgsBuffer.isValid();

    // True single-call multi-draw indirect (Vulkan): bind the merged scene
    // buffers once, then issue one drawIndexedIndirect per material range (each
    // spanning many meshes). The cull pass wrote per-instance commands with real
    // merged-buffer offsets; instanceCount 0 makes culled instances no-ops. The
    // instance index comes from gl_InstanceIndex (= command.firstInstance), so
    // the push-constant instanceID is 0.
    // MDI works on Vulkan when the multiDrawIndirect feature is available (native
    // multi-draw), and on Metal via the RHI's per-command loop (correct, though
    // single-call there needs an Indirect Command Buffer — a Metal-internal
    // follow-up). Both backends share this renderer path.
    const bool mdiBackendOk = (backend == GraphicsBackend::Vulkan && capabilities.multiDrawIndirect) ||
                              backend == GraphicsBackend::Metal;
    const bool useMDI = useGpuDriven && gpuDrivenMDI && mdiBackendOk &&
                        mergedVertexBuffer.isValid() && mergedIndexBuffer.isValid() &&
                        !m_materialRanges.empty();

    // Bindless MDI: the whole scene in ONE submission on the bindless pipeline
    // (material textures fetched by materialID — no per-material loop).
    //   Metal:  replay the GPU-encoded sceneICB with executeCommandsInBuffer.
    //   Vulkan: one native vkCmdDrawIndexedIndirect over every instance,
    //           reading the same cull-written args buffer as plain MDI.
    // Takes precedence over plain MDI; falls back to the paths below when
    // anything is missing (pipeline compile failed, caps absent...).
    const bool bindlessBackendOk = backend == GraphicsBackend::Metal
        ? (capabilities.indirectCommandBuffers && sceneICB.isValid())
        : (capabilities.multiDrawIndirect && mergedIndexBuffer.isValid());
    const bool useBindless = useGpuDriven && gpuDrivenBindless() &&
                             capabilities.bindlessTextures && bindlessBackendOk &&
                             mainPipelineBindless.isValid() &&
                             bindlessMaterialTable.isValid() && mergedVertexBuffer.isValid() &&
                             totalInstanceCount > 0;

    // Count the geometry submissions issued below so the debug panel can show
    // which path actually ran (see FrameStats::mainDrawCalls / mainPath).
    Uint32 mainDrawCalls = 0;
    lastFrameStats.mainPath = useBindless ? "BindlessMDI" : useMDI ? "MDI"
                            : useGpuDriven ? "Indirect" : "CPU";
    if (useBindless) {
        // Binding state set for mainPipeline above (camera/instances/lights/
        // system textures) carries over. Only the pipeline (bindless fragment),
        // the merged buffers, and the material table differ from the normal path.
        rhi->bindPipeline(mainPipelineBindless);
        rhi->bindVertexBuffer(mergedVertexBuffer, 3, 0);
        Uint32 zeroId = 0;
        // Instance index rides base_instance (Metal) / gl_InstanceIndex (Vulkan).
        rhi->setVertexBytes(&zeroId, sizeof(Uint32), 4);
        rhi->bindTextureArgumentTable(bindlessMaterialTable);
        if (backend == GraphicsBackend::Metal) {
            // ICB pipelines reject direct fragment texture arguments, so the 10
            // system textures (Metal contract slots 6-15) also go through an
            // argument table. Resolve the same values the bound path binds
            // above (KEEP IN SYNC with the Metal else-branch of the fragment
            // contract), rewrite only changed slots (the table is shared with
            // in-flight frames), and bind it at buffer(14).
            if (!bindlessSystemTable.isValid()) {
                bindlessSystemTable = rhi->createTextureArgumentTable(
                    fragmentShaderBindless, /*bufferIndex=*/14, 1, /*texturesPerEntry=*/12);
            }
            if (bindlessSystemTable.isValid()) {
                TextureHandle whiteTex = textures[defaultWhiteTexture].handle;
                TextureHandle blackTex = textures[defaultBlackTexture].handle;
                const bool iblReady = m_iblReady;
                bool reflOn = rtReflectionsEnabled && capabilities.raytracing && reflectionRT.isValid();
                bool refrOn = rtRefractionsEnabled && sceneHasTransmission &&
                              capabilities.raytracing && refractionRT.isValid();
                const TextureHandle sys[12] = {
                    (aoEnabled && aoRT.isValid()) ? aoRT : whiteTex,                     // 0 texAO
                    (capabilities.raytracing && shadowRT.isValid()) ? shadowRT : whiteTex, // 1 texShadow
                    (iblReady && irradianceMap.isValid()) ? irradianceMap : defaultBlackCubemapTex, // 2
                    (iblReady && prefilterMap.isValid()) ? prefilterMap : defaultBlackCubemapTex,   // 3
                    (iblReady && brdfLUTTex.isValid()) ? brdfLUTTex : blackTex,          // 4 brdfLUT
                    whiteTex,                                                            // 5 rectLightVideo
                    pssmShadowArrayTexture,                                              // 6 pssmShadowMaps
                    (capabilities.raytracing && stochasticShadowsEnabled && stochasticShadowHistoryWritten &&
                     stochasticShadowHistoryRT.isValid())
                        ? ((stochasticShadowDenoiseRan && stochasticShadowDenoisedRT.isValid())
                               ? stochasticShadowDenoisedRT : stochasticShadowHistoryRT)
                        : whiteTex, // 7 texPointShadow (denoised copy when available)
                    (capabilities.raytracing && gibsEnabled && giResultTexture.isValid()) ? giResultTexture : blackTex, // 8
                    (sscsEnabled && sscsRT.isValid()) ? sscsRT : whiteTex,               // 9 texSSCS
                    reflOn ? reflectionRT : blackTex,                                    // 10 texReflection
                    refrOn ? refractionRT : blackTex,                                    // 11 texRefraction
                };
                for (Uint32 i = 0; i < 12; ++i) {
                    if (sys[i].id != m_bindlessSysCache[i].id) {
                        rhi->writeTextureArgumentTable(bindlessSystemTable, 0, i, sys[i]);
                        m_bindlessSysCache[i] = sys[i];
                    }
                }
                rhi->bindTextureArgumentTable(bindlessSystemTable);
                // RT composite params (buffer 17/18) — same runtime gate as the
                // bound path; the shader reads texReflection/texRefraction from
                // the system table above on this ICB path.
                glm::vec2 reflParams(reflOn ? 1.0f : 0.0f, rtReflectionIntensity);
                rhi->setFragmentBytes(&reflParams, sizeof(glm::vec2), 17);
                glm::vec2 refrParams(refrOn ? 1.0f : 0.0f, rtRefractionIntensity);
                rhi->setFragmentBytes(&refrParams, sizeof(glm::vec2), 18);
            }
            // Replay the GPU-encoded command buffer (commands carry their own
            // index-buffer regions).
            rhi->executeICB(sceneICB, totalInstanceCount);
        } else {
            // One native multi-draw over the cull-written args, all materials.
            rhi->bindIndexBuffer(mergedIndexBuffer, 0);
            rhi->drawIndexedIndirect(gpuCullArgsBuffer, 0, totalInstanceCount,
                                     sizeof(Vapor::DrawCommand));
        }
        mainDrawCalls = 1;  // the whole scene in one submission
    } else if (useMDI) {
        // Cull-output diagnostics moved to the StatsLog "GPUDRV" source (covers
        // every indirect mode, only reads back when --stats is enabled).
        rhi->bindVertexBuffer(mergedVertexBuffer, 3, 0);
        rhi->bindIndexBuffer(mergedIndexBuffer, 0);
        Uint32 zeroId = 0;
        rhi->setVertexBytes(&zeroId, sizeof(Uint32), 4);
        for (const auto& [materialId, range] : m_materialRanges) {
            if (materialId < materials.size()) bindMaterial(materialId);
            rhi->drawIndexedIndirect(gpuCullArgsBuffer,
                                     static_cast<size_t>(range.first) * sizeof(Vapor::DrawCommand),
                                     range.second, sizeof(Vapor::DrawCommand));
            mainDrawCalls++;  // one native multi-draw per material range
        }
    } else {

    // Group drawables by material to reduce state changes.
    std::map<MaterialId, std::vector<Uint32>> materialBatches;
    if (useGpuDriven) {
        // GPU-driven: submit ALL drawables and let the compute cull decide
        // visibility per instance (via instanceCount in the indirect args),
        // rather than the CPU-culled visibleDrawables subset.
        for (Uint32 i = 0; i < frameDrawables.size(); ++i) {
            materialBatches[frameDrawables[i].material].push_back(i);
        }
    } else {
        for (Uint32 drawableIdx : visibleDrawables) {
            const Drawable& drawable = frameDrawables[drawableIdx];
            materialBatches[drawable.material].push_back(drawableIdx);
        }
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

            // Set instance ID. On Vulkan the vertex shader reads
            // instances[instanceID + gl_InstanceIndex]; in GPU-driven mode the
            // draw command's firstInstance carries the index (gl_InstanceIndex),
            // so push 0. Metal keeps using the push constant directly.
            // In GPU-driven mode the index comes from gl_InstanceIndex (Vulkan) /
            // base_instance (Metal), carried by the draw command's firstInstance,
            // so push 0. The CPU path pushes the real index.
            Uint32 vsInstanceID = useGpuDriven ? 0u : correctInstanceID;
            rhi->setVertexBytes(&vsInstanceID, sizeof(Uint32), 4);

            // Draw
            if (mesh.indexBuffer.isValid()) {
                rhi->bindIndexBuffer(mesh.indexBuffer, 0);
                if (useGpuDriven) {
                    // Per-object indirect draw: the compute cull wrote this
                    // instance's command (instanceCount 0 => culled => GPU
                    // no-op). firstIndex/vertexOffset are 0 for per-mesh buffers,
                    // matching what the cull shader wrote. The push-constant
                    // instanceID set above still drives the vertex shader.
                    rhi->drawIndexedIndirect(gpuCullArgsBuffer,
                                             correctInstanceID * sizeof(Vapor::DrawCommand),
                                             1, sizeof(Vapor::DrawCommand));
                } else {
                    rhi->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
                }
                mainDrawCalls++;
            } else if (mesh.vertexBuffer.isValid()) {
                rhi->draw(mesh.vertexCount, 1, 0, 0);
                mainDrawCalls++;
            }
        }
    }
    }  // end !useMDI (per-object / CPU path)
    lastFrameStats.mainDrawCalls = mainDrawCalls;

    // World-space batch quads are NOT flushed here: they render in the
    // WorldCanvas pass after sky/fog/scattering (native graph order), so the
    // sky can't overwrite them and they still feed bloom + post-processing.

    // End render pass
    rhi->endRenderPass();
}

// Load an equirectangular HDR image as the IBL environment source. The actual
// cubemap bake (irradiance/prefilter/BRDF LUT) happens later in iblCapturePass.
void Renderer::loadHDRI(const std::string& path) {
    // loadHDRI logs and returns nullptr on failure — keep the sky as-is.
    auto img = AssetManager::loadHDRI(path);
    if (!img || img->floatArray.empty()) return;

    // (Re)create the RGBA32F equirect source texture and upload the float pixels.
    if (equirectHDRITexture.isValid()) rhi->destroyTexture(equirectHDRITexture);
    TextureDesc desc;
    desc.width = img->width;
    desc.height = img->height;
    desc.format = PixelFormat::RGBA32_FLOAT;
    desc.usage = TextureUsage::Sampled;
    equirectHDRITexture = rhi->createTexture(desc);
    rhi->updateTexture(equirectHDRITexture, img->floatArray.data(),
                       img->floatArray.size() * sizeof(float));

    iblSource = IBLSource::HDRI;
    iblNeedsUpdate = true;  // re-run the IBL chain from the new environment
    fmt::print("HDRI loaded: {} ({}x{})\n", path, img->width, img->height);
}

void Renderer::iblCapturePass() {
    if (!irradiancePipeline.isValid() || !prefilterPipeline.isValid() || !brdfLUTPipeline.isValid()) {
        return;
    }
    // Need an environment source: HDRI (equirect->cubemap) or the procedural sky
    // capture. Vulkan only ships the HDRI source, so it no-ops without an HDRI.
    const bool haveHDRISource = (iblSource == IBLSource::HDRI) && equirectHDRITexture.isValid() &&
                                equirectToCubemapPipeline.isValid();
    if (!haveHDRISource && !skyCapturePipeline.isValid()) return;

    // Start a new amortized bake when one is requested and we're idle. The 42
    // cube-face capture/convolve is spread one stage per frame: doing it all in a
    // single frame stalls the GPU (~a hitch), and with a moving sun that fired
    // roughly every second. Requests arriving mid-bake are picked up after it
    // finishes (iblNeedsUpdate is consumed at the start).
    if (m_iblBakeStage < 0) {
        if (!iblNeedsUpdate) return;

        // Fill all 42 capture slots up front into the stable capture buffer;
        // every amortized stage reads from it.
        const glm::mat4 captureViews[6] = {
            glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0),  glm::vec3(0, -1, 0)),
            glm::lookAt(glm::vec3(0), glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)),
            glm::lookAt(glm::vec3(0), glm::vec3(0, 1, 0),  glm::vec3(0, 0, 1)),
            glm::lookAt(glm::vec3(0), glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)),
            glm::lookAt(glm::vec3(0), glm::vec3(0, 0, 1),  glm::vec3(0, -1, 0)),
            glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)),
        };
        const glm::mat4 captureProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
        IBLCaptureRenderData slots[42];
        for (Uint32 f = 0; f < 6; f++) {
            slots[f].viewProj = captureProj * captureViews[f];
            slots[f].faceIndex = f;
            slots[6 + f].faceIndex = f;  // irradiance
        }
        for (Uint32 m = 0; m < PREFILTER_MIP_LEVELS; m++) {
            for (Uint32 f = 0; f < 6; f++) {
                auto& s = slots[12 + m * 6 + f];
                s.faceIndex = f;
                s.roughness = float(m) / float(PREFILTER_MIP_LEVELS - 1);
            }
        }
        rhi->updateBuffer(iblCaptureDataBuffer, slots, 0, sizeof(slots));

        m_iblBakeStage = 0;
        iblNeedsUpdate = false;  // consumed; a request during the bake re-triggers after
    }

    const size_t stride = sizeof(IBLCaptureRenderData);
    auto faceDraw = [&](const char* name, PipelineHandle pipe, TextureHandle target,
                        Uint32 slot, Uint32 face, Uint32 mip, bool bindEnv, bool bindAtmo,
                        bool bindCaptureFrag) {
        RenderPassDesc rp;
        rp.name = name;
        rp.colorAttachments.push_back(target);
        rp.clearColors.push_back(glm::vec4(0, 0, 0, 1));
        rp.loadColor.push_back(false);
        rp.colorArrayLayer = face;
        rp.colorMipLevel = mip;
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(pipe);
        rhi->setVertexBuffer(0, iblCaptureDataBuffer, slot * stride, stride);
        if (bindAtmo) rhi->setFragmentBuffer(0, atmosphereDataBuffer, 0, sizeof(AtmosphereRenderData));
        // 3d_prefilter_envmap.metal's FRAGMENT stage also reads the capture
        // slot (roughness) at buffer(0) — vertex/fragment buffer tables are
        // separate namespaces on Metal, so it needs its own bind.
        if (bindCaptureFrag) rhi->setFragmentBuffer(0, iblCaptureDataBuffer, slot * stride, stride);
        if (bindEnv) rhi->setTexture(0, 0, environmentCubemap, clampSampler);
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    };

    const bool useHDRI = (iblSource == IBLSource::HDRI) && equirectHDRITexture.isValid() &&
                         equirectToCubemapPipeline.isValid();

    // One stage per frame.
    if (m_iblBakeStage == 0) {
        // Environment capture (HDRI equirect->cube, or procedural sky) + mips.
        for (Uint32 f = 0; f < 6; f++) {
            if (useHDRI) {
                RenderPassDesc rp;
                rp.name = "EquirectToCubemap";
                rp.colorAttachments.push_back(environmentCubemap);
                rp.clearColors.push_back(glm::vec4(0, 0, 0, 1));
                rp.loadColor.push_back(false);
                rp.colorArrayLayer = f;
                rp.colorMipLevel = 0;
                rhi->beginRenderPass(rp);
                rhi->bindPipeline(equirectToCubemapPipeline);
                rhi->setVertexBuffer(0, iblCaptureDataBuffer, f * stride, stride);  // faceIndex slot
                rhi->setTexture(0, 0, equirectHDRITexture, clampSampler);
                rhi->draw(3, 1, 0, 0);
                rhi->endRenderPass();
            } else {
                faceDraw("SkyCapture", skyCapturePipeline, environmentCubemap, f, f, 0, false, true, false);
            }
        }
        rhi->generateMipmaps(environmentCubemap);
        // BRDF LUT is view-independent — bake it just once, not every rebake.
        if (!m_brdfBaked) {
            RenderPassDesc rp;
            rp.name = "BRDFLUT";
            rp.colorAttachments.push_back(brdfLUTTex);
            rp.clearColors.push_back(glm::vec4(0, 0, 0, 1));
            rp.loadColor.push_back(false);
            rhi->beginRenderPass(rp);
            rhi->bindPipeline(brdfLUTPipeline);
            rhi->draw(3, 1, 0, 0);
            rhi->endRenderPass();
            m_brdfBaked = true;
        }
    } else if (m_iblBakeStage == 1) {
        // Diffuse irradiance convolution.
        for (Uint32 f = 0; f < 6; f++)
            faceDraw("IrradianceConv", irradiancePipeline, irradianceMap, 6 + f, f, 0, true, false, false);
    } else {
        // Specular prefilter, one roughness mip per frame.
        Uint32 m = static_cast<Uint32>(m_iblBakeStage - 2);
        for (Uint32 f = 0; f < 6; f++)
            faceDraw("PrefilterEnv", prefilterPipeline, prefilterMap, 12 + m * 6 + f, f, m, true, false, true);
    }

    if (++m_iblBakeStage >= int(2 + PREFILTER_MIP_LEVELS)) {
        m_iblBakeStage = -1;  // bake complete
        m_iblReady = true;    // IBL maps are now valid to sample
    }
}

void Renderer::iblPreviewPass() {
    // IBL debug: unwrap environmentCubemap into the 2D equirect RT so ImGui can
    // show it. Cheap (one 512x256 fullscreen draw); only runs once the IBL has
    // been baked at least once.
    if (!m_iblPreviewEnabled || !m_iblReady || !iblPreviewPipeline.isValid() ||
        !iblPreviewRT.isValid() || !environmentCubemap.isValid()) {
        return;
    }
    RenderPassDesc rp;
    rp.name = "IBLPreview";
    rp.colorAttachments.push_back(iblPreviewRT);
    rp.clearColors.push_back(glm::vec4(0, 0, 0, 1));
    rp.loadColor.push_back(false);
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(iblPreviewPipeline);
    rhi->setTexture(0, 0, environmentCubemap, clampSampler);  // samplerCube set2/binding0 (Vk) / texture(0) (Metal)
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();
}

// Depth + normal (+ albedo) pre-pass. Feeds the RT shadow/AO kernels (and,
// later, SSAO/GIBS). The main pass re-clears depth and redraws — command-
// buffer order guarantees the RT passes read the pre-pass results first.
void Renderer::prePass() {
    if (!prePassPipeline.isValid() || !normalRT.isValid() || !albedoRT.isValid() ||
        !depthStencilRT.isValid()) {
        return;
    }
    RenderPassDesc rp;
    rp.name = "PrePass";
    rp.colorAttachments = { normalRT, albedoRT };
    rp.clearColors = { glm::vec4(0.0f), glm::vec4(0.0f) };
    rp.loadColor = { false, false };
    rp.depthAttachment = depthStencilRT;
    rp.loadDepth = false;
    rp.clearDepth = 1.0f;
    rhi->beginRenderPass(rp);

    rhi->bindPipeline(prePassPipeline);
    rhi->setVertexBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setVertexBuffer(1, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
    // Full instance range: in MDI mode the material-sorted instance IDs of the
    // CPU-visible drawables can land anywhere in [0, totalInstanceCount).
    rhi->setVertexBuffer(2, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * std::max<Uint32>(1, totalInstanceCount));
    // MDI layout: instances carry MERGED-buffer vertex offsets, and the Metal
    // pre-pass shader adds instances[iid].vertexOffset to vertex_id. Feed it the
    // merged vertex buffer (per-mesh index buffers stay: their local indices +
    // the merged base land on the right vertices). Binding the small per-mesh
    // buffers here instead made every fetch read far out of bounds -> garbage
    // depth over the whole screen -> the main pass's LessOrEqual rejected all
    // fragments (clear color) while rasterizing junk (FPS drop). Vulkan's
    // PrePass.vert uses fixed-function attributes and ignores vertexOffset, so
    // it keeps the per-mesh buffers.
    // Option A: GPU-driven (indirect) pre-pass. When an MDI layout is active, the
    // PreCull compute pass has written one frustum-culled DrawCommand per instance
    // into prepassCullArgsBuffer (with real merged-buffer offsets; instanceCount 0
    // = culled). Bind the merged buffers once and issue one drawIndexedIndirect per
    // material range — the GPU decides visibility, so no CPU visibleDrawables loop.
    // This mirrors the main pass's MDI branch and keeps the indirect path complete
    // (depth is produced by the same GPU decision the main pass draws from).
    // Instance index rides base_instance (Metal) / gl_InstanceIndex (Vulkan); both
    // pre-pass vertex shaders read instanceID + that, so the byte push is 0.
    const bool prePassIndirect = gpuDrivenPrePass && m_mdiInstanceLayout &&
        prepassCullArgsBuffer.isValid() &&
        mergedVertexBuffer.isValid() && mergedIndexBuffer.isValid() &&
        !m_materialRanges.empty() &&
        ((backend == GraphicsBackend::Vulkan && capabilities.multiDrawIndirect) ||
         backend == GraphicsBackend::Metal);
    if (prePassIndirect) {
        rhi->bindVertexBuffer(mergedVertexBuffer, 3, 0);
        rhi->bindIndexBuffer(mergedIndexBuffer, 0);
        Uint32 zeroId = 0;
        rhi->setVertexBytes(&zeroId, sizeof(Uint32), 4);
        for (const auto& [materialId, range] : m_materialRanges) {
            if (materialId < materials.size()) bindMaterial(materialId);  // MRT albedo/normal maps
            rhi->drawIndexedIndirect(prepassCullArgsBuffer,
                                     static_cast<size_t>(range.first) * sizeof(Vapor::DrawCommand),
                                     range.second, sizeof(Vapor::DrawCommand));
        }
        rhi->endRenderPass();
        return;
    }

    const bool prePullsMerged = backend == GraphicsBackend::Metal &&
                                m_mdiInstanceLayout && mergedVertexBuffer.isValid();
    if (prePullsMerged) rhi->bindVertexBuffer(mergedVertexBuffer, 3, 0);
    for (Uint32 drawableIdx : visibleDrawables) {
        const Drawable& drawable = frameDrawables[drawableIdx];
        const RenderMesh& mesh = meshes[drawable.mesh];
        auto it = drawableToInstanceID.find(drawableIdx);
        if (it == drawableToInstanceID.end()) continue;
        Uint32 iid = it->second;
        bindMaterial(drawable.material);  // albedo/normal maps for the MRT frag
        if (!prePullsMerged && mesh.vertexBuffer.isValid()) rhi->bindVertexBuffer(mesh.vertexBuffer, 3, 0);
        rhi->setVertexBytes(&iid, sizeof(Uint32), 4);
        if (mesh.indexBuffer.isValid()) {
            rhi->bindIndexBuffer(mesh.indexBuffer, 0);
            rhi->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
        } else if (mesh.vertexBuffer.isValid()) {
            rhi->draw(mesh.vertexCount, 1, 0, 0);
        }
    }
    rhi->endRenderPass();
}

// Rebuild the TLAS from ALL of this frame's drawables (RequiresRaytracing).
// NOT the camera-culled visible set: rays need to hit casters OUTSIDE the
// view frustum (native builds its instance list from the whole scene too) —
// building from visibleDrawables made objects lose RT shadows/AO/GI the
// moment they left the screen.
void Renderer::buildAccelerationStructures() {
    if (!capabilities.raytracing || !sceneTLAS.isValid()) return;
    tlasInstances.clear();
    tlasInstances.reserve(frameDrawables.size());
    for (Uint32 drawableIdx = 0; drawableIdx < frameDrawables.size(); drawableIdx++) {
        const Drawable& drawable = frameDrawables[drawableIdx];
        if (drawable.mesh >= meshBLAS.size() || !meshBLAS[drawable.mesh].isValid()) continue;
        auto it = drawableToInstanceID.find(drawableIdx);
        AccelStructInstance inst;
        inst.blas = meshBLAS[drawable.mesh];
        inst.transform = drawable.transform;
        inst.instanceID = it != drawableToInstanceID.end() ? it->second : 0;
        inst.mask = 0xFF;
        tlasInstances.push_back(inst);
    }
    if (!tlasInstances.empty()) {
        rhi->updateAccelerationStructure(sceneTLAS, tlasInstances);  // rebuilds
    }

    // (RT diagnostics moved to the StatsLog "RT" OnChange source.)
}

void Renderer::normalResolvePass() {
    // TODO: Implement normal resolve pass for Metal
    // For now, this is a placeholder
}

// Clustered point-light culling on both backends: the Metal branch fills the
// Metal PBR shader's cluster contract (3d_tile_light_cull.metal), the Vulkan
// branch runs TileLightCull.comp for RHIMain.frag's tiled point-light loop.
void Renderer::tileCullingPass() {
    if (!clusterBuffer.isValid()) return;
    glm::vec2 screenSize(rhi->getSwapchainWidth(), rhi->getSwapchainHeight());
    glm::uvec3 gridSize(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
    Uint32 pointLightCount = static_cast<Uint32>(pointLights.size());

    if (backend == GraphicsBackend::Metal) {
        if (!tileCullingPipeline.isValid()) return;
        rhi->beginComputePass("TileCulling");
        rhi->bindComputePipeline(tileCullingPipeline);
        rhi->setComputeBuffer(0, clusterBuffer);
        rhi->setComputeBuffer(1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
        rhi->setComputeBuffer(2, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBytes(&pointLightCount, sizeof(Uint32), 3);
        rhi->setComputeBytes(&gridSize, sizeof(glm::uvec3), 4);
        rhi->setComputeBytes(&screenSize, sizeof(glm::vec2), 5);
        rhi->dispatch(clusterGridSizeX, clusterGridSizeY, 1);
        rhi->endComputePass();
    } else {
        if (!vkTileCullPipeline.isValid() || !lightCullDataBuffer.isValid()) return;
        Vapor::LightCullData lc{};
        lc.screenSize = screenSize;
        lc.gridSize = gridSize;
        lc.lightCount = pointLightCount;
        rhi->updateBuffer(lightCullDataBuffer, &lc, 0, sizeof(lc));
        rhi->beginComputePass("TileCulling");
        rhi->bindComputePipeline(vkTileCullPipeline);
        rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBuffer(3, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
        rhi->setComputeBuffer(4, lightCullDataBuffer, 0, sizeof(Vapor::LightCullData));
        rhi->setComputeBuffer(5, clusterBuffer);
        rhi->dispatch(clusterGridSizeX, clusterGridSizeY, 1);
        rhi->endComputePass();
    }
    rhi->computeBarrier();  // cluster writes -> fragment reads in Main

    // Refresh the Light Culling Debug panel's cached histogram while it's open.
    // Throttled because sampleClusterHistogram() stalls (waitIdle). (The
    // StatsLog "CULL" source covers the --stats path independently.)
    if (lightCullDebugOpen && (frameNumber % 15) == 0) {
        sampleClusterHistogram(cullMinLights, cullAvgLights, cullMaxLights, cullNonEmptyTiles);
    }
}

// GPU-driven rendering: frustum-cull every instance in a compute pass, writing
// one indirect draw command per instance into gpuCullArgsBuffer (instanceCount
// 0 = culled). The main pass then issues per-object drawIndexedIndirect, so the
// GPU decides what actually draws. No-op unless the Indirect GPU-driven mode is
// active. The existing CPU cull path is untouched.
// Build the Hi-Z depth pyramid from the PrePass depth: mip 0 is a 2:1 max-depth
// reduction of the scene depth; each further mip max-reduces the previous. Built
// into a single sampleable texture via a scratch copy per level, because a
// render pass can't read and write the same image (see copyTexture).
void Renderer::hizBuildPass() {
    if (!gpuDrivenActive() || !gpuOcclusionCulling) return;
    if (!hizReducePipeline.isValid() || !hizTexture.isValid() ||
        !hizScratchTexture.isValid() || !depthStencilRT.isValid() || hizMipCount == 0) {
        return;
    }

    auto reduce = [&](TextureHandle src, int srcLod, Uint32 dstMip) {
        RenderPassDesc rp;
        rp.name = "HiZBuild";  // one per mip; the HUD aggregates them into one row
        rp.colorAttachments.push_back(hizTexture);
        rp.clearColors.push_back(glm::vec4(0.0f));
        rp.loadColor.push_back(false);  // full overwrite
        rp.colorMipLevel = dstMip;
        // hizTexture is multi-mip: force the single-mip subresource-view path in
        // both backends (a whole-texture multi-mip view is invalid as a color
        // attachment). layer 0 is the only layer of this 2D texture.
        rp.colorArrayLayer = 0;
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(hizReducePipeline);
        rhi->setTexture(0, 0, src, clampSampler);
        rhi->setFragmentBytes(&srcLod, sizeof(int), 0);  // source mip to reduce
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    };

    // Level 0: reduce the full-res scene depth into the half-res mip 0.
    reduce(depthStencilRT, 0, 0);
    // Levels 1..N-1: stage the previous level through the scratch (a render pass
    // can't sample hizTexture while targeting it), then reduce into this level.
    for (Uint32 mip = 1; mip < hizMipCount; mip++) {
        rhi->copyTexture(hizTexture, mip - 1, hizScratchTexture, mip - 1);
        reduce(hizScratchTexture, int(mip - 1), mip);
    }
}

// Main-pass cull: frustum + optional Hi-Z occlusion -> gpuCullArgsBuffer.
void Renderer::gpuCullPass() {
    if (!gpuDrivenIndirect()) return;
    if (!gpuCullPipeline.isValid() || !gpuCullArgsBuffer.isValid()) return;
    if (totalInstanceCount == 0) return;

    // Metal Bindless MDI: the kernel encodes real draw commands into the scene
    // ICB (created lazily here) instead of writing indirect-args structs.
    // Vulkan Bindless MDI keeps the classic args-writing cull below — its
    // single native multi-draw consumes the same args buffer.
    const bool icbMode = gpuDrivenBindless() && capabilities.indirectCommandBuffers &&
                         gpuCullICBPipeline.isValid() && mergedIndexBuffer.isValid();
    if (icbMode && !sceneICB.isValid()) {
        sceneICB = rhi->createIndirectCommandBuffer(MAX_INSTANCES);
    }

    const Uint32 n = totalInstanceCount;
    if (icbMode && sceneICB.isValid()) {
        rhi->beginComputePass("GpuCull");
        rhi->bindComputePipeline(gpuCullICBPipeline);
        rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBuffer(1, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * n);
        rhi->bindComputeICB(2, sceneICB);
        rhi->setComputeBytes(&n, sizeof(Uint32), 3);
        struct OccParams { Uint32 enabled; Uint32 mipCount; float hizW; float hizH; } occ;
        occ.enabled = (gpuOcclusionCulling && hizTexture.isValid()) ? 1u : 0u;
        occ.mipCount = hizMipCount;
        occ.hizW = float(hizWidth);
        occ.hizH = float(hizHeight);
        rhi->setComputeBytes(&occ, sizeof(occ), 4);
        rhi->setComputeBuffer(5, mergedIndexBuffer, 0, 0);
        rhi->setComputeSampledTexture(4, hizTexture, clampSampler);
        rhi->dispatch((n + 63) / 64, 1, 1);
        rhi->endComputePass();
        rhi->computeBarrier();  // ICB writes -> executeICB reads in Main
        return;
    }

    runGpuCull(gpuCullArgsBuffer, gpuOcclusionCulling);
}

// Pre-pass cull (Option A): frustum only (Hi-Z isn't built until after the
// pre-pass), so the depth pass can draw indirect instead of from the CPU-culled
// visibleDrawables. Only meaningful when the pre-pass runs GPU-driven.
void Renderer::prePassCullPass() {
    if (!gpuDrivenIndirect() || !gpuDrivenPrePass) return;
    if (!m_mdiInstanceLayout || !prepassCullArgsBuffer.isValid()) return;
    if (!gpuCullPipeline.isValid() || totalInstanceCount == 0) return;
    runGpuCull(prepassCullArgsBuffer, /*enableOcclusion=*/false);
}

// Shared cull dispatch: one DrawCommand per instance into argsBuffer, optionally
// applying the Hi-Z occlusion test (never for the pre-pass — its output is the
// depth Hi-Z is built from).
void Renderer::runGpuCull(BufferHandle argsBuffer, bool enableOcclusion) {
    const Uint32 n = totalInstanceCount;
    rhi->beginComputePass("GpuCull");
    rhi->bindComputePipeline(gpuCullPipeline);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    // Bind instance + args with instance-count-sized ranges so the GLSL shader's
    // instances.length() equals the live instance count.
    rhi->setComputeBuffer(1, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * n);
    rhi->setComputeBuffer(2, argsBuffer, 0, sizeof(Vapor::DrawCommand) * n);

    // Hi-Z occlusion inputs. Always bound (the shader declares them) so the
    // resources are valid even when occlusion is off; `enabled` gates the test.
    // hizTexture always exists; its contents are only meaningful once hizBuildPass
    // has run this frame, which happens iff occlusion is enabled.
    struct OccParams { Uint32 enabled; Uint32 mipCount; float hizW; float hizH; } occ;
    occ.enabled = (enableOcclusion && hizTexture.isValid()) ? 1u : 0u;
    occ.mipCount = hizMipCount;
    occ.hizW = float(hizWidth);
    occ.hizH = float(hizHeight);

    // The MSL kernel can't use .length() on a device pointer, so Metal takes the
    // instance count as an explicit constant at buffer(3). Binding indices differ
    // per backend: Vulkan set 2/binding 0 + push constant; Metal texture/sampler/
    // buffer index 4 (see GpuCull.comp / 3d_gpu_cull.metal).
    if (backend == GraphicsBackend::Metal) {
        rhi->setComputeBytes(&n, sizeof(Uint32), 3);
        rhi->setComputeSampledTexture(4, hizTexture, clampSampler);
        rhi->setComputeBytes(&occ, sizeof(occ), 4);
    } else {
        rhi->setComputeSampledTexture(0, hizTexture, clampSampler);
        rhi->setComputeBytes(&occ, sizeof(occ), 0);
    }
    rhi->dispatch((n + 63) / 64, 1, 1);
    rhi->endComputePass();
    rhi->computeBarrier();  // cull writes args -> indirect draw reads them
}

// Build (or rebuild, if meshes were added) the merged scene vertex/index buffers
// used by single-call MDI, from the CPU geometry accumulated in registerMesh.
void Renderer::ensureMergedGeometry() {
    if (!m_mergedGeometryDirty) return;
    if (m_mergedVertices.empty() || m_mergedIndices.empty()) return;

    if (mergedVertexBuffer.isValid()) rhi->destroyBuffer(mergedVertexBuffer);
    if (mergedIndexBuffer.isValid()) rhi->destroyBuffer(mergedIndexBuffer);

    BufferDesc vbDesc;
    vbDesc.size = m_mergedVertices.size() * sizeof(Vapor::VertexData);
    vbDesc.usage = BufferUsage::Vertex;
    vbDesc.memoryUsage = MemoryUsage::GPU;
    mergedVertexBuffer = rhi->createBuffer(vbDesc);
    rhi->updateBuffer(mergedVertexBuffer, m_mergedVertices.data(), 0, vbDesc.size);

    BufferDesc ibDesc;
    ibDesc.size = m_mergedIndices.size() * sizeof(Uint32);
    ibDesc.usage = BufferUsage::Index;
    ibDesc.memoryUsage = MemoryUsage::GPU;
    mergedIndexBuffer = rhi->createBuffer(ibDesc);
    rhi->updateBuffer(mergedIndexBuffer, m_mergedIndices.data(), 0, ibDesc.size);

    m_mergedGeometryDirty = false;
    // These are large one-time uploads recorded into the upload command stream.
    // Force them to complete now: otherwise an oversize staging buffer (retired
    // on a frame-counter timer) can be freed one frame before the upload command
    // buffer that reads it finishes (VUID-vkDestroyBuffer-buffer-00922).
    rhi->flushUploads();
}

// (Re)write the bindless material texture table for the ICB draw mode: one
// MaterialTexs entry (6 texture slots) per material, indexed by materialID in
// the bindless-specialized PBR fragment. Missing textures fall back to the
// engine defaults (white/flat-normal/black) so every slot is a valid handle —
// argument-table reads have no "unbound" concept. Self-gates on dirty + caps.
void Renderer::ensureBindlessMaterialTable() {
    if (!m_bindlessTableDirty) return;
    if (!capabilities.bindlessTextures || !fragmentShaderBindless.isValid()) return;
    if (materials.empty()) return;

    if (!bindlessMaterialTable.isValid()) {
        bindlessMaterialTable = rhi->createTextureArgumentTable(
            fragmentShaderBindless, /*bufferIndex=*/13, MAX_INSTANCES, /*texturesPerEntry=*/6);
        if (!bindlessMaterialTable.isValid()) return;
    }

    auto texOrDefault = [&](TextureId id, TextureId fallback) -> TextureHandle {
        if (id < textures.size() && textures[id].handle.isValid()) return textures[id].handle;
        return textures[fallback].handle;
    };
    const Uint32 n = std::min<Uint32>(static_cast<Uint32>(materials.size()), MAX_INSTANCES);
    for (Uint32 i = 0; i < n; ++i) {
        const RenderMaterial& m = materials[i];
        rhi->writeTextureArgumentTable(bindlessMaterialTable, i, 0, texOrDefault(m.albedoTexture,    defaultWhiteTexture));
        rhi->writeTextureArgumentTable(bindlessMaterialTable, i, 1, texOrDefault(m.normalTexture,    defaultNormalTexture));
        rhi->writeTextureArgumentTable(bindlessMaterialTable, i, 2, texOrDefault(m.metallicTexture,  defaultWhiteTexture));
        rhi->writeTextureArgumentTable(bindlessMaterialTable, i, 3, texOrDefault(m.roughnessTexture, defaultWhiteTexture));
        rhi->writeTextureArgumentTable(bindlessMaterialTable, i, 4, texOrDefault(m.occlusionTexture, defaultWhiteTexture));
        rhi->writeTextureArgumentTable(bindlessMaterialTable, i, 5, texOrDefault(m.emissiveTexture,  defaultBlackTexture));
    }
    m_bindlessTableDirty = false;
    fmt::print("Bindless material table written: {} materials\n", n);
}

// Sun/lens flare: procedural glow/halo/ghosts/starburst added over the HDR
// scene (native draws onto the pre-tonemap bloom result; here colorRT before
// PostProcess — same stage of the chain). Metal MSL only for now.
void Renderer::sunFlarePass() {
    if (!sunFlareEnabled || !sunFlarePipeline.isValid() || !colorRT.isValid() ||
        !sunFlareDataBuffer.isValid()) {
        return;
    }
    glm::vec2 screenSize(rhi->getSwapchainWidth(), rhi->getSwapchainHeight());
    glm::vec3 sunDir = glm::normalize(atmosphereData.sunDirection);
    // Fade the flare out as the sun drops below the horizon (night). sunDirection
    // points toward the sun, so its .y is the sun elevation — the same signal the
    // Atmosphere pass uses for stars/moon. ToD drives sunDirection, so this makes
    // the flare follow the day/night cycle without any extra plumbing.
    float e = glm::clamp((sunDir.y + 0.06f) / 0.12f, 0.0f, 1.0f);
    float dayFactor = e * e * (3.0f - 2.0f * e);  // smoothstep(-0.06, 0.06, sunDir.y)
    if (dayFactor <= 0.0f) return;  // sun below horizon -> no flare
    glm::vec3 sunWorldPos = currentCamera.position + sunDir * 10000.0f;
    glm::vec4 sunClip = (currentCamera.proj * currentCamera.view) * glm::vec4(sunWorldPos, 1.0f);
    if (sunClip.w <= 0.0f) return;
    glm::vec2 sunNDC = glm::vec2(sunClip) / sunClip.w;
    glm::vec2 sunScreenPos = sunNDC * 0.5f + 0.5f;
    sunScreenPos.y = 1.0f - sunScreenPos.y;

    // Local copy so the day/night scale never mutates the panel-set intensity.
    SunFlareRenderData sf = sunFlareSettings;
    sf.sunScreenPos = sunScreenPos;
    sf.aspectRatio = glm::vec2(screenSize.x / screenSize.y, 1.0f);
    sf.sunColor = atmosphereData.sunColor;
    sf.intensity = sunFlareSettings.intensity * dayFactor;
    // Occlusion + edge fade are computed in-shader (smooth depth-disk test).
    rhi->updateBuffer(sunFlareDataBuffer, &sf, 0, sizeof(sf));

    RenderPassDesc rp;
    rp.name = "SunFlare";
    rp.colorAttachments.push_back(colorRT);
    rp.loadColor.push_back(true);  // additive over the scene
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(sunFlarePipeline);
    rhi->setTexture(0, 0, depthStencilRT, clampSampler);  // occlusion depth
    rhi->setFragmentBuffer(0, sunFlareDataBuffer, 0, sizeof(SunFlareRenderData));
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();
}

// Ray-traced near-field directional shadow into shadowRT (mips regenerated for
// the PBR shader's soft lookup). Mirrors the native RaytraceShadowPass 1:1.
void Renderer::raytraceShadowPass() {
    // The independent near-field shadow map owns the near region on the Vulkan
    // path (RHIMain.frag never samples shadowRT), so tracing it here is pure
    // waste. Skip it — the near map + SSCS provide the near shadow. (The dead
    // Metal-via-RHI branch still consumes shadowRT at b7, so keep it for Metal.)
    if (backend == GraphicsBackend::Vulkan) return;
    if (!raytraceShadowPipeline.isValid() || !sceneTLAS.isValid() || !shadowRT.isValid()) return;
    // Half-res: shadowRT is half-res, the kernel derives UV from its own dims.
    Uint32 w = (rhi->getSwapchainWidth() + 1) / 2;
    Uint32 h = (rhi->getSwapchainHeight() + 1) / 2;
    glm::vec2 screenSize(w, h);
    rhi->beginComputePass("RaytraceShadow");
    rhi->bindComputePipeline(raytraceShadowPipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, shadowRT);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setComputeBuffer(1, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
    rhi->setComputeBuffer(2, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
    rhi->setComputeBytes(&screenSize, sizeof(glm::vec2), 3);
    rhi->setAccelerationStructure(4, sceneTLAS);
    // Sun soft-shadow params (buffer 5): angularRadius 0 = legacy hard shadow,
    // > 0 = cone-sampled penumbra with 4 rays/pixel (see SunShadowParams).
    struct { float angularRadius; Uint32 frameIndex; Uint32 samples; Uint32 _pad; } sunParams{
        rtSunAngularRadius, frameCounter, 4u, 0u };
    rhi->setComputeBytes(&sunParams, sizeof(sunParams), 5);
    rhi->dispatch(w, h, 1);  // native dispatches w*h groups of 1x1
    rhi->endComputePass();
    rhi->generateMipmaps(shadowRT);
}

// RT mirror reflections: one closest-hit ray per (half-res) pixel; hits shade
// from the GIBS surfel radiance cache, misses from the prefiltered env map.
// Runs AFTER the GIBS pass so the surfel/gibsData buffers hold this frame's
// state. With GIBS disabled, gibsDataBuffer is zeroed here so hit lookups
// return black and only env misses contribute (the panel says so). Mirror-only
// output is noise-free, so no temporal denoise is needed; the PBR composite
// (texture 16 / buffer 13) weights it by fresnel and fades it by roughness.
void Renderer::raytraceReflectionPass() {
    if (!rtReflectionsEnabled || !raytraceReflectionPipeline.isValid() ||
        !sceneTLAS.isValid() || !reflectionRT.isValid() ||
        !surfelBuffer.isValid() || !cellHeadBuffer.isValid() ||
        !surfelNextBuffer.isValid() || !gibsDataBuffer.isValid() ||
        !prefilterMap.isValid()) {
        return;
    }
    if (!gibsEnabled && gibsDataBuffer.isValid()) {
        GIBSData zero{};  // totalCells==0 -> surfel lookups return black
        zero.frameIndex = frameCounter;
        rhi->updateBuffer(gibsDataBuffer, &zero, 0, sizeof(zero));
    }

    Uint32 w = (rhi->getSwapchainWidth() + 1) / 2;   // reflectionRT is half-res
    Uint32 h = (rhi->getSwapchainHeight() + 1) / 2;
    const size_t surfelBytes = size_t(gibsMaxSurfels) * sizeof(Surfel);

    // Hit shading needs the merged geometry (to fetch the hit triangle's vertex
    // normals + UV — Metal's intersector exposes no built-in hit normal) and the
    // bindless material table (to sample the hit's albedo). Both are otherwise
    // built only in MDI/bindless draw modes; ensure them for RT regardless of
    // this frame's draw mode. Self-gate on dirty, so this is near-free.
    ensureMergedGeometry();
    ensureBindlessMaterialTable();
    const bool rtGeo = mergedVertexBuffer.isValid() && mergedIndexBuffer.isValid() &&
                       bindlessMaterialTable.isValid();

    struct { float rayBias; float rayMaxDistance; Uint32 frameIndex; Uint32 hasBindlessGeo; } rp{
        0.01f, 200.0f, frameCounter, rtGeo ? 1u : 0u };

    rhi->beginComputePass("RaytraceReflection");
    rhi->bindComputePipeline(raytraceReflectionPipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, reflectionRT);
    rhi->setComputeTexture(3, prefilterMap);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setComputeBuffer(1, surfelBuffer, 0, surfelBytes);
    rhi->setComputeBuffer(2, cellHeadBuffer);
    rhi->setComputeBuffer(3, surfelNextBuffer);
    rhi->setComputeBuffer(4, gibsDataBuffer, 0, sizeof(GIBSData));
    rhi->setAccelerationStructure(5, sceneTLAS);
    rhi->setComputeBytes(&rp, sizeof(rp), 6);
    // Standalone hit shading inputs: user_instance_id -> InstanceData ->
    // materialID -> base color; dirLights[0] for the sun occlusion ray.
    rhi->setComputeBuffer(7, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * MAX_INSTANCES);
    rhi->setComputeBuffer(8, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
    rhi->setComputeBuffer(9, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
    // Albedo-at-hit inputs (hasBindlessGeo): merged vertex/index buffers +
    // per-material texture argument table. Indexed by rtVertexOffset/rtIndexOffset
    // + primitive_id and materialID (see 3d_raytrace_reflection.metal).
    if (rtGeo) {
        rhi->setComputeBuffer(10, mergedVertexBuffer);
        rhi->setComputeBuffer(11, mergedIndexBuffer);
        rhi->bindComputeTextureArgumentTable(bindlessMaterialTable, 12);
    }
    rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    rhi->endComputePass();
    rhi->computeBarrier();  // reflection writes -> fragment reads in Main
}

// RT refractions: the reflection pass's structural twin with a refracted ray
// (fixed IOR 1.5, thin-walled, TIR falls back to reflect — see
// 3d_raytrace_refraction.metal). Only runs while a scene material has
// transmission > 0; the PBR composite (texture 16 / buffer 16) weights it per
// pixel by the material's transmission and fades it by roughness.
void Renderer::raytraceRefractionPass() {
    if (!rtRefractionsEnabled || !sceneHasTransmission ||
        !raytraceRefractionPipeline.isValid() ||
        !sceneTLAS.isValid() || !refractionRT.isValid() ||
        !surfelBuffer.isValid() || !cellHeadBuffer.isValid() ||
        !surfelNextBuffer.isValid() || !gibsDataBuffer.isValid() ||
        !prefilterMap.isValid()) {
        return;
    }
    // gibsDataBuffer neutralization when GIBS is off is handled by the
    // reflection pass, which always runs first in the graph; doing it again
    // here would be a redundant upload of the same zero struct.
    if (!gibsEnabled && !rtReflectionsEnabled) {
        GIBSData zero{};  // totalCells==0 -> surfel lookups return black
        zero.frameIndex = frameCounter;
        rhi->updateBuffer(gibsDataBuffer, &zero, 0, sizeof(GIBSData));
    }

    Uint32 w = (rhi->getSwapchainWidth() + 1) / 2;   // refractionRT is half-res
    Uint32 h = (rhi->getSwapchainHeight() + 1) / 2;
    const size_t surfelBytes = size_t(gibsMaxSurfels) * sizeof(Surfel);

    // See raytraceReflectionPass: ensure the merged geometry + material table
    // the hit shading fetches, independent of this frame's draw mode.
    ensureMergedGeometry();
    ensureBindlessMaterialTable();
    const bool rtGeo = mergedVertexBuffer.isValid() && mergedIndexBuffer.isValid() &&
                       bindlessMaterialTable.isValid();

    struct { float rayBias; float rayMaxDistance; Uint32 frameIndex; Uint32 hasBindlessGeo; } rp{
        0.01f, 200.0f, frameCounter, rtGeo ? 1u : 0u };

    rhi->beginComputePass("RaytraceRefraction");
    rhi->bindComputePipeline(raytraceRefractionPipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, refractionRT);
    rhi->setComputeTexture(3, prefilterMap);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setComputeBuffer(1, surfelBuffer, 0, surfelBytes);
    rhi->setComputeBuffer(2, cellHeadBuffer);
    rhi->setComputeBuffer(3, surfelNextBuffer);
    rhi->setComputeBuffer(4, gibsDataBuffer, 0, sizeof(GIBSData));
    rhi->setAccelerationStructure(5, sceneTLAS);
    rhi->setComputeBytes(&rp, sizeof(rp), 6);
    // Standalone hit shading inputs: user_instance_id -> InstanceData ->
    // materialID -> base color; dirLights[0] for the sun occlusion ray.
    rhi->setComputeBuffer(7, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * MAX_INSTANCES);
    rhi->setComputeBuffer(8, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
    rhi->setComputeBuffer(9, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
    if (rtGeo) {
        rhi->setComputeBuffer(10, mergedVertexBuffer);
        rhi->setComputeBuffer(11, mergedIndexBuffer);
        rhi->bindComputeTextureArgumentTable(bindlessMaterialTable, 12);
    }
    rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    rhi->endComputePass();
    rhi->computeBarrier();  // refraction writes -> fragment reads in Main
}

// AO raygen into the noisy aoRawRT; the temporal + à-trous passes below
// produce the final aoRT the lighting samples. The kernel is chosen by
// aoMethod exactly like native RaytraceAOPass: 0 = ray traced (needs TLAS),
// 1 = SSAO (same bindings minus the TLAS). On Vulkan (no RT) the SSAO
// fullscreen-fragment twin runs instead.
void Renderer::raytraceAOPass() {
    if (!aoEnabled || !aoRawRT.isValid()) return;

    if (backend == GraphicsBackend::Metal) {
        bool useRT = aoMethod == 0 && capabilities.raytracing &&
                     raytraceAOPipeline.isValid() && sceneTLAS.isValid();
        ComputePipelineHandle pipe = useRT ? raytraceAOPipeline : ssaoPipeline;
        if (!pipe.isValid()) return;

        FrameData fd{};
        fd.frameNumber = frameCounter;
        fd.time = float(frameCounter) / 60.0f;
        fd.deltaTime = 1.0f / 60.0f;
        rhi->updateBuffer(frameDataBuffer, &fd, 0, sizeof(fd));

        Uint32 w = (rhi->getSwapchainWidth() + 1) / 2;   // aoRawRT is half-res
        Uint32 h = (rhi->getSwapchainHeight() + 1) / 2;
        rhi->beginComputePass("RaytraceAO");
        rhi->bindComputePipeline(pipe);
        rhi->setComputeTexture(0, depthStencilRT);
        rhi->setComputeTexture(1, normalRT);
        rhi->setComputeTexture(2, aoRawRT);
        rhi->setComputeBuffer(0, frameDataBuffer, 0, sizeof(FrameData));
        rhi->setComputeBuffer(1, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        if (useRT) rhi->setAccelerationStructure(2, sceneTLAS);
        rhi->dispatch(w, h, 1);
        rhi->endComputePass();
    } else {
        // Vulkan: SSAO.frag into aoRawRT (frameNumber via push-constant slot 0).
        if (!vkSsaoPipeline.isValid() || !depthStencilRT.isValid() || !normalRT.isValid()) return;
        RenderPassDesc rp;
        rp.name = "RaytraceAO";  // keep the pass name stable in the timings panel
        rp.colorAttachments.push_back(aoRawRT);
        rp.clearColors.push_back(glm::vec4(1.0f));
        rp.loadColor.push_back(false);
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(vkSsaoPipeline);
        rhi->setTexture(0, 0, depthStencilRT, clampSampler);
        rhi->setTexture(0, 1, normalRT, clampSampler);
        rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setFragmentBytes(&frameCounter, sizeof(Uint32), 0);
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    }
}

// Temporal AO accumulation (view reprojection + velocity), aoHistory ping-pong.
void Renderer::aoTemporalPass() {
    if (!aoEnabled || !aoRawRT.isValid()) return;
    glm::mat4 curView = currentCamera.view;
    if (!prevViewValid) { prevView = curView; prevViewValid = true; }
    Uint32 historyValid = aoHistoryValid ? 1u : 0u;
    Uint32 inIdx = aoHistoryIndex;
    Uint32 outIdx = inIdx ^ 1u;

    if (backend == GraphicsBackend::Metal) {
        if (!aoTemporalPipeline.isValid()) return;
        Uint32 w = (rhi->getSwapchainWidth() + 1) / 2;   // AO chain is half-res
        Uint32 h = (rhi->getSwapchainHeight() + 1) / 2;
        rhi->beginComputePass("AOTemporal");
        rhi->bindComputePipeline(aoTemporalPipeline);
        rhi->setComputeTexture(0, aoRawRT);
        rhi->setComputeTexture(1, aoHistoryRT[inIdx]);
        rhi->setComputeTexture(2, aoHistoryRT[outIdx]);
        rhi->setComputeTexture(3, velocityRT);
        rhi->setComputeTexture(4, depthStencilRT);
        rhi->setComputeTexture(5, normalRT);
        rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBytes(&prevView, sizeof(glm::mat4), 1);
        rhi->setComputeBytes(&historyValid, sizeof(Uint32), 2);
        rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
        rhi->endComputePass();
    } else {
        // Vulkan: AOTemporal.frag into the history target.
        if (!vkAoTemporalPipeline.isValid() || !velocityRT.isValid() ||
            !aoTemporalDataBuffer.isValid()) return;
        AOTemporalRenderData td;
        td.prevView = prevView;
        td.historyValid = historyValid;
        rhi->updateBuffer(aoTemporalDataBuffer, &td, 0, sizeof(td));

        RenderPassDesc rp;
        rp.name = "AOTemporal";
        rp.colorAttachments.push_back(aoHistoryRT[outIdx]);
        rp.clearColors.push_back(glm::vec4(0.0f));
        rp.loadColor.push_back(false);
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(vkAoTemporalPipeline);
        rhi->setTexture(0, 0, aoRawRT, clampSampler);
        rhi->setTexture(0, 1, aoHistoryRT[inIdx], clampSampler);
        rhi->setTexture(0, 2, velocityRT, clampSampler);
        rhi->setTexture(0, 3, depthStencilRT, clampSampler);
        rhi->setTexture(0, 4, normalRT, clampSampler);
        rhi->setFragmentBuffer(0, aoTemporalDataBuffer, 0, sizeof(AOTemporalRenderData));
        rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    }

    aoHistoryIndex = outIdx;
    aoHistoryValid = true;
    prevView = curView;
}

// À-trous denoise: history -> scratch (stride 1) -> aoRT (stride 2).
void Renderer::aoDenoisePass() {
    if (!aoEnabled || !aoRT.isValid()) return;

    if (backend == GraphicsBackend::Metal) {
        if (!aoDenoisePipeline.isValid()) return;
        Uint32 w = (rhi->getSwapchainWidth() + 1) / 2;   // aoRT is half-res
        Uint32 h = (rhi->getSwapchainHeight() + 1) / 2;
        struct Iter { TextureHandle src, dst; Uint32 stride; };
        const Iter iters[] = {
            { aoHistoryRT[aoHistoryIndex], aoScratchRT, 1u },
            { aoScratchRT, aoRT, 2u },
        };
        rhi->beginComputePass("AODenoise");
        rhi->bindComputePipeline(aoDenoisePipeline);
        for (const Iter& it : iters) {
            rhi->setComputeTexture(0, it.src);
            rhi->setComputeTexture(1, it.dst);
            rhi->setComputeBytes(&it.stride, sizeof(Uint32), 0);
            rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
            rhi->computeBarrier();  // iteration 2 reads what iteration 1 wrote
        }
        rhi->endComputePass();
    } else {
        // Vulkan: two fullscreen passes; the target formats differ (scratch is
        // RGBA16F, aoRT is R16F), so each iteration uses its matching pipeline.
        if (!vkAoDenoisePipelineRGBA.isValid() || !vkAoDenoisePipelineR16.isValid()) return;
        struct Iter { TextureHandle src, dst; PipelineHandle pipe; Uint32 stride; };
        const Iter iters[] = {
            { aoHistoryRT[aoHistoryIndex], aoScratchRT, vkAoDenoisePipelineRGBA, 1u },
            { aoScratchRT, aoRT, vkAoDenoisePipelineR16, 2u },
        };
        for (const Iter& it : iters) {
            RenderPassDesc rp;
            rp.name = "AODenoise";
            rp.colorAttachments.push_back(it.dst);
            rp.clearColors.push_back(glm::vec4(0.0f));
            rp.loadColor.push_back(false);
            rhi->beginRenderPass(rp);
            rhi->bindPipeline(it.pipe);
            rhi->setTexture(0, 0, it.src, clampSampler);
            rhi->setFragmentBytes(&it.stride, sizeof(Uint32), 0);
            rhi->draw(3, 1, 0, 0);
            rhi->endRenderPass();
        }
    }
}

// Stochastic ray-traced point-light shadows (clustered light sampling).
// Mirrors ShadowReservoirSet in restir_shadow_common.metal (32 B per pixel,
// allocated on the HALF-res grid).
struct ShadowReservoirSetCPU {
    Uint32 pointData;    float pointW;
    Uint32 spotData;     float spotW;
    Uint32 rectData;     float rectW;
    Uint32 packedNormal; float viewDepth;
};
static_assert(sizeof(ShadowReservoirSetCPU) == 32, "must match the MSL ShadowReservoirSet");

// Mirrors RestirShadowParams in restir_shadow_common.metal (80 bytes).
struct RestirShadowParamsCPU {
    glm::vec2 screenSize;
    glm::uvec2 gridDims;
    Uint32 frameIndex;
    Uint32 pointCount;
    Uint32 rectCount;
    Uint32 spotCount;
    Uint32 historyValid;
    Uint32 pointCandidates;
    Uint32 rectCandidates;
    Uint32 spotCandidates;
    Uint32 debugMode;
    Uint32 spatialTaps;
    float pointMClamp;
    float rectMClamp;
    float spatialRadius;
    float depthTolerance;
    float normalTolerance;
    float spotMClamp;
};
static_assert(sizeof(RestirShadowParamsCPU) == 80, "must match the MSL RestirShadowParams");

// Stochastic ray-traced point-light shadows (clustered light sampling).
// Routes to the half-res ReSTIR reservoir path when enabled; the legacy
// full-res uniform-pick kernel below stays as the fallback (and the A/B
// reference).
void Renderer::stochasticShadowPass() {
    stochasticShadowWritten = false;  // set again below iff a kernel writes the RT
    stochasticShadowDenoiseRan = false;
    const bool restirWanted = stochasticShadowsEnabled && restirShadowsEnabled &&
                              restirShadowTemporalPipeline.isValid() &&
                              restirShadowResolvePipeline.isValid() &&
                              stochasticShadowUpsamplePipeline.isValid();
    // Reservoirs exist exactly while the ReSTIR path is active: free them the
    // frame the feature stops running (toggle off, missing TLAS, ...) so a
    // one-time experiment doesn't pin 2x 32 B/half-pixel for the session.
    if (!restirWanted && restirReservoirHistory.isValid()) {
        rhi->destroyBuffer(restirReservoirHistory);
        rhi->destroyBuffer(restirReservoirScratch);
        restirReservoirHistory = {};
        restirReservoirScratch = {};
        restirHistoryValid = false;
    }
    if (!stochasticShadowsEnabled) return;  // off = aligns with the RT-less Vulkan output
    if (!sceneTLAS.isValid() || !stochasticShadowRT.isValid()) return;
    if (restirWanted && restirShadowPass()) { stochasticShadowWritten = true; return; }
    if (!stochasticShadowPipeline.isValid()) return;
    Uint32 w = rhi->getSwapchainWidth();
    Uint32 h = rhi->getSwapchainHeight();
    glm::vec2 screenSize(w, h);
    // The kernel declares a NON-packed `constant uint3&` (16 bytes in MSL,
    // unlike the packed_uint3 the other kernels use) — push a uvec4.
    glm::uvec4 gridDims(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ, 0u);
    Uint32 fi = frameCounter;
    Uint32 debugMode = stochasticShadowDebugMode;  // panel "Point shadow view"
    rhi->beginComputePass(renderGraph.activePassName().c_str());
    rhi->bindComputePipeline(stochasticShadowPipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, stochasticShadowRT);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setComputeBuffer(1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
    rhi->setComputeBuffer(2, clusterBuffer);
    rhi->setComputeBytes(&screenSize, sizeof(glm::vec2), 3);
    rhi->setComputeBytes(&gridDims, sizeof(glm::uvec4), 4);
    rhi->setComputeBytes(&fi, sizeof(Uint32), 5);
    rhi->setAccelerationStructure(6, sceneTLAS);
    rhi->setComputeBytes(&debugMode, sizeof(Uint32), 7);
    // Rect area + spot light shadow inputs (channels G / B of the output).
    rhi->setComputeBuffer(8, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
    rhi->setComputeBuffer(9, rectLightBuffer);
    glm::uvec2 extraCounts(static_cast<Uint32>(rectLights.size()),
                           static_cast<Uint32>(spotLights.size()));
    rhi->setComputeBytes(&extraCounts, sizeof(glm::uvec2), 10);
    rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    rhi->endComputePass();
    stochasticShadowWritten = true;
}

// ReSTIR denoise for the stochastic shadows. Three kernels in one compute pass:
//   1) 3d_restir_shadow_temporal — fresh light candidates (RIS, no rays) +
//      velocity-reprojected temporal reservoir merge -> scratch buffer (HALF res)
//   2) 3d_restir_shadow_resolve — spatial reservoir merge + winner visibility
//      rays -> stochasticShadowHalfRT (RGB) + history buffer (HALF res)
//   3) 3d_stochastic_shadow_upsample — joint bilateral upsample -> stochasticShadowRT
// The winner's traced visibility estimates the contribution-weighted shadow
// factor per domain; the existing full-res StochasticShadowTemporal accumulator
// and StochasticShadowDenoise filter stay as the final averager. Ray budget: <= 4 per
// HALF pixel (1 point + 1 spot + 2 stratified rect quad points) = ~1 ray
// per full-res pixel; the legacy full-res kernel spends up to 4. (Rect has no
// distance cull — matching the PBR's global rect loop — so it traces on every
// forward-facing pixel; keeping it to 2 rays is what bounds the pass cost.)
// Returns false (without recording) if the reservoirs can't be allocated, so
// the caller falls back to the legacy kernel.
bool Renderer::restirShadowPass() {
    Uint32 w = rhi->getSwapchainWidth();
    Uint32 h = rhi->getSwapchainHeight();
    Uint32 halfW = (w + 1) / 2;
    Uint32 halfH = (h + 1) / 2;
    if (!stochasticShadowHalfRT.isValid()) return false;

    // Reservoirs allocate on first use and are freed the moment the path stops
    // running (see stochasticShadowPass) or the swapchain resizes
    // (destroyRenderTargets). 32 B/half-pixel x2: ~7 MB combined at 1500x900,
    // ~59 MB at 2560x1440 retina, ~133 MB at 4K.
    if (!restirReservoirHistory.isValid() || !restirReservoirScratch.isValid()) {
        BufferDesc bd;
        bd.size = size_t(halfW) * size_t(halfH) * sizeof(ShadowReservoirSetCPU);
        bd.usage = BufferUsage::Storage;
        bd.memoryUsage = MemoryUsage::GPU;
        try {
            restirReservoirHistory = rhi->createBuffer(bd);
            restirReservoirScratch = rhi->createBuffer(bd);
        } catch (const std::exception& e) {
            if (restirReservoirHistory.isValid()) rhi->destroyBuffer(restirReservoirHistory);
            restirReservoirHistory = {};
            restirReservoirScratch = {};
            restirShadowsEnabled = false;  // don't retry every frame
            fmt::print(stderr, "restirShadowPass: reservoir allocation failed ({}), "
                               "falling back to the legacy stochastic kernel\n", e.what());
            return false;
        }
        restirHistoryValid = false;  // fresh buffers hold garbage, not history
    }

    RestirShadowParamsCPU p{};
    p.screenSize = glm::vec2(w, h);  // FULL res; kernels derive the half grid
    p.gridDims = glm::uvec2(clusterGridSizeX, clusterGridSizeY);
    // frameNumber, not frameCounter: the latter only advances inside
    // lightScatteringPass, so it freezes (and with it the RNG sequence and
    // history-contiguity check) whenever god rays are toggled off.
    p.frameIndex = frameNumber;
    p.pointCount = static_cast<Uint32>(pointLights.size());
    p.rectCount = static_cast<Uint32>(rectLights.size());
    p.spotCount = static_cast<Uint32>(spotLights.size());
    // History is only trusted when the pass also ran last frame — any skip
    // (toggle, invalid TLAS, resize, graph edits) breaks the chain here
    // instead of every skip site having to remember to invalidate.
    p.historyValid = (restirHistoryValid && frameNumber == restirLastFrame + 1) ? 1u : 0u;
    p.pointCandidates = std::max(restirPointCandidates, 1u);  // panel slider allows typed 0
    p.rectCandidates = restirRectCandidates;
    p.spotCandidates = restirSpotCandidates;
    p.debugMode = stochasticShadowDebugMode;
    p.spatialTaps = restirSpatialTaps;
    p.pointMClamp = restirPointMClamp * float(p.pointCandidates);
    p.rectMClamp = restirRectMClamp * float(p.rectCandidates);
    p.spotMClamp = restirPointMClamp * float(p.spotCandidates);
    p.spatialRadius = restirSpatialRadius * 0.5f;  // panel value is full-res px; taps run on the half grid
    p.depthTolerance = 0.1f;
    p.normalTolerance = 0.9f;

    // Three separately-timed compute passes so the profiler shows where the
    // cost actually goes — candidate-gen ALU vs resolve+rays vs the full-res
    // upsample. They're dependent (each reads the previous), so Metal already
    // serializes them via hazard tracking; separate encoders cost nothing over
    // the single-encoder version and buy the per-kernel breakdown.
    const std::string base = renderGraph.activePassName() + " (ReSTIR ";
    rhi->beginComputePass((base + "candidates)").c_str());
    // Pass 1 (half grid): fresh candidates + temporal reservoir merge.
    rhi->bindComputePipeline(restirShadowTemporalPipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, velocityRT);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setComputeBuffer(1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
    rhi->setComputeBuffer(2, clusterBuffer);
    rhi->setComputeBuffer(3, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
    rhi->setComputeBuffer(4, rectLightBuffer);
    rhi->setComputeBuffer(5, restirReservoirHistory);
    rhi->setComputeBuffer(6, restirReservoirScratch);
    rhi->setComputeBytes(&p, sizeof(p), 7);
    rhi->dispatch((halfW + 7) / 8, (halfH + 7) / 8, 1);
    rhi->endComputePass();

    rhi->beginComputePass((base + "resolve+rays)").c_str());
    // Pass 2 (half grid): spatial merge + winner visibility rays. Reads the
    // scratch reservoirs, writes the half-res raw target + the history buffer
    // consumed next frame.
    rhi->bindComputePipeline(restirShadowResolvePipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, stochasticShadowHalfRT);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setComputeBuffer(1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
    rhi->setComputeBuffer(2, clusterBuffer);
    rhi->setComputeBuffer(3, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
    rhi->setComputeBuffer(4, rectLightBuffer);
    rhi->setComputeBuffer(5, restirReservoirScratch);
    rhi->setComputeBuffer(6, restirReservoirHistory);
    rhi->setAccelerationStructure(7, sceneTLAS);
    rhi->setComputeBytes(&p, sizeof(p), 8);
    rhi->dispatch((halfW + 7) / 8, (halfH + 7) / 8, 1);
    rhi->endComputePass();

    rhi->beginComputePass((base + "upsample)").c_str());
    // Pass 3 (full grid): joint bilateral upsample into the raw target the
    // temporal accumulator reads.
    rhi->bindComputePipeline(stochasticShadowUpsamplePipeline);
    rhi->setComputeTexture(0, stochasticShadowHalfRT);
    rhi->setComputeTexture(1, depthStencilRT);
    rhi->setComputeTexture(2, normalRT);
    rhi->setComputeTexture(3, stochasticShadowRT);
    rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    rhi->endComputePass();

    restirHistoryValid = true;
    restirLastFrame = frameNumber;
    return true;
}

// GIBS surfel GI (RequiresRaytracing): generate surfels from the pre-pass
// G-buffer, rebuild the spatial hash, ray-trace surfel irradiance against the
// TLAS, temporally smooth, then gather per-pixel GI into giResultTexture. One
// graph pass chaining the five native kernels with compute barriers.
void Renderer::gibsPass() {
    if (!gibsEnabled || !sceneTLAS.isValid() || !surfelGenPipeline.isValid() ||
        !surfelClearPipeline.isValid() || !surfelInsertPipeline.isValid() ||
        !surfelRTPipeline.isValid() || !surfelTemporalPipeline.isValid() ||
        !giSamplePipeline.isValid() || !giResultTexture.isValid()) {
        return;
    }

    // Counter readback: p[1] = allocated surfels (one frame latent, like the
    // native shared-memory read); p[0] = per-frame new-surfel budget counter.
    // A panel-requested reset zeroes both cursors (native resetSurfels());
    // stale surfels beyond the count stay in the buffer but every consumer
    // gates on activeSurfelCount, so they are never read.
    if (Uint32* p = static_cast<Uint32*>(rhi->mapBuffer(surfelCounterBuffer))) {
        if (gibsResetRequested) {
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;  // free-list depth: drop all reclaimed slots too
            gibsActiveSurfels = 0;
            gibsResetRequested = false;
        } else {
            gibsActiveSurfels = std::min(p[1], gibsMaxSurfels);
            p[0] = 0;
            // p[1] (high-water) and p[2] (free-list depth) persist across frames.
        }
        rhi->unmapBuffer(surfelCounterBuffer);
    }

    Uint32 w = rhi->getSwapchainWidth();
    Uint32 h = rhi->getSwapchainHeight();
    glm::vec2 screenSize(w, h);
    glm::vec2 giResolution = screenSize * gibsResolutionScale;

    // Grid over the world bounds, derived exactly like native's
    // GIBSManager::calculateGridSize (128^3 at 1.0m cells for the ±64 default).
    // Must match the cellHeadBuffer allocation in initialize().
    glm::uvec3 gridSize; Uint32 totalCells; float cellSize;
    computeGibsGrid(gibsWorldMin, gibsWorldMax, gridSize, totalCells, cellSize);

    GIBSData gd{};
    gd.invViewProj = glm::inverse(currentCamera.proj * currentCamera.view);
    gd.prevViewProj = gibsPrevViewProj;
    gd.cameraPosition = currentCamera.position;
    gd.sunDirection = glm::normalize(atmosphereData.sunDirection);
    gd.sunColor = atmosphereData.sunColor;
    gd.sunIntensity = atmosphereData.sunIntensity;
    gd.maxSurfels = gibsMaxSurfels;
    gd.activeSurfelCount = gibsActiveSurfels;
    gd.surfelRadius = 0.25f;
    gd.surfelDensity = 4.0f;
    gd.worldMin = gibsWorldMin;
    gd.cellSize = cellSize;
    gd.worldMax = gibsWorldMax;
    gd.totalCells = totalCells;
    gd.gridSize = gridSize;
    gd.raysPerSurfel = gibsRaysPerSurfel;
    gd.maxBounces = 1;
    gd.rayBias = 0.001f;
    gd.rayMaxDistance = 100.0f;
    gd.temporalBlend = 0.1f;
    gd.hysteresis = 0.95f;
    gd.frameIndex = frameCounter;
    gd.screenSize = screenSize;
    gd.giResolution = giResolution;
    gd.sampleRadius = 1;
    gd.maxSurfelsPerPixel = 8;
    rhi->updateBuffer(gibsDataBuffer, &gd, 0, sizeof(gd));
    gibsPrevViewProj = currentCamera.proj * currentCamera.view;

    const size_t surfelBytes = size_t(gibsMaxSurfels) * sizeof(Surfel);
    Uint32 active = std::max(gibsActiveSurfels, 1u);

    rhi->beginComputePass("GIBS");

    // 0) Age + evict existing surfels, pushing reclaimed slots onto the
    //    free-list, BEFORE generation so this frame can reuse them. Without
    //    this the pool only grows and freezes at maxSurfels (manual reset was
    //    the only recovery). Iterates [0, active); the kernel skips invalid
    //    slots (already freed).
    if (surfelUpdatePipeline.isValid() && gibsActiveSurfels > 0) {
        rhi->bindComputePipeline(surfelUpdatePipeline);
        rhi->setComputeBuffer(0, surfelBuffer, 0, surfelBytes);
        rhi->setComputeBuffer(1, surfelCounterBuffer, 0, 4 * sizeof(Uint32));
        rhi->setComputeBuffer(2, gibsDataBuffer, 0, sizeof(GIBSData));
        rhi->setComputeBuffer(3, surfelFreeListBuffer);
        rhi->dispatch((gibsActiveSurfels + 255) / 256, 1, 1);
        rhi->computeBarrier();
    }

    // 1) Surfel generation from the pre-pass G-buffer.
    SurfelGenerationParams gp{};
    gp.invViewProj = gd.invViewProj;
    gp.screenSize = screenSize;
    gp.surfelRadius = gd.surfelRadius;
    gp.densityThreshold = 0.01f;
    gp.maxNewSurfels = std::max(gibsMaxSurfels / 100, 1000u);
    gp.frameIndex = frameCounter;
    rhi->bindComputePipeline(surfelGenPipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, albedoRT);
    rhi->setComputeBuffer(0, surfelBuffer, 0, surfelBytes);
    rhi->setComputeBuffer(1, surfelCounterBuffer, 0, 4 * sizeof(Uint32));
    rhi->setComputeBuffer(2, gibsDataBuffer, 0, sizeof(GIBSData));
    rhi->setComputeBytes(&gp, sizeof(gp), 3);
    rhi->setComputeBuffer(4, cellHeadBuffer);
    rhi->setComputeBuffer(5, surfelNextBuffer);
    rhi->setComputeBuffer(6, surfelFreeListBuffer);  // pop reclaimed slots
    rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    rhi->computeBarrier();

    // 2) Spatial hash: clear cell heads, then insert surfels.
    rhi->bindComputePipeline(surfelClearPipeline);
    rhi->setComputeBuffer(0, cellHeadBuffer);
    rhi->setComputeBuffer(1, gibsDataBuffer, 0, sizeof(GIBSData));
    rhi->dispatch((totalCells + 255) / 256, 1, 1);
    rhi->computeBarrier();
    rhi->bindComputePipeline(surfelInsertPipeline);
    rhi->setComputeBuffer(0, surfelBuffer, 0, surfelBytes);
    rhi->setComputeBuffer(1, cellHeadBuffer);
    rhi->setComputeBuffer(2, surfelNextBuffer);
    rhi->setComputeBuffer(3, gibsDataBuffer, 0, sizeof(GIBSData));
    rhi->dispatch((active + 255) / 256, 1, 1);
    rhi->computeBarrier();

    // 3) Surfel ray tracing (interval-staggered like native).
    constexpr Uint32 UPDATE_INTERVAL = 4;
    SurfelRaytracingParams rp{};
    rp.surfelOffset = 0;
    rp.surfelCount = gibsActiveSurfels;
    rp.raysPerSurfel = gibsRaysPerSurfel;
    rp.frameIndex = frameCounter;
    rp.rayBias = gd.rayBias;
    rp.rayMaxDistance = gd.rayMaxDistance;
    rp.updateInterval = UPDATE_INTERVAL;
    if (rp.surfelCount > 0) {
        rhi->bindComputePipeline(surfelRTPipeline);
        rhi->setComputeBuffer(0, surfelBuffer, 0, surfelBytes);
        rhi->setComputeBuffer(1, cellHeadBuffer);
        rhi->setComputeBuffer(2, gibsDataBuffer, 0, sizeof(GIBSData));
        rhi->setComputeBytes(&rp, sizeof(rp), 3);
        rhi->setAccelerationStructure(4, sceneTLAS);
        rhi->setComputeBuffer(5, surfelNextBuffer);
        Uint32 perFrame = (rp.surfelCount + UPDATE_INTERVAL - 1) / UPDATE_INTERVAL;
        rhi->dispatch((perFrame + 63) / 64, 1, 1);
        rhi->computeBarrier();

        // 4) Temporal smoothing over surfel irradiance.
        rhi->bindComputePipeline(surfelTemporalPipeline);
        rhi->setComputeBuffer(0, surfelBuffer, 0, surfelBytes);
        rhi->setComputeBuffer(1, gibsDataBuffer, 0, sizeof(GIBSData));
        rhi->dispatch((active + 255) / 256, 1, 1);
        rhi->computeBarrier();
    }

    // 5) Per-pixel GI gather. With the SVGF-lite chain available it writes the
    // raw target and the chain below produces giResultTexture; otherwise it
    // writes giResultTexture directly (the pre-denoise behavior).
    const bool denoise = gibsDenoiseEnabled &&
        giTemporalPipeline.isValid() && giDenoisePipeline.isValid() &&
        giRawRT.isValid() && giScratchRT.isValid() &&
        giHistoryChainRT[0].isValid() && giHistoryChainRT[1].isValid() &&
        velocityRT.isValid();
    GIBSSampleParams sp{};
    sp.invViewProj = gd.invViewProj;
    sp.screenSize = screenSize;
    sp.giResolution = giResolution;
    sp.sampleRadius = gd.cellSize;
    sp.maxSamples = gd.maxSurfelsPerPixel;
    sp.normalWeight = 1.0f;
    sp.distanceWeight = 1.0f;
    rhi->bindComputePipeline(giSamplePipeline);
    rhi->setComputeTexture(0, depthStencilRT);
    rhi->setComputeTexture(1, normalRT);
    rhi->setComputeTexture(2, denoise ? giRawRT : giResultTexture);
    rhi->setComputeBuffer(0, surfelBuffer, 0, surfelBytes);
    rhi->setComputeBuffer(1, cellHeadBuffer);
    rhi->setComputeBuffer(2, gibsDataBuffer, 0, sizeof(GIBSData));
    rhi->setComputeBytes(&sp, sizeof(sp), 3);
    rhi->setComputeBuffer(4, surfelNextBuffer);
    rhi->dispatch((Uint32(giResolution.x) + 7) / 8, (Uint32(giResolution.y) + 7) / 8, 1);
    rhi->endComputePass();
    rhi->computeBarrier();  // GI writes -> next consumer

    // 6) SVGF-lite: temporal reprojection over the gather, then two edge-aware
    // a-trous iterations (stride 1, 2) into giResultTexture. Blends away the
    // surfel-disc seams and the flicker the surfel-space EMA leaves behind.
    if (denoise) {
        if (!giPrevViewValid) { giPrevView = currentCamera.view; giPrevViewValid = true; }
        Uint32 histIn = giHistoryIndex;
        Uint32 histOut = histIn ^ 1u;
        Uint32 histValid = giHistoryValid ? 1u : 0u;
        // Chain dispatch covers the full GI texture (64px floor included) so
        // the history never carries unwritten texels into the a-trous taps.
        Uint32 giW = std::max(64u, Uint32(w * gibsResolutionScale));
        Uint32 giH = std::max(64u, Uint32(h * gibsResolutionScale));

        rhi->beginComputePass("GIBSDenoise");
        rhi->bindComputePipeline(giTemporalPipeline);
        rhi->setComputeTexture(0, giRawRT);
        rhi->setComputeTexture(1, giHistoryChainRT[histIn]);
        rhi->setComputeTexture(2, giHistoryChainRT[histOut]);
        rhi->setComputeTexture(3, velocityRT);
        rhi->setComputeTexture(4, depthStencilRT);
        rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBytes(&giPrevView, sizeof(glm::mat4), 1);
        rhi->setComputeBytes(&histValid, sizeof(Uint32), 2);
        rhi->dispatch((giW + 7) / 8, (giH + 7) / 8, 1);
        rhi->computeBarrier();

        Uint32 stride = 1;
        rhi->bindComputePipeline(giDenoisePipeline);
        rhi->setComputeTexture(0, giHistoryChainRT[histOut]);
        rhi->setComputeTexture(1, giScratchRT);
        rhi->setComputeTexture(2, normalRT);
        rhi->setComputeBytes(&stride, sizeof(Uint32), 0);
        rhi->dispatch((giW + 7) / 8, (giH + 7) / 8, 1);
        rhi->computeBarrier();

        stride = 2;
        rhi->setComputeTexture(0, giScratchRT);
        rhi->setComputeTexture(1, giResultTexture);
        rhi->setComputeTexture(2, normalRT);
        rhi->setComputeBytes(&stride, sizeof(Uint32), 0);
        rhi->dispatch((giW + 7) / 8, (giH + 7) / 8, 1);
        rhi->endComputePass();
        rhi->computeBarrier();

        giHistoryIndex = histOut;
        giHistoryValid = true;
        giPrevView = currentCamera.view;
    }
}

// Temporal point-shadow resolve. Native copies denoised->history with a blit;
// here the handles are swapped instead (post-swap, history holds the latest
// denoised result and is what the PBR shader binds).
void Renderer::stochasticShadowTemporalPass() {
    // Only accumulate frames the stochastic pass actually wrote (covers the
    // feature toggle, missing RT support, and the frames before the TLAS is
    // built) — otherwise the EMA would fold undefined texture memory into the
    // history the PBR pass samples.
    if (!stochasticShadowWritten) return;
    if (!stochasticShadowTemporalPipeline.isValid() || !stochasticShadowRT.isValid()) return;
    Uint32 w = rhi->getSwapchainWidth();
    Uint32 h = rhi->getSwapchainHeight();
    rhi->beginComputePass(renderGraph.activePassName().c_str());
    rhi->bindComputePipeline(stochasticShadowTemporalPipeline);
    rhi->setComputeTexture(0, stochasticShadowRT);
    rhi->setComputeTexture(1, stochasticShadowHistoryRT);
    rhi->setComputeTexture(2, velocityRT);
    rhi->setComputeTexture(3, stochasticShadowDenoisedRT);
    rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    rhi->endComputePass();
    std::swap(stochasticShadowDenoisedRT, stochasticShadowHistoryRT);
    stochasticShadowHistoryWritten = true;
}

// Edge-aware 5x5 cross-bilateral filter over the accumulated shadow factors —
// the spatial-filtering stage of the stochastic-RT skeleton. The accumulator's
// EMA keeps a variance floor for the rect penumbra's per-frame coverage
// samples; averaging geometry-compatible neighbors removes it. Reads the
// post-swap history (the accumulated result) and writes the now-scratch
// denoised target, which is what the PBR pass samples — so the history
// feedback stays unfiltered and the blur never compounds across frames.
void Renderer::stochasticShadowDenoisePass() {
    if (!stochasticShadowWritten) return;  // accumulator didn't run either
    if (!stochasticShadowDenoisePipeline.isValid() || !stochasticShadowHistoryRT.isValid() ||
        !stochasticShadowDenoisedRT.isValid() || !stochasticShadowRT.isValid()) return;
    Uint32 w = rhi->getSwapchainWidth();
    Uint32 h = rhi->getSwapchainHeight();
    // À-trous: accumulated -> scratch (stride 1) -> display copy (stride 2).
    // stochasticShadowRT (the raw upsample) is free once the accumulator consumed it,
    // so it doubles as the ping-pong scratch — no extra RT. stochasticShadowHistoryRT
    // is only READ here, so the frame count packed in its alpha survives for
    // next frame's accumulator.
    struct Iter { TextureHandle src, dst; Uint32 stride; };
    const Iter iters[] = {
        { stochasticShadowHistoryRT, stochasticShadowRT, 1u },
        { stochasticShadowRT, stochasticShadowDenoisedRT, 2u },
    };
    rhi->beginComputePass(renderGraph.activePassName().c_str());
    rhi->bindComputePipeline(stochasticShadowDenoisePipeline);
    for (const Iter& it : iters) {
        rhi->setComputeTexture(0, it.src);
        rhi->setComputeTexture(1, depthStencilRT);
        rhi->setComputeTexture(2, normalRT);
        rhi->setComputeTexture(3, it.dst);
        rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBytes(&it.stride, sizeof(Uint32), 1);
        rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
        rhi->computeBarrier();  // pass 2 reads what pass 1 wrote (stochasticShadowRT)
    }
    rhi->endComputePass();
    stochasticShadowDenoiseRan = true;
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
    // cascadeSplits.x is the view-space depth where the ray-traced near-field
    // shadow ends and the PSSM cascades begin (the PBR shader uses texShadow
    // for viewDepth <= splits.x). On RT backends the RT shadow owns the near
    // 0..pssmRTMaxDist metres — hardcoding nearClip here (as before) shrank
    // that region to ~0, so the RT shadow never applied and only the softer
    // PSSM showed, matching the "weak RT shadow" report. Vulkan has no RT
    // shadow, so it keeps nearClip (cascade 0 starts at the near plane).
    // pssmRTMaxDist is a member now (panel slider "RT shadow max dist", 5..200).
    // The independent near-field shadow map (not RT) owns [near, rtEnd] on this
    // path, so the boundary is unconditional — no capabilities.raytracing gate.
    // (Previously, with RT available the near map only reached nearShadowEnd
    // while cascades started at rtEnd, leaving [nearShadowEnd, rtEnd] unshadowed
    // because RHIMain.frag never consumed the RT shadow.)
    const float rtEnd = pssmRTMaxDist;

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
        // Quantize the radius so texelSize doesn't jitter from per-frame float noise
        sphereRadius = std::ceil(sphereRadius * 16.0f) / 16.0f;

        // Snap the cascade center to the shadow-map texel grid so the map moves
        // in whole texels with the camera (anti-shimmer). The snap MUST be done
        // in a world-anchored, rotation-only light frame: snapping
        // lightView * sphereCenter is a no-op because that view looks AT
        // sphereCenter, so the product is always (0, 0, -lightDist).
        const glm::mat4 lightRot = glm::lookAt(glm::vec3(0.0f), lightDir, up);
        const float texelSize = (2.0f * sphereRadius) / float(SHADOW_MAP_SIZE);
        glm::vec3 lsC = glm::vec3(lightRot * glm::vec4(sphereCenter, 1.0f));
        lsC.x = std::floor(lsC.x / texelSize) * texelSize;
        lsC.y = std::floor(lsC.y / texelSize) * texelSize;
        const glm::vec3 snapped = glm::vec3(glm::inverse(lightRot) * glm::vec4(lsC, 1.0f));

        const float lightDist = sphereRadius * 2.0f + 1.0f;
        glm::mat4 lightView = glm::lookAt(snapped - lightDir * lightDist, snapped, up);

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

    // Independent near-field shadow map for the [near, rtEnd] sub-frustum.
    // Unlike the cascades (bounding-sphere fit, for zero rotation shimmer), the
    // near map uses a TIGHT AABB fit: it recovers the ~30-40% resolution a sphere
    // wastes around the slice, so the near shadow is noticeably sharper. Still
    // texel-snapped in a world-anchored light frame so the map translates in
    // whole texels; the AABB *size* can shift slightly on rotation (minor
    // shimmer), which is acceptable over this small near range.
    gpuData.nearShadowEnd = rtEnd;
    gpuData.pcfSampleCount = pssmPcfSampleCount;
    gpuData.cascadeBlendRange = pssmCascadeBlendRange;
    gpuData.debugVisualize = pssmDebugVisualize ? 1u : 0u;
    if (rtEnd > nearClip) {
        float nNDC = viewDepthToNDCz(glm::clamp(nearClip, nearClip, farClip));
        float fNDC = viewDepthToNDCz(glm::clamp(rtEnd,    nearClip, farClip));
        const glm::vec4 ndc8[8] = {
            {-1,-1,nNDC,1},{1,-1,nNDC,1},{-1,1,nNDC,1},{1,1,nNDC,1},
            {-1,-1,fNDC,1},{1,-1,fNDC,1},{-1,1,fNDC,1},{1,1,fNDC,1},
        };
        glm::vec3 c[8];
        for (int i = 0; i < 8; i++) { glm::vec4 w = invVP * ndc8[i]; c[i] = glm::vec3(w) / w.w; }
        // XY AABB (+ Z extent) of the slice in a world-anchored, rotation-only
        // light frame.
        const glm::mat4 lightRot = glm::lookAt(glm::vec3(0.0f), lightDir, up);
        glm::vec3 lmin(1e30f), lmax(-1e30f);
        for (auto& p : c) {
            glm::vec3 lc = glm::vec3(lightRot * glm::vec4(p, 1.0f));
            lmin = glm::min(lmin, lc); lmax = glm::max(lmax, lc);
        }
        // Square, quantized extent -> stable texel size frame to frame.
        float extent = std::ceil(glm::max(lmax.x - lmin.x, lmax.y - lmin.y) * 16.0f) / 16.0f;
        float texel = extent / float(NEAR_SHADOW_MAP_SIZE);
        // Snap the box centre to the texel grid so the map moves in whole texels.
        glm::vec2 boxCtr(0.5f * (lmin.x + lmax.x), 0.5f * (lmin.y + lmax.y));
        boxCtr.x = std::floor(boxCtr.x / texel) * texel;
        boxCtr.y = std::floor(boxCtr.y / texel) * texel;
        glm::vec3 ctrWorld = glm::vec3(glm::inverse(lightRot) *
            glm::vec4(boxCtr.x, boxCtr.y, 0.5f * (lmin.z + lmax.z), 1.0f));
        const float dist = extent + 1.0f;
        glm::mat4 lv = glm::lookAt(ctrWorld - lightDir * dist, ctrWorld, up);
        float mn = 1e30f, mx = -1e30f;
        for (auto& p : c) { float d = -(lv * glm::vec4(p, 1.0f)).z; mn = glm::min(mn, d); mx = glm::max(mx, d); }
        mn -= (mx - mn);
        gpuData.nearLightMatrix = glm::orthoZO(-extent * 0.5f, extent * 0.5f,
                                               -extent * 0.5f, extent * 0.5f, mn, mx) * lv;
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
        if (backend == GraphicsBackend::Metal) {
            // 3d_pssm_shadow_depth.metal: this cascade's matrix at buffer(0)
            // (no cascadeIndex — one matrix per pass), materials at buffer(1)
            // for the alpha-test UV path, instances at buffer(2).
            rhi->setVertexBytes(&gpuData.lightSpaceMatrices[ci], sizeof(glm::mat4), 0);
            rhi->setVertexBuffer(1, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
        } else {
            rhi->setVertexBuffer(0, pssmDataBuffer, 0, sizeof(PSSMRenderData));
            rhi->setVertexBytes(&ci, sizeof(Uint32), 5);  // cascadeIndex -> push offset 16
            // Materials at set0 b1 for the fragment alpha-cutout (ShadowDepth.frag).
            rhi->setVertexBuffer(1, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
        }
        rhi->setVertexBuffer(2, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * std::max<Uint32>(1, totalInstanceCount));
        // MDI layout: Metal's shadow shader pulls instances[iid].vertexOffset +
        // vertex_id, so it must read the merged vertex buffer (see prePass).
        const bool shadowPullsMerged = backend == GraphicsBackend::Metal &&
                                       m_mdiInstanceLayout && mergedVertexBuffer.isValid();
        if (shadowPullsMerged) rhi->bindVertexBuffer(mergedVertexBuffer, 3, 0);
        // Default albedo so the shadow frag's declared sampler (set2 b0) is
        // always valid even with no MASK casters; opaque casters never sample
        // it (alphaCutoff == 0), MASK casters override it per draw below.
        if (defaultWhiteTexture < textures.size() && textures[defaultWhiteTexture].handle.isValid())
            rhi->setTexture(0, 0, textures[defaultWhiteTexture].handle, textures[defaultWhiteTexture].sampler);

        // ALL drawables, not the camera-visible set: casters outside the view
        // frustum must still render into the cascades or their shadows vanish
        // when they leave the screen. (updateBuffers uploads instance data for
        // every drawable — culled ones follow the visible prefix.)
        for (Uint32 drawableIdx = 0; drawableIdx < frameDrawables.size(); drawableIdx++) {
            const Drawable& drawable = frameDrawables[drawableIdx];
            const RenderMesh& mesh = meshes[drawable.mesh];
            auto it = drawableToInstanceID.find(drawableIdx);
            if (it == drawableToInstanceID.end()) continue;
            Uint32 iid = it->second;
            // Alpha-cutout casters (MASK) bind their albedo for the fragment
            // discard — Metal via 3d_pssm_shadow_depth, Vulkan via
            // ShadowDepth.frag. Opaque casters need no texture (pure depth).
            if (drawable.material < materials.size() &&
                materials[drawable.material].alphaMode == AlphaMode::MASK) {
                bindMaterialAlbedo(drawable.material);
            }
            if (!shadowPullsMerged && mesh.vertexBuffer.isValid()) rhi->bindVertexBuffer(mesh.vertexBuffer, 3, 0);
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

    // Render the independent near-field shadow map into its own texture. The VS
    // selects nearLightMatrix when cascadeIndex == 3 (Vulkan) / receives it via
    // vertex bytes (Metal).
    if (rtEnd > nearClip && nearShadowMap.isValid()) {
        RenderPassDesc rp;
        rp.name = "NearShadow";
        rp.depthAttachment = nearShadowMap;
        rp.loadDepth = false;  // clear
        rp.clearDepth = 1.0f;
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(shadowPipeline);
        if (backend == GraphicsBackend::Metal) {
            rhi->setVertexBytes(&gpuData.nearLightMatrix, sizeof(glm::mat4), 0);
            rhi->setVertexBuffer(1, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
        } else {
            Uint32 nearIdx = 3u;
            rhi->setVertexBuffer(0, pssmDataBuffer, 0, sizeof(PSSMRenderData));
            rhi->setVertexBytes(&nearIdx, sizeof(Uint32), 5);  // cascadeIndex -> push offset 16
            rhi->setVertexBuffer(1, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
        }
        rhi->setVertexBuffer(2, instanceDataBuffer, 0, sizeof(Vapor::InstanceData) * std::max<Uint32>(1, totalInstanceCount));
        // Same merged-VB requirement as the cascade loop above (MDI layout).
        const bool nearPullsMerged = backend == GraphicsBackend::Metal &&
                                     m_mdiInstanceLayout && mergedVertexBuffer.isValid();
        if (nearPullsMerged) rhi->bindVertexBuffer(mergedVertexBuffer, 3, 0);
        if (defaultWhiteTexture < textures.size() && textures[defaultWhiteTexture].handle.isValid())
            rhi->setTexture(0, 0, textures[defaultWhiteTexture].handle, textures[defaultWhiteTexture].sampler);
        for (Uint32 drawableIdx = 0; drawableIdx < frameDrawables.size(); drawableIdx++) {
            const Drawable& drawable = frameDrawables[drawableIdx];
            const RenderMesh& mesh = meshes[drawable.mesh];
            auto it = drawableToInstanceID.find(drawableIdx);
            if (it == drawableToInstanceID.end()) continue;
            Uint32 iid = it->second;
            if (drawable.material < materials.size() &&
                materials[drawable.material].alphaMode == AlphaMode::MASK) {
                bindMaterialAlbedo(drawable.material);  // albedo for the alpha-cutout
            }
            if (!nearPullsMerged && mesh.vertexBuffer.isValid()) rhi->bindVertexBuffer(mesh.vertexBuffer, 3, 0);
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

// Screen-space contact shadows: march the pre-pass depth toward the light and
// write a visibility RT (R: 0 shadowed, 1 lit). The main pass min-composites it
// onto the sun shadow, tightening the near contact the cascade/near map miss.
// Vulkan fullscreen frag only (the Metal native renderer has RT near shadows).
void Renderer::sscsPass() {
    if (!sscsEnabled || !sscsRT.isValid() || !depthStencilRT.isValid() || directionalLights.empty()) {
        return;
    }

    // Metal (RHI): compute path with 3d_sscs.metal (mirrors raytraceShadowPass).
    // The kernel derives the view-space light dir from directionalLights[0] +
    // camera itself, so we only feed camera + lights + screenSize + params.
    if (backend == GraphicsBackend::Metal) {
        if (!sscsComputePipeline.isValid()) return;
        struct SSCSParamsC { float rayLength; float thickness; Uint32 stepCount; float bias; } params;
        params.rayLength = sscsLength;
        params.thickness = sscsThickness;
        params.stepCount = sscsSteps;
        params.bias      = sscsBias;
        Uint32 w = (rhi->getSwapchainWidth() + 1) / 2;   // sscsRT is half-res
        Uint32 h = (rhi->getSwapchainHeight() + 1) / 2;
        glm::vec2 screenSize(w, h);
        rhi->beginComputePass("SSCS");
        rhi->bindComputePipeline(sscsComputePipeline);
        rhi->setComputeTexture(0, depthStencilRT);
        rhi->setComputeTexture(1, sscsRT);
        rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBuffer(1, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
        rhi->setComputeBytes(&screenSize, sizeof(glm::vec2), 2);
        rhi->setComputeBytes(&params, sizeof(params), 3);
        rhi->dispatch(w, h, 1);  // 1x1 groups, matching raytraceShadowPass
        rhi->endComputePass();
        return;
    }

    // Vulkan: fullscreen fragment path with SSCS.frag.
    if (!vkSscsPipeline.isValid()) return;

    struct SSCSPush {
        glm::vec4 lightDirVS;   // view-space direction TO the light
        float rayLength;
        float thickness;
        Uint32 stepCount;
        float bias;
    } push;
    glm::vec3 Lworld = glm::normalize(-directionalLights[0].direction);   // toward light
    glm::vec3 Lview  = glm::normalize(glm::mat3(currentCamera.view) * Lworld);
    push.lightDirVS = glm::vec4(Lview, 0.0f);
    push.rayLength  = sscsLength;
    push.thickness  = sscsThickness;
    push.stepCount  = sscsSteps;
    push.bias       = sscsBias;

    RenderPassDesc rp;
    rp.name = "SSCS";
    rp.colorAttachments.push_back(sscsRT);
    rp.clearColors.push_back(glm::vec4(1.0f));  // default lit
    rp.loadColor.push_back(false);              // clear
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(vkSscsPipeline);
    rhi->setTexture(0, 0, depthStencilRT, clampSampler);
    rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->setFragmentBytes(&push, sizeof(push), 0);
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();
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
    // Threshold is dynamic on BOTH backends: Metal 3d_bloom_brightness.metal
    // reads buffer(0); Vulkan BloomBright.frag reads push-constant offset 64
    // (the setFragmentBytes(binding=0) slot).
    rhi->setFragmentBytes(&bloomThreshold, sizeof(float), 0);
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
        if (backend == GraphicsBackend::Metal) {
            // 3d_bloom_upsample.metal blends in-shader: texBlend at texture(1)
            // is the level being written (same feedback the native pass uses).
            rhi->setTexture(0, 1, bloomPyramid[i], clampSampler);
        }
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    }

    // Metal-only: composite the accumulated bloom over the scene now
    // (native BloomCompositePass) — colorRT + pyramid[0] -> tempColorRT,
    // then swap so downstream passes see the composited scene. The Vulkan
    // twin composites inside PostProcess.frag instead.
    if (backend == GraphicsBackend::Metal && bloomCompositePipeline.isValid() &&
        colorRT.isValid() && tempColorRT.isValid() && bloomPyramid[0].isValid()) {
        RenderPassDesc rp;
        rp.name = "BloomComposite";
        rp.colorAttachments.push_back(tempColorRT);
        rp.clearColors.push_back(glm::vec4(0.0f));
        rp.loadColor.push_back(false);
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(bloomCompositePipeline);
        rhi->setTexture(0, 0, colorRT, clampSampler);
        rhi->setTexture(0, 1, bloomPyramid[0], clampSampler);
        rhi->setFragmentBytes(&bloomStrength, sizeof(float), 0);
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
        std::swap(colorRT, tempColorRT);
    }
}

// Sky/atmosphere pass: physically-based Rayleigh/Mie scattering rendered into
// the HDR colorRT, depth-tested so it only fills background pixels (where the
// main pass left depth at the far plane). Runs after Main, before bloom, so the
// bright sky/sun participate in bloom.
void Renderer::skyAtmospherePass() {
    // SkyType::HDRI samples the captured environment cubemap; everything else
    // marches the procedural atmosphere. The cubemap path degrades to atmosphere
    // where its pipeline/texture aren't available (e.g. the RHI-Metal path until
    // its own slice), so this stays safe on every backend.
    const bool useCubemapSky = (m_skyType == SkyType::HDRI) &&
                               skyboxPipeline.isValid() && environmentCubemap.isValid();
    const bool useGradientSky = (m_skyType == SkyType::Gradient) &&
                                gradientPipeline.isValid() && gradientDataBuffer.isValid();
    if (!useCubemapSky && !useGradientSky && !atmospherePipeline.isValid()) return;
    if (!colorRT.isValid() || !depthStencilRT.isValid() || !atmosphereDataBuffer.isValid()) {
        return;
    }
    RenderPassDesc rp;
    rp.name = "SkyAtmosphere";
    rp.colorAttachments.push_back(colorRT);
    rp.loadColor.push_back(true);      // preserve the rendered scene
    rp.depthAttachment = depthStencilRT;
    rp.loadDepth = true;               // test against the scene depth (no writes)
    rhi->beginRenderPass(rp);
    if (useCubemapSky) {
        rhi->bindPipeline(skyboxPipeline);
        rhi->setTexture(0, 0, environmentCubemap, clampSampler);   // cube: set2/binding0 (Vk), texture(0) (Metal)
        // 3d_skybox.metal reads camera at buffer(0); the GLSL twin at set1/binding3.
        rhi->setFragmentBuffer(backend == GraphicsBackend::Metal ? 0 : 3,
                               cameraUniformBuffer, 0, sizeof(CameraRenderData));
    } else if (useGradientSky) {
        rhi->bindPipeline(gradientPipeline);
        if (backend == GraphicsBackend::Metal) {
            // 3d_gradient.metal: camera at buffer(0), gradient colors at buffer(1).
            rhi->setFragmentBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
            rhi->setFragmentBuffer(1, gradientDataBuffer, 0, sizeof(GradientRenderData));
        } else {
            // Gradient.frag: colors at set0/binding0, camera at set1/binding3.
            rhi->setFragmentBuffer(0, gradientDataBuffer, 0, sizeof(GradientRenderData));
            rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        }
    } else {
        rhi->bindPipeline(atmospherePipeline);
        if (backend == GraphicsBackend::Metal) {
            // 3d_atmosphere.metal: camera(0), atmosphere(1), night sky(2).
            rhi->setFragmentBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
            rhi->setFragmentBuffer(1, atmosphereDataBuffer, 0, sizeof(AtmosphereRenderData));
            rhi->setFragmentBuffer(2, nightSkyDataBuffer, 0, sizeof(NightSkyRenderData));
        } else {
            // Atmosphere.frag: atmosphere set1/b0, night sky set1/b2, camera set1/b3.
            rhi->setFragmentBuffer(0, atmosphereDataBuffer, 0, sizeof(AtmosphereRenderData));
            rhi->setFragmentBuffer(2, nightSkyDataBuffer, 0, sizeof(NightSkyRenderData));
            rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        }
    }
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();
}

// God rays: screen-space radial light scattering from the sun's projected
// position, marching over the scene color/depth and accumulating sky luminance.
// Rendered into the half-res lightScatteringRT and composited in PostProcess.
void Renderer::lightScatteringPass() {
    if (!lightScatteringEnabled) return;
    if (!lightScatteringPipeline.isValid() || !lightScatteringRT.isValid() ||
        !colorRT.isValid() || !depthStencilRT.isValid() || !lightScatteringDataBuffer.isValid()) {
        return;
    }

    // Project the sun (at infinity along sunDirection) to screen UV.
    // Tunables come from the persistent, panel-editable settings; only the
    // per-frame fields below are overwritten.
    LightScatteringRenderData ls = lightScatteringSettings;
    ls.sunColor = atmosphereData.sunColor;
    ls.screenSize = glm::vec2(std::max(1u, rhi->getSwapchainWidth() / 2),
                              std::max(1u, rhi->getSwapchainHeight() / 2));
    glm::vec3 sunDir = glm::normalize(atmosphereData.sunDirection);
    glm::vec3 sunWorldPos = currentCamera.position + sunDir * 10000.0f;
    glm::vec4 sunClip = (currentCamera.proj * currentCamera.view) * glm::vec4(sunWorldPos, 1.0f);
    if (sunClip.w > 0.0f) {
        glm::vec2 ndc = glm::vec2(sunClip) / sunClip.w;
        ls.sunScreenPos = ndc * 0.5f + 0.5f;
        ls.sunScreenPos.y = 1.0f - ls.sunScreenPos.y;  // match FullScreen.vert Y-down UV
    } else {
        ls.sunScreenPos = glm::vec2(-10.0f);  // behind camera -> shader outputs 0
    }
    rhi->updateBuffer(lightScatteringDataBuffer, &ls, 0, sizeof(ls));

    RenderPassDesc rp;
    rp.name = "LightScattering";
    rp.colorAttachments.push_back(lightScatteringRT);
    rp.clearColors.push_back(glm::vec4(0.0f));
    rp.loadColor.push_back(false);  // clear
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(lightScatteringPipeline);
    rhi->setTexture(0, 0, colorRT, clampSampler);
    rhi->setTexture(0, 1, depthStencilRT, clampSampler);
    rhi->setFragmentBuffer(0, lightScatteringDataBuffer, 0, sizeof(LightScatteringRenderData));
    if (backend == GraphicsBackend::Metal) {
        // 3d_light_scattering.metal reads FrameData at buffer(1) for jitter.
        rhi->setFragmentBuffer(1, frameDataBuffer, 0, sizeof(FrameData));
    } else {
        rhi->setFragmentBytes(&frameCounter, sizeof(Uint32), 7);  // push offset 112
    }
    frameCounter++;
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();
}

// Cheap analytic exponential height fog: one fullscreen pass, no shadows/lights.
// Reads colorRT + depth, writes tempColorRT, then swaps so downstream passes
// (bloom/god rays/post) see the fogged scene. The expensive per-light volumetric
// variant is volumetricFogPass (now ECS-driven).
void Renderer::heightFogPass() {
    if (!heightFogEnabled || !heightFogPipeline.isValid() ||
        !colorRT.isValid() || !tempColorRT.isValid() || !depthStencilRT.isValid() ||
        !heightFogDataBuffer.isValid()) {
        return;
    }

    // Tunables from the persistent settings; per-frame fields (matrices/camera/
    // sun) overwritten below. Sun follows the atmosphere/sky (light-driven).
    HeightFogRenderData hf = heightFogSettings;
    hf.invViewProj = glm::inverse(currentCamera.proj * currentCamera.view);
    hf.cameraPosition = glm::vec4(currentCamera.position, 0.0f);
    hf.sunDirection = glm::vec4(glm::normalize(atmosphereData.sunDirection), 0.0f);
    hf.sunColorIntensity = glm::vec4(atmosphereData.sunColor, atmosphereData.sunIntensity);
    rhi->updateBuffer(heightFogDataBuffer, &hf, 0, sizeof(hf));

    RenderPassDesc rp;
    rp.name = "HeightFog";
    rp.colorAttachments.push_back(tempColorRT);
    rp.loadColor.push_back(false);  // every pixel written
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(heightFogPipeline);
    rhi->setTexture(0, 0, colorRT, clampSampler);
    rhi->setTexture(0, 1, depthStencilRT, clampSampler);
    if (backend == GraphicsBackend::Metal) {
        // 3d_height_fog.metal reads HeightFogData at buffer(0) (inline bytes).
        rhi->setFragmentBytes(&hf, sizeof(hf), 0);
    } else {
        rhi->setFragmentBuffer(0, heightFogDataBuffer, 0, sizeof(HeightFogRenderData));
    }
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();

    std::swap(colorRT, tempColorRT);  // colorRT now holds the fogged scene
}

// Simple height/distance fog: read colorRT + depth, write the fogged result to
// tempColorRT, then swap so downstream passes (bloom/god rays/post) see it.
void Renderer::volumetricFogPass() {
    if (!volumetricFogEnabled || !volumetricFogPipeline.isValid() ||
        !colorRT.isValid() || !tempColorRT.isValid() || !depthStencilRT.isValid() ||
        !fogDataBuffer.isValid() ||
        // The raymarch reads the shadow cascades and every light list.
        !pssmShadowArrayTexture.isValid() || !pssmDataBuffer.isValid() ||
        !clusterBuffer.isValid() || !pointLightBuffer.isValid() ||
        !spotLightBuffer.isValid() || !rectLightBuffer.isValid()) {
        return;
    }

    // Tunables from the persistent panel-editable settings; per-frame fields
    // (matrices/camera/sun) overwritten below.
    FogRenderData fog = fogSettings;
    fog.invViewProj = glm::inverse(currentCamera.proj * currentCamera.view);
    fog.cameraPosition = currentCamera.position;
    fog.sunDirection = glm::normalize(atmosphereData.sunDirection);
    fog.sunColor = atmosphereData.sunColor;
    fog.sunIntensity = atmosphereData.sunIntensity;
    // Animate the density-noise scroll; the per-medium windSpeed coefficient is
    // scaled by the shared wind strength (windDirection is carried from setWind).
    fogSettings.time += 1.0f / 60.0f;
    fog.time = fogSettings.time;
    fog.windSpeed = fogSettings.windSpeed * m_windStrength;

    // Pack the fog volume array (global + bounded banks) for the Vulkan raymarch.
    // windSpeed folds in the shared wind strength (matches the fog buffer above).
    Uint32 volCount = static_cast<Uint32>(volumetricFogVolumes.size());
    if (volCount > static_cast<Uint32>(kMaxFogVolumes)) volCount = kMaxFogVolumes;
    fog.volumeCount = volCount;
    if (volCount > 0 && fogVolumeBuffer.isValid()) {
        VolumetricFogVolumeGPU gpuVols[kMaxFogVolumes] = {};
        for (Uint32 i = 0; i < volCount; ++i) {
            const auto& s = volumetricFogVolumes[i];
            gpuVols[i].boundsMin = glm::vec4(s.boundsMin, s.bounded ? 1.0f : 0.0f);
            gpuVols[i].boundsMax = glm::vec4(s.boundsMax, s.edgeFalloff);
            gpuVols[i].densityParams = glm::vec4(s.density, s.heightFalloff, s.baseHeight, s.maxHeight);
            gpuVols[i].albedoBlend = glm::vec4(s.albedo, s.blendWeight);
            gpuVols[i].phaseNoise = glm::vec4(s.anisotropy, s.ambientIntensity, s.noiseScale, s.noiseIntensity);
            gpuVols[i].wind = glm::vec4(s.windSpeed * m_windStrength, 0.0f, 0.0f, 0.0f);
        }
        rhi->updateBuffer(fogVolumeBuffer, gpuVols,
                          0, sizeof(VolumetricFogVolumeGPU) * volCount);
    }
    rhi->updateBuffer(fogDataBuffer, &fog, 0, sizeof(fog));

    // ── Froxel path: inject (compute) -> integrate (compute) -> composite.
    // Decouples fog cost from screen resolution. Backend-agnostic — the Metal
    // kernels (3d_volumetric_fog.metal) and the Vulkan .comp twins (FroxelInject/
    // Integrate.comp) bind identically through the RHI compute API, so this is
    // gated purely on the froxel resources existing, not on the backend. ──
    const bool froxelReady = fogUseFroxel
        && volCount > 0
        && fogFroxelInjectPipeline.isValid() && fogFroxelIntegratePipeline.isValid()
        && fogFroxelCompositePipeline.isValid()
        && fogFroxelGridTexture.isValid() && fogIntegratedVolumeTexture.isValid()
        && fogFroxelGlobalsBuffer.isValid();
    if (froxelReady) {
        // Froxel globals in the VolumetricFogData layout the .metal kernels read
        // (distinct from the FogRenderData `fog` the raymarch uses).
        VolumetricFogData fg{};
        fg.invViewProj    = fog.invViewProj;
        fg.cameraPosition = fog.cameraPosition;
        fg.sunDirection   = fog.sunDirection;
        fg.sunColor       = fog.sunColor;
        fg.sunIntensity   = fog.sunIntensity;
        fg.nearPlane      = currentCamera.nearPlane;
        fg.farPlane       = currentCamera.farPlane;
        fg.screenSize     = glm::vec2(rhi->getSwapchainWidth(), rhi->getSwapchainHeight());
        fg.time           = fog.time;
        fg.windDirection  = fog.windDirection;
        fg.windSpeed      = fog.windSpeed;   // already scaled by wind strength
        fg.volumeCount    = volCount;
        rhi->updateBuffer(fogFroxelGlobalsBuffer, &fg, 0, sizeof(fg));

        glm::uvec4 fogLightParams(clusterGridSizeX, clusterGridSizeY,
                                  static_cast<Uint32>(spotLights.size()),
                                  static_cast<Uint32>(rectLights.size()));

        // Inject + integrate. One compute pass; Metal serializes the two
        // dispatches and barriers the froxel grid between them (computeBarrier is
        // a no-op there, and the correct fence once the Vulkan path is wired).
        rhi->beginComputePass("FroxelFog");
        rhi->bindComputePipeline(fogFroxelInjectPipeline);
        rhi->setComputeBuffer(0, fogFroxelGlobalsBuffer, 0, sizeof(VolumetricFogData));
        rhi->setComputeBuffer(1, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBuffer(2, pssmDataBuffer, 0, sizeof(PSSMRenderData));
        rhi->setComputeBuffer(3, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
        rhi->setComputeBuffer(4, clusterBuffer);
        rhi->setComputeBuffer(5, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
        rhi->setComputeBuffer(6, rectLightBuffer);
        rhi->setComputeBytes(&fogLightParams, sizeof(glm::uvec4), 7);
        // Fog volumes: Metal uses buffer index 8 ([[buffer(8)]], past the light-
        // params bytes at 7). Vulkan has only 8 descriptor slots (0-7) and its
        // light params are a push constant (not a descriptor), so it binds b7.
        rhi->setComputeBuffer(backend == GraphicsBackend::Metal ? 8u : 7u,
                              fogVolumeBuffer, 0, sizeof(VolumetricFogVolumeGPU) * kMaxFogVolumes);
        rhi->setComputeTexture(0, fogFroxelGridTexture);       // storage write (Metal [[texture(0)]] / Vk set1 b0)
        // Shadow map: Metal samples a plain [[texture(1)]] with the kernel's inline
        // sampler; Vulkan needs a combined image sampler (set 2) it can textureLod.
        if (backend == GraphicsBackend::Metal) {
            rhi->setComputeTexture(1, pssmShadowArrayTexture);
        } else {
            rhi->setComputeSampledTexture(1, pssmShadowArrayTexture, shadowSampler);
        }
        rhi->dispatch((FROXEL_GRID_X + 3) / 4, (FROXEL_GRID_Y + 3) / 4, (FROXEL_GRID_Z + 3) / 4);

        rhi->computeBarrier();
        rhi->bindComputePipeline(fogFroxelIntegratePipeline);
        rhi->setComputeBuffer(0, fogFroxelGlobalsBuffer, 0, sizeof(VolumetricFogData));
        rhi->setComputeTexture(0, fogFroxelGridTexture);       // read [[texture(0)]]
        rhi->setComputeTexture(1, fogIntegratedVolumeTexture); // write [[texture(1)]]
        rhi->dispatch((FROXEL_GRID_X + 7) / 8, (FROXEL_GRID_Y + 7) / 8, 1);
        rhi->endComputePass();

        // The integrate pass wrote the volume as a storage image (GENERAL); the
        // composite samples it. Transition + barrier before the render pass (a
        // no-op on Metal, which tracks the hazard). Must be outside a render pass.
        rhi->prepareTextureForSampling(fogIntegratedVolumeTexture);

        // Composite: sample the integrated volume, ping-pong colorRT.
        RenderPassDesc crp;
        crp.name = "FroxelFogComposite";
        crp.colorAttachments.push_back(tempColorRT);
        crp.loadColor.push_back(false);
        rhi->beginRenderPass(crp);
        rhi->bindPipeline(fogFroxelCompositePipeline);
        rhi->setTexture(0, 0, colorRT, clampSampler);
        rhi->setTexture(0, 1, depthStencilRT, clampSampler);
        rhi->setTexture(0, 2, fogIntegratedVolumeTexture, clampSampler);
        rhi->setFragmentBuffer(0, fogFroxelGlobalsBuffer, 0, sizeof(VolumetricFogData));
        rhi->setFragmentBuffer(1, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
        std::swap(colorRT, tempColorRT);
        return;
    }

    RenderPassDesc rp;
    rp.name = "VolumetricFog";
    rp.colorAttachments.push_back(tempColorRT);
    rp.loadColor.push_back(false);  // every pixel written
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(volumetricFogPipeline);
    rhi->setTexture(0, 0, colorRT, clampSampler);
    rhi->setTexture(0, 1, depthStencilRT, clampSampler);
    if (backend == GraphicsBackend::Metal) {
        // simpleFogFragment reads the full VolumetricFogData layout at
        // buffer(0) (MSL float3 = 16 bytes) plus CameraData at buffer(1).
        // Mirror the MSL struct exactly; only the fields the simple fog path
        // reads are meaningful, the froxel/temporal tail just pads the size.
        struct alignas(16) MetalFogData {
            glm::mat4 invViewProj{1.0f};
            glm::mat4 prevViewProj{1.0f};
            glm::vec4 cameraPosition{0.0f};
            glm::vec4 sunDirection{0.0f};
            glm::vec4 sunColorPad{1.0f};   // float3 slot; intensity follows
            float sunIntensity = 1.0f;
            float fogDensity = 0.0f;
            float fogHeightFalloff = 0.0f;
            float fogBaseHeight = 0.0f;
            float fogMaxHeight = 100.0f;
            float scatteringCoeff = 0.0f;
            float extinctionCoeff = 0.0f;
            float anisotropy = 0.0f;
            float ambientIntensity = 0.0f;
            float nearPlane = 0.1f;
            float farPlane = 1000.0f;
            float _align0 = 0.0f;          // aligns screenSize to 8
            glm::vec2 screenSize{1.0f};
            Uint32 frameIndex = 0;
            float temporalBlend = 0.0f;
            float noiseScale = 0.0f;
            float noiseIntensity = 0.0f;
            float windSpeed = 0.0f;
            float time = 0.0f;
            glm::vec4 windDirection{0.0f};
        } mfd;
        mfd.invViewProj = fog.invViewProj;
        mfd.cameraPosition = glm::vec4(fog.cameraPosition, 0.0f);
        mfd.sunDirection = glm::vec4(fog.sunDirection, 0.0f);
        mfd.sunColorPad = glm::vec4(fog.sunColor, 0.0f);
        mfd.sunIntensity = fog.sunIntensity;
        mfd.fogDensity = fog.fogDensity;
        mfd.fogHeightFalloff = fog.fogHeightFalloff;
        mfd.fogBaseHeight = fog.fogBaseHeight;
        mfd.fogMaxHeight = fog.fogMaxHeight;
        mfd.anisotropy = fog.anisotropy;
        mfd.ambientIntensity = fog.ambientIntensity;
        mfd.nearPlane = currentCamera.nearPlane;
        mfd.farPlane = currentCamera.farPlane;
        mfd.screenSize = glm::vec2(rhi->getSwapchainWidth(), rhi->getSwapchainHeight());
        mfd.noiseScale = fog.noiseScale;
        mfd.noiseIntensity = fog.noiseIntensity;
        mfd.windSpeed = fog.windSpeed;   // already scaled by wind strength
        mfd.time = fog.time;
        mfd.windDirection = glm::vec4(fog.windDirection, 0.0f);
        rhi->setFragmentBytes(&mfd, sizeof(mfd), 0);
        rhi->setFragmentBuffer(1, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        // Volumetric raymarch inputs: PSSM cascades for sun shafts + the full
        // light set (tile-culled points, spots, rects).
        rhi->setTexture(0, 2, pssmShadowArrayTexture, shadowSampler);
        rhi->setFragmentBuffer(2, pssmDataBuffer, 0, sizeof(PSSMRenderData));
        rhi->setFragmentBuffer(3, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
        rhi->setFragmentBuffer(4, clusterBuffer);
        rhi->setFragmentBuffer(5, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
        rhi->setFragmentBuffer(6, rectLightBuffer);
        glm::uvec4 fogLightParams(clusterGridSizeX, clusterGridSizeY,
                                  static_cast<Uint32>(spotLights.size()),
                                  static_cast<Uint32>(rectLights.size()));
        rhi->setFragmentBytes(&fogLightParams, sizeof(glm::uvec4), 7);
    } else {
        rhi->setFragmentBuffer(0, fogDataBuffer, 0, sizeof(FogRenderData));
        rhi->setFragmentBuffer(1, pssmDataBuffer, 0, sizeof(PSSMRenderData));
        rhi->setFragmentBuffer(2, clusterBuffer);
        rhi->setFragmentBuffer(3, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
        rhi->setFragmentBuffer(4, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
        rhi->setFragmentBuffer(5, rectLightBuffer);
        // Fog volume array (bounded banks + blend). Always bound — the shader
        // falls back to legacy single-volume height fog when volumeCount == 0.
        rhi->setFragmentBuffer(6, fogVolumeBuffer, 0, sizeof(VolumetricFogVolumeGPU) * kMaxFogVolumes);
        rhi->setTexture(0, 2, pssmShadowArrayTexture, shadowSampler);
        glm::uvec4 fogLightParams(clusterGridSizeX, clusterGridSizeY,
                                  static_cast<Uint32>(spotLights.size()),
                                  static_cast<Uint32>(rectLights.size()));
        rhi->setFragmentBytes(&fogLightParams, sizeof(glm::uvec4), 0);  // push offset 64
    }
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();

    std::swap(colorRT, tempColorRT);  // colorRT now holds the fogged scene
}

// Heterogeneous volume raymarch (EmberGen-style density grid in an AABB):
// read colorRT + depth, march the 3D density texture with sun single
// scattering (PSSM tap + self-shadow march), write to tempColorRT, swap.
// Rendering only — EmberGen import/parsing lives in a separate PR; until then
// the procedural test grid stands in (panel: Effects > Volume).
void Renderer::volumeRaymarchPass() {
    if (!volumeRenderEnabled || !volumeRaymarchPipeline.isValid() ||
        !volumeDensityTexture.isValid() || !volumeDataBuffer.isValid() ||
        !colorRT.isValid() || !tempColorRT.isValid() || !depthStencilRT.isValid() ||
        !pssmShadowArrayTexture.isValid() || !pssmDataBuffer.isValid()) {
        return;
    }

    VolumeRenderData vol = volumeSettings;
    vol.invViewProj = glm::inverse(currentCamera.proj * currentCamera.view);
    vol.cameraPosition = glm::vec4(currentCamera.position, 0.0f);
    vol.sunDirection = glm::vec4(glm::normalize(atmosphereData.sunDirection), 0.0f);
    vol.sunColor = glm::vec4(atmosphereData.sunColor, atmosphereData.sunIntensity);

    RenderPassDesc rp;
    rp.name = "VolumeRaymarch";
    rp.colorAttachments.push_back(tempColorRT);
    rp.loadColor.push_back(false);  // every pixel written
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(volumeRaymarchPipeline);
    rhi->setTexture(0, 0, colorRT, clampSampler);
    rhi->setTexture(0, 1, depthStencilRT, clampSampler);
    rhi->setTexture(0, 2, pssmShadowArrayTexture, shadowSampler);
    rhi->setTexture(0, 3, volumeDensityTexture, clampSampler);
    if (backend == GraphicsBackend::Metal) {
        // VolumeRenderData is vec4-only, so the C++ struct matches the MSL
        // twin byte-for-byte and can travel as immediate bytes.
        rhi->setFragmentBytes(&vol, sizeof(vol), 0);
        rhi->setFragmentBuffer(1, pssmDataBuffer, 0, sizeof(PSSMRenderData));
    } else {
        rhi->updateBuffer(volumeDataBuffer, &vol, 0, sizeof(vol));
        rhi->setFragmentBuffer(0, volumeDataBuffer, 0, sizeof(VolumeRenderData));
        rhi->setFragmentBuffer(1, pssmDataBuffer, 0, sizeof(PSSMRenderData));
    }
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();

    std::swap(colorRT, tempColorRT);  // colorRT now holds the composited volume
}

// Volume Rendering API — the hook a future EmberGen import PR calls with
// decoded voxel data. R8_UNORM slice-major grid -> sampled 3D texture.
TextureHandle Renderer::createVolumeTexture(Uint32 width, Uint32 height, Uint32 depth,
                                            const void* data, size_t size) {
    if (!rhi || width == 0 || height == 0 || depth <= 1 || !data ||
        size < size_t(width) * height * depth) {
        return {};
    }
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.format = PixelFormat::R8_UNORM;
    desc.usage = TextureUsage::Sampled;
    TextureHandle tex = rhi->createTexture(desc);
    if (tex.isValid()) {
        rhi->updateTexture(tex, data, size, 0, 0);
    }
    return tex;
}

void Renderer::setVolumeDensity(TextureHandle density, const glm::vec3& boxMin,
                                const glm::vec3& boxMax) {
    if (density.isValid()) {
        volumeDensityTexture = density;
        volumeSettings.boxMin = glm::vec4(boxMin, volumeSettings.boxMin.w);
        volumeSettings.boxMax = glm::vec4(boxMax, volumeSettings.boxMax.w);
    } else {
        volumeDensityTexture = volumeTestTexture;  // back to the built-in grid
    }
}

// Camera-motion velocity (motion vectors) from the depth buffer. Standalone
// infrastructure for a future TAA / motion-blur consumer; produces velocityRT.
void Renderer::velocityPass() {
    // Metal: the native velocity is a compute kernel (3d_velocity.metal —
    // depth at texture(0), velocityRT written at texture(1), camera buffer(0),
    // prevViewProj buffer(1), 8x8 threadgroups with in-kernel bounds checks).
    if (backend == GraphicsBackend::Metal) {
        if (!velocityComputePipeline.isValid() || !velocityRT.isValid() ||
            !depthStencilRT.isValid() || !prevViewProjBuffer.isValid()) {
            return;
        }
        glm::mat4 curViewProj = currentCamera.proj * currentCamera.view;
        if (!prevViewProjValid) { prevViewProj = curViewProj; prevViewProjValid = true; }
        rhi->updateBuffer(prevViewProjBuffer, &prevViewProj, 0, sizeof(glm::mat4));

        rhi->beginComputePass("Velocity");
        rhi->bindComputePipeline(velocityComputePipeline);
        rhi->setComputeTexture(0, depthStencilRT);
        rhi->setComputeTexture(1, velocityRT);
        rhi->setComputeBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        rhi->setComputeBuffer(1, prevViewProjBuffer, 0, sizeof(glm::mat4));
        Uint32 w = rhi->getSwapchainWidth(), h = rhi->getSwapchainHeight();
        rhi->dispatch((w + 7) / 8, (h + 7) / 8, 1);
        rhi->endComputePass();

        prevViewProj = curViewProj;
        return;
    }

    if (!velocityPipeline.isValid() || !velocityRT.isValid() ||
        !depthStencilRT.isValid() || !prevViewProjBuffer.isValid()) {
        return;
    }
    glm::mat4 curViewProj = currentCamera.proj * currentCamera.view;
    if (!prevViewProjValid) { prevViewProj = curViewProj; prevViewProjValid = true; }
    rhi->updateBuffer(prevViewProjBuffer, &prevViewProj, 0, sizeof(glm::mat4));

    RenderPassDesc rp;
    rp.name = "Velocity";
    rp.colorAttachments.push_back(velocityRT);
    rp.clearColors.push_back(glm::vec4(0.0f));
    rp.loadColor.push_back(false);  // clear
    rhi->beginRenderPass(rp);
    rhi->bindPipeline(velocityPipeline);
    rhi->setTexture(0, 0, depthStencilRT, clampSampler);
    rhi->setFragmentBuffer(0, prevViewProjBuffer, 0, sizeof(glm::mat4));
    rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
    rhi->draw(3, 1, 0, 0);
    rhi->endRenderPass();

    prevViewProj = curViewProj;  // roll forward for next frame
}

// GPU particle system: simulate (force -> integrate compute) then render as
// instanced billboards into colorRT — one draw per ParticleDrawPacket, each
// with its own blend pipeline + texture (per-material draws).
void Renderer::particlePass() {
    if (particleCount == 0) return;
    if (!particleForcePipeline.isValid() || !particleIntegratePipeline.isValid() ||
        !particleRenderPipelines[0].isValid() || !particleBuffer.isValid()) {
        return;
    }

    const size_t bufBytes = sizeof(GPUParticleData) * particleCount;
    const Uint32 groups = (particleCount + 255) / 256;
    const bool metal = (backend == GraphicsBackend::Metal);

    // ── Simulate ──────────────────────────────────────────────────────────────
    // Pause: skip the sim entirely — with deltaTime=0 the compute would be a
    // no-op anyway, so we save the dispatch and just leave state frozen.
    if (!m_particleSimPaused) {
        ParticleSimParams sp;
        sp.resolution = glm::vec2(rhi->getSwapchainWidth(), rhi->getSwapchainHeight());
        sp.time = float(frameCounter) / 60.0f;
        sp.deltaTime = 1.0f / 60.0f;
        sp.particleCount = particleCount;
        sp.wind      = m_forceField.wind;
        sp.turbulence = glm::vec4(0.0f, 0.0f, 0.0f, m_forceField.turbulence);

        const auto& attractors = m_forceField.attractors;
        sp.attractorCount = static_cast<Uint32>(attractors.size());
        rhi->updateBuffer(particleSimParamsBuffer, &sp, 0, sizeof(sp));
        if (!attractors.empty())
            rhi->updateBuffer(particleAttractorBuffer, attractors.data(), 0,
                              attractors.size() * sizeof(ParticleAttractor));

        // Compute: force writes p.force, then integrate reads it -> barrier between.
        // 3d_particle.metal orders the kernel buffers particles(0)/params(1)/
        // attractor(2); the Vulkan .comp uses params(0)/attractor(1)/particles(2).
        rhi->beginComputePass("ParticleSim");
        rhi->bindComputePipeline(particleForcePipeline);
        if (metal) {
            rhi->setComputeBuffer(0, particleBuffer, 0, bufBytes);
            rhi->setComputeBuffer(1, particleSimParamsBuffer, 0, sizeof(ParticleSimParams));
            rhi->setComputeBuffer(2, particleAttractorBuffer, 0, sizeof(ParticleAttractor) * MAX_PARTICLE_ATTRACTORS);
        } else {
            rhi->setComputeBuffer(0, particleSimParamsBuffer, 0, sizeof(ParticleSimParams));
            rhi->setComputeBuffer(1, particleAttractorBuffer, 0, sizeof(ParticleAttractor) * MAX_PARTICLE_ATTRACTORS);
            rhi->setComputeBuffer(2, particleBuffer, 0, bufBytes);
        }
        rhi->dispatch(groups, 1, 1);
        rhi->computeBarrier();
        rhi->bindComputePipeline(particleIntegratePipeline);
        if (metal) {
            rhi->setComputeBuffer(0, particleBuffer, 0, bufBytes);
            rhi->setComputeBuffer(1, particleSimParamsBuffer, 0, sizeof(ParticleSimParams));
        } else {
            rhi->setComputeBuffer(0, particleSimParamsBuffer, 0, sizeof(ParticleSimParams));
            rhi->setComputeBuffer(2, particleBuffer, 0, bufBytes);
        }
        rhi->dispatch(groups, 1, 1);
        rhi->endComputePass();
        rhi->computeBarrier();  // sim writes -> vertex reads
    }

    // ── Render ────────────────────────────────────────────────────────────────
    // Hide: skip drawing but keep simulating, so unhiding is seamless.
    if (!particleVisible) return;

    // Per-emitter draw list from ParticleRenderSystem. Fallback: one full-range
    // additive draw when no packets were submitted (particles driven without
    // the ECS render system).
    std::vector<ParticleDrawPacket> fallbackDraws;
    const std::vector<ParticleDrawPacket>* draws = &m_particleDrawList;
    if (draws->empty()) {
        ParticleDrawPacket all;
        all.slotBegin = 0;
        all.slotCount = particleCount;
        fallbackDraws.push_back(all);
        draws = &fallbackDraws;
    }
    const Uint32 drawTotal = static_cast<Uint32>(
        std::min<size_t>(draws->size(), MAX_PARTICLE_DRAWS));

    // Indirect mode (follows the GPU-driven toggle): draw args come from a
    // buffer instead of CPU call parameters. The args are CPU-written today —
    // the point is the submission path, so a later GPU compact/cull pass can
    // overwrite instanceCount without touching this loop.
    const bool indirect = gpuDrivenIndirect() && particleDrawArgsBuffer.isValid();
    if (indirect) {
        // VkDrawIndirectCommand == MTLDrawPrimitivesIndirectArguments (16 B).
        struct DrawArgs { Uint32 vertexCount, instanceCount, firstVertex, firstInstance; };
        DrawArgs args[MAX_PARTICLE_DRAWS];
        for (Uint32 i = 0; i < drawTotal; ++i) {
            const auto& p = (*draws)[i];
            args[i] = { 6u, p.slotCount, 0u, p.slotBegin };
        }
        rhi->updateBuffer(particleDrawArgsBuffer, args, 0, sizeof(DrawArgs) * drawTotal);
    }

    // Instanced billboards (6 verts × slotCount per packet) into colorRT+depth.
    RenderPassDesc rp;
    rp.name = "Particles";
    rp.colorAttachments.push_back(colorRT);
    rp.loadColor.push_back(true);
    rp.depthAttachment = depthStencilRT;
    rp.loadDepth = true;
    rhi->beginRenderPass(rp);
    for (Uint32 i = 0; i < drawTotal; ++i) {
        const auto& p = (*draws)[i];
        if (p.slotCount == 0) continue;

        const Uint32 blend = std::min<Uint32>(p.blendMode, PARTICLE_BLEND_COUNT - 1);
        rhi->bindPipeline(particleRenderPipelines[blend]);

        // Per-packet texture; the default white texture keeps the sampler bound
        // while useTexture=0 selects the procedural soft-disc path in the shader.
        const bool hasTexture = p.texture != INVALID_TEXTURE_ID &&
                                p.texture < textures.size();
        const TextureId texId = hasTexture ? p.texture : defaultWhiteTexture;
        struct { float particleSize; float useTexture; float _pad[2]; }
            pc{ p.size, hasTexture ? 1.0f : 0.0f, {0.0f, 0.0f} };

        if (metal) {
            // particleVertex: camera(0), ParticlePushConstants(1), particles(2);
            // particleFragment: params in buffer(0), texture/sampler at 0.
            rhi->setVertexBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));
            rhi->setVertexBytes(&pc, sizeof(pc), 1);
            rhi->setFragmentBytes(&pc, sizeof(pc), 0);
            rhi->setVertexBuffer(2, particleBuffer, 0, bufBytes);
        } else {
            rhi->setVertexBuffer(0, cameraUniformBuffer, 0, sizeof(CameraRenderData));   // set0 b0
            rhi->setFragmentBuffer(0, particleBuffer, 0, bufBytes);                       // set1 b0 (read in VS)
            rhi->setVertexBytes(&pc, sizeof(pc), 0);  // pc [0,16), visible to VS+FS
        }
        if (texId < textures.size()) {
            const RenderTexture& tex = textures[texId];
            rhi->setTexture(2, 0, tex.handle, tex.sampler);  // GLSL set2 b0 / MSL texture(0)
        }

        if (indirect) {
            rhi->drawIndirect(particleDrawArgsBuffer, static_cast<size_t>(i) * 16u, 1, 16);
        } else {
            // firstInstance = slotBegin: gl_InstanceIndex / [[instance_id]]
            // include the base instance on both backends, indexing the SSBO
            // directly at the emitter's slot range.
            rhi->draw(6, p.slotCount, 0, p.slotBegin);
        }
    }
    rhi->endRenderPass();
}

// ============================================================================
// ECS Particle Integration API
// ============================================================================

void Renderer::ensureParticleFreeList() {
    if (!m_particleFreeListInitialized) {
        m_particleSlotFreeList.push_back({0u, MAX_PARTICLES});
        m_particleFreeListInitialized = true;
    }
}

uint32_t Renderer::allocParticleSlots(uint32_t count) {
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

void Renderer::freeParticleSlots(uint32_t slotBegin, uint32_t count) {
    if (count == 0) return;
    m_particleSlotFreeList.push_back({slotBegin, count});
    // Coalesce adjacent ranges by sorting and merging
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

uint32_t Renderer::claimParticleSlots(uint32_t count) {
    uint32_t begin = allocParticleSlots(count);
    if (begin != ~0u) {
        // Expand dispatch range to cover newly claimed slots.
        particleCount = std::max(particleCount, begin + count);
    }
    return begin;
}

void Renderer::releaseParticleSlots(uint32_t slotBegin, uint32_t count) {
    freeParticleSlots(slotBegin, count);
    // Zero-clear the released GPU slots. A freed mid-buffer range stays within
    // particleCount and would otherwise keep rendering stale particles; zeroing
    // makes age=0 >= lifetime=0, so the compute passes skip them immediately.
    if (count > 0 && particleBuffer.isValid()) {
        std::vector<GPUParticleData> blank(count);
        rhi->updateBuffer(particleBuffer, blank.data(),
                          slotBegin * sizeof(GPUParticleData),
                          count * sizeof(GPUParticleData));
    }
    // Recompute the high-water mark: the tail [particleCount, MAX_PARTICLES) must
    // be entirely free. Find the free range that reaches the end of the pool; its
    // start is the new mark (0 when the whole pool is free). This is order-
    // independent — unlike shrinking only when the freed range touched the mark,
    // which left it stuck if a middle range was freed before the tail.
    uint32_t hw = MAX_PARTICLES;
    for (const auto& r : m_particleSlotFreeList) {
        if (r.begin + r.count == MAX_PARTICLES) { hw = r.begin; break; }
    }
    particleCount = hw;
}

void Renderer::uploadParticles(uint32_t slotBegin, const std::vector<GPUParticleData>& particles) {
    if (slotBegin == ~0u || particles.empty()) return;
    if (slotBegin + particles.size() > MAX_PARTICLES) return;
    if (!particleBuffer.isValid()) return;
    rhi->updateBuffer(particleBuffer,
                      particles.data(),
                      slotBegin * sizeof(GPUParticleData),
                      particles.size() * sizeof(GPUParticleData));
}

void Renderer::setParticleForceField(const ParticleForceField& field) {
    m_forceField = field;
    if (m_forceField.attractors.size() > MAX_PARTICLE_ATTRACTORS)
        m_forceField.attractors.resize(MAX_PARTICLE_ATTRACTORS);
}

void Renderer::setSky(const SkyRenderData& sky) {
    // Visible sky mode: HDRI samples environmentCubemap in skyAtmospherePass;
    // Atmosphere (and, until S1, Gradient) march the procedural atmosphere.
    m_skyType = sky.type;
    // Push the atmosphere tunables into the CPU copy that is re-uploaded every
    // frame; the sun fields stay driven by directionalLights[0] (see the
    // per-frame sync in stage()/update()).
    atmosphereData.rayleighCoefficients  = sky.rayleighCoefficients;
    atmosphereData.rayleighScaleHeight   = sky.rayleighScaleHeight;
    atmosphereData.mieCoefficient        = sky.mieCoefficient;
    atmosphereData.mieScaleHeight        = sky.mieScaleHeight;
    atmosphereData.miePreferredDirection = sky.miePreferredDirection;
    atmosphereData.planetRadius          = sky.planetRadius;
    atmosphereData.atmosphereRadius      = sky.atmosphereRadius;
    atmosphereData.exposure              = sky.exposure;
    atmosphereData.groundColor           = sky.groundColor;
    // Gradient sky colors (used when type == Gradient). setSky only runs when the
    // SkyComponent is dirty, so re-upload the small unslotted buffer directly.
    gradientData.zenith  = glm::vec4(sky.gradientZenith, 1.0f);
    gradientData.horizon = glm::vec4(sky.gradientHorizon, 1.0f);
    gradientData.ground  = glm::vec4(sky.gradientGround, 1.0f);
    if (gradientDataBuffer.isValid()) {
        rhi->updateBuffer(gradientDataBuffer, &gradientData, 0, sizeof(gradientData));
    }
    // Night-sky (stars + moon) visuals for the Atmosphere pass.
    nightSkyData.moonColor      = glm::vec4(sky.moonColor, 0.0f);
    nightSkyData.starDensity    = sky.starDensity;
    nightSkyData.starBrightness = sky.starBrightness;
    nightSkyData.moonSize       = sky.moonSize;
    nightSkyData.moonBrightness = sky.moonBrightness;
    if (nightSkyDataBuffer.isValid()) {
        rhi->updateBuffer(nightSkyDataBuffer, &nightSkyData, 0, sizeof(nightSkyData));
    }
    iblNeedsUpdate = true;  // re-bake IBL from the new sky
}

void Renderer::setWind(const WindRenderData& wind) {
    // Shared wind direction drives both the cloud and fog scroll; the shared
    // strength scales each medium's per-medium windSpeed coefficient at fill
    // time. Fog now has wind-animated density noise (VolumetricFog.frag), at
    // parity with the native/Metal simpleFogFragment.
    cloudSettings.windDirection = wind.direction;
    fogSettings.windDirection = wind.direction;
    m_windStrength = wind.strength;
}

void Renderer::setVolumetricFogVolumes(const std::vector<VolumetricFogVolumeData>& volumes) {
    // ECS-resolved fog volumes: store them, gate the pass on whether any exist,
    // and mirror the first volume into fogSettings so the single-volume Metal
    // (simpleFogFragment) path and the panel stay populated. The Vulkan raymarch
    // blends the whole list (uploaded to fogVolumeBuffer in volumetricFogPass).
    volumetricFogVolumes = volumes;
    if (volumetricFogVolumes.size() > static_cast<size_t>(kMaxFogVolumes))
        volumetricFogVolumes.resize(kMaxFogVolumes);
    volumetricFogEnabled = !volumetricFogVolumes.empty();
    if (!volumetricFogVolumes.empty()) {
        const auto& v = volumetricFogVolumes.front();
        fogSettings.fogDensity       = v.density;
        fogSettings.fogHeightFalloff = v.heightFalloff;
        fogSettings.fogBaseHeight    = v.baseHeight;
        fogSettings.fogMaxHeight     = v.maxHeight;
        fogSettings.anisotropy       = v.anisotropy;
        fogSettings.ambientIntensity = v.ambientIntensity;
        fogSettings.noiseScale       = v.noiseScale;
        fogSettings.noiseIntensity   = v.noiseIntensity;
        fogSettings.windSpeed        = v.windSpeed;
    }
}

void Renderer::setParticleDrawList(const std::vector<ParticleDrawPacket>& draws) {
    m_particleDrawList = draws;
    if (m_particleDrawList.size() > MAX_PARTICLE_DRAWS)
        m_particleDrawList.resize(MAX_PARTICLE_DRAWS);
}

// Volumetric clouds (port of the Metal quarter-res path): raymarch into a
// quarter-res RT, temporally resolve against the previous frame's result
// (prevViewProj reprojection + neighborhood clamp), then upscale-composite
// over the scene with a colorRT/tempColorRT swap. Parameters live in
// cloudSettings (Metal-tested defaults). Off by default.
void Renderer::volumetricCloudPass() {
    if (!volumetricCloudsEnabled) return;
    if (!cloudRaymarchPipeline.isValid() || !cloudTemporalPipeline.isValid() ||
        !cloudCompositePipeline.isValid() || !cloudRT.isValid() ||
        !cloudHistoryRT.isValid() || !cloudResolvedRT.isValid() ||
        !colorRT.isValid() || !tempColorRT.isValid() || !depthStencilRT.isValid() ||
        !cloudDataBuffer.isValid()) {
        return;
    }

    glm::mat4 curViewProj = currentCamera.proj * currentCamera.view;
    if (!cloudPrevViewProjValid) { cloudPrevViewProj = curViewProj; cloudPrevViewProjValid = true; }

    // Per-frame data: camera/sun, accumulated wind, temporal state. screenSize
    // is the quarter-res cloud RT size (only the temporal neighborhood uses it).
    cloudSettings.invViewProj = glm::inverse(curViewProj);
    cloudSettings.prevViewProj = cloudPrevViewProj;
    cloudSettings.cameraPosition = currentCamera.position;
    cloudSettings.sunDirection = glm::normalize(atmosphereData.sunDirection);
    cloudSettings.sunColor = atmosphereData.sunColor;
    // windSpeed is the cloud's per-medium scroll coefficient; the shared wind
    // strength scales it so the WindFieldComponent drives the scroll rate.
    cloudSettings.windOffset += cloudSettings.windDirection * (cloudSettings.windSpeed * m_windStrength) * 0.016f;
    cloudSettings.time += 1.0f / 60.0f;
    cloudSettings.frameIndex = frameCounter;
    cloudSettings.screenSize = glm::vec2(std::max(1u, rhi->getSwapchainWidth() / 4),
                                         std::max(1u, rhi->getSwapchainHeight() / 4));
    cloudSettings.cloudLayerThickness = cloudSettings.cloudLayerTop - cloudSettings.cloudLayerBottom;
    rhi->updateBuffer(cloudDataBuffer, &cloudSettings, 0, sizeof(cloudSettings));

    // Pass 1: quarter-res raymarch -> cloudRT.
    {
        RenderPassDesc rp;
        rp.name = "CloudRaymarch";
        rp.colorAttachments.push_back(cloudRT);
        rp.clearColors.push_back(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        rp.loadColor.push_back(false);
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(cloudRaymarchPipeline);
        rhi->setTexture(0, 0, depthStencilRT, clampSampler);
        rhi->setFragmentBuffer(0, cloudDataBuffer, 0, sizeof(VolumetricCloudRenderData));
        // cloudFragmentLowRes reads the camera at buffer(1) on Metal; the
        // GLSL twin declares it at set-relative binding 3.
        if (backend == GraphicsBackend::Metal) {
            rhi->setFragmentBuffer(1, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        } else {
            rhi->setFragmentBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        }
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
    }

    // Pass 2: temporal resolve (current + history -> resolved), then the
    // resolved RT becomes next frame's history.
    {
        RenderPassDesc rp;
        rp.name = "CloudTemporal";
        rp.colorAttachments.push_back(cloudResolvedRT);
        rp.clearColors.push_back(glm::vec4(0.0f));
        rp.loadColor.push_back(false);
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(cloudTemporalPipeline);
        rhi->setTexture(0, 0, cloudRT, clampSampler);
        rhi->setTexture(0, 1, cloudHistoryRT, clampSampler);
        rhi->setTexture(0, 2, depthStencilRT, clampSampler);
        rhi->setTexture(0, 3, velocityRT, clampSampler);  // bound for future MV reprojection
        rhi->setFragmentBuffer(0, cloudDataBuffer, 0, sizeof(VolumetricCloudRenderData));
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
        std::swap(cloudHistoryRT, cloudResolvedRT);  // history <- resolved
    }

    // Pass 3: upscale + composite over the scene (ping-pong like fog).
    {
        RenderPassDesc rp;
        rp.name = "CloudComposite";
        rp.colorAttachments.push_back(tempColorRT);
        rp.loadColor.push_back(false);  // every pixel written
        rhi->beginRenderPass(rp);
        rhi->bindPipeline(cloudCompositePipeline);
        rhi->setTexture(0, 0, colorRT, clampSampler);
        rhi->setTexture(0, 1, cloudHistoryRT, clampSampler);  // resolved clouds
        rhi->setTexture(0, 2, depthStencilRT, clampSampler);
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
        std::swap(colorRT, tempColorRT);
    }

    cloudPrevViewProj = curViewProj;
}

// RmlUI bring-up on the RHI path — mirrors Renderer_Metal::initUI(), but with
// the cross-backend RmlRendererRHI so it works on Vulkan (and Metal via the
// RHI routing).
bool Renderer::initUI() {
    auto* engineCore = Vapor::EngineCore::Get();
    if (!engineCore) {
        fmt::print("Renderer::initUI: EngineCore not available\n");
        return false;
    }
    // Bootstrap the manager if nothing did yet (native Metal does this from
    // its ImGui section). RmlUi lays out in LOGICAL window coordinates — the
    // physical framebuffer scaling happens in the render interface (Retina).
    auto* rmluiManager = engineCore->getRmlUiManager();
    if (!rmluiManager && window) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        if (engineCore->initRmlUI(w, h)) {
            rmluiManager = engineCore->getRmlUiManager();
        }
    }
    if (!rmluiManager || !rmluiManager->IsInitialized()) {
        fmt::print("Renderer::initUI: RmlUiManager not initialized\n");
        return false;
    }

    auto uiRenderer = std::make_unique<Vapor::RmlRendererRHI>(rhi.get(), backend);
    if (!uiRenderer->initialize()) {
        fmt::print("Renderer::initUI: Failed to initialize RHI UI renderer\n");
        return false;
    }

    Rml::SetRenderInterface(uiRenderer.get());
    if (!rmluiManager->FinalizeInitialization()) {
        fmt::print("Renderer::initUI: Failed to finalize RmlUI\n");
        // uiRenderer is about to be destroyed — don't leave Rml holding it.
        Rml::SetRenderInterface(nullptr);
        return false;
    }
    m_uiRenderer = std::move(uiRenderer);
    m_uiContext = rmluiManager->GetContext();
    fmt::print("Renderer::initUI: RHI UI renderer initialized successfully\n");
    return true;
}

// Draw the RmlUI context over the swapchain at the end of the frame graph.
void Renderer::renderUI() {
    if (!m_uiRenderer || !m_uiContext) return;
    // Debug escape hatch: draw everything except the UI overlay.
    static const bool uiDisabled = std::getenv("VAPOR_DISABLE_RMLUI") != nullptr;
    if (uiDisabled) return;
    auto* uiRenderer = m_uiRenderer.get();

    // Logical UI size = the Rml context dimensions (set from the window size);
    // framebuffer size = the swapchain (physical pixels, HiDPI-aware).
    Rml::Vector2i dims = m_uiContext->GetDimensions();
    int fbWidth = static_cast<int>(rhi->getSwapchainWidth());
    int fbHeight = static_cast<int>(rhi->getSwapchainHeight());

    RenderPassDesc rp;
    rp.name = "RmlUi";
    rp.colorAttachments.push_back(TextureHandle{0});  // swapchain
    rp.loadColor.push_back(true);                     // draw on top
    rhi->beginRenderPass(rp);
    uiRenderer->beginFrame(dims.x, dims.y, fbWidth, fbHeight);
    m_uiContext->Render();
    uiRenderer->endFrame();
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

    if (backend == GraphicsBackend::Metal) {
        // 3d_post_process.metal: screen(0) — bloom was already composited
        // into colorRT by the BloomComposite step — AO(1), normal(2, for the
        // deband dither), god rays(3), params at buffer(0).
        TextureHandle whiteTex = textures[defaultWhiteTexture].handle;
        TextureHandle blackTex = textures[defaultBlackTexture].handle;
        rhi->setTexture(0, 0, colorRT, clampSampler);
        TextureHandle ao = (capabilities.raytracing && aoEnabled && aoRT.isValid()) ? aoRT : whiteTex;
        rhi->setTexture(0, 1, ao, clampSampler);
        rhi->setTexture(0, 2, normalRT.isValid() ? normalRT : blackTex, clampSampler);
        TextureHandle godRays = (lightScatteringEnabled && lightScatteringRT.isValid())
            ? lightScatteringRT : blackTex;
        rhi->setTexture(0, 3, godRays, clampSampler);
        rhi->setFragmentBytes(&postProcessParams, sizeof(postProcessParams), 0);
        rhi->draw(3, 1, 0, 0);
        rhi->endRenderPass();
        return;
    }

    // Fragment texture 0: HDR colorRT; texture 1: accumulated bloom (pyramid[0]).
    if (colorRT.isValid() && clampSampler.isValid()) {
        rhi->setTexture(0, 0, colorRT, clampSampler);
    }
    if (bloomPyramid[0].isValid() && clampSampler.isValid()) {
        rhi->setTexture(0, 1, bloomPyramid[0], clampSampler);
    }
    if (clampSampler.isValid()) {
        // When god rays are disabled their RT is never written — bind black
        // (adds nothing) instead of sampling uninitialized memory.
        TextureHandle godRays = (lightScatteringEnabled && lightScatteringRT.isValid())
            ? lightScatteringRT : textures[defaultBlackTexture].handle;
        rhi->setTexture(0, 2, godRays, clampSampler);
    }
    // Chromatic aberration / vignette / color grade params (set1 b0).
    if (postProcessParamsBuffer.isValid()) {
        rhi->updateBuffer(postProcessParamsBuffer, &postProcessParams, 0, sizeof(postProcessParams));
        rhi->setFragmentBuffer(0, postProcessParamsBuffer, 0, sizeof(PostProcessParams));
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

    // Create default ORM texture: occlusion=1, roughness=1, metallic=0. The
    // PBR shaders read occlusion from .r, roughness from .g, metallic from .b,
    // so a material with no ORM map shades as a fully-rough dielectric (not a
    // mirror-metal, which the old white default produced). Matches the native
    // default_orm.png byte-for-byte.
    {
        TextureDesc texDesc;
        texDesc.width = 1;
        texDesc.height = 1;
        texDesc.format = PixelFormat::RGBA8_UNORM;
        texDesc.usage = TextureUsage::Sampled;
        TextureHandle texHandle = rhi->createTexture(texDesc);

        Uint32 ormPixel = 0xFF00FFFF;  // RGBA8: R=255 G=255 B=0 A=255
        rhi->updateTexture(texHandle, &ormPixel, sizeof(Uint32));

        RenderTexture tex;
        tex.handle = texHandle;
        tex.sampler = defaultSampler;
        tex.width = 1;
        tex.height = 1;
        tex.format = PixelFormat::RGBA8_UNORM;

        defaultORMTexture = static_cast<TextureId>(textures.size());
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

        // Independent near-field shadow map: its own texture at its own (finer)
        // resolution, tight-fit to [near, pssmRTMaxDist]. Sampled at set2 b9 —
        // the texture set now has 10 bindings, so a distinct map costs nothing.
        TextureDesc nearDesc;
        nearDesc.width = NEAR_SHADOW_MAP_SIZE;
        nearDesc.height = NEAR_SHADOW_MAP_SIZE;
        nearDesc.arrayLayers = 1;
        nearDesc.format = PixelFormat::Depth32Float;
        nearDesc.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
        nearShadowMap = rhi->createTexture(nearDesc);
    }

    // GPU particle pool: ECS emitters claim slots via claimParticleSlots().
    // Buffer is zero-filled — lifetime=0/age=0 means instantly-dead in the shader,
    // so uninitialized slots are safely skipped by the compute passes.
    {
        BufferDesc pbDesc;
        pbDesc.size = sizeof(GPUParticleData) * MAX_PARTICLES;
        pbDesc.usage = BufferUsage::Storage;
        pbDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        particleBuffer = rhi->createBuffer(pbDesc);

        // Zero-fill via mapped pointer — avoids a 192 MB temporary heap allocation.
        // lifetime=0/age=0 → age >= lifetime → compute shaders skip these slots.
        void* ptr = rhi->mapBuffer(particleBuffer);
        std::memset(ptr, 0, pbDesc.size);
        rhi->unmapBuffer(particleBuffer);
        // particleCount starts at 0; updated by claimParticleSlots() high-water mark.

        BufferDesc uDesc;
        uDesc.usage = BufferUsage::Storage;
        uDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        uDesc.size = sizeof(ParticleSimParams);
        createFrameSlottedBuffer(particleSimParamsBuffer, uDesc);
        uDesc.size = sizeof(ParticleAttractor) * MAX_PARTICLE_ATTRACTORS;
        createFrameSlottedBuffer(particleAttractorBuffer, uDesc);

        // Indirect draw args for per-emitter particle draws: one 16-byte
        // {vertexCount, instanceCount, firstVertex, firstInstance} per packet.
        BufferDesc iaDesc;
        iaDesc.usage = BufferUsage::Indirect;
        iaDesc.memoryUsage = MemoryUsage::CPUtoGPU;
        iaDesc.size = 16u * MAX_PARTICLE_DRAWS;
        createFrameSlottedBuffer(particleDrawArgsBuffer, iaDesc);
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

void Renderer::destroyRenderTargets() {
    auto kill = [&](TextureHandle& t) {
        if (t.isValid()) { rhi->destroyTexture(t); t = {}; }
    };
    kill(depthStencilRT_MSAA); kill(depthStencilRT);
    kill(colorRT_MSAA); kill(colorRT); kill(tempColorRT);
    kill(normalRT_MSAA); kill(normalRT); kill(albedoRT);
    kill(shadowRT); kill(aoRT); kill(reflectionRT); kill(refractionRT);
    kill(aoRawRT); kill(aoScratchRT); kill(aoHistoryRT[0]); kill(aoHistoryRT[1]);
    kill(stochasticShadowRT); kill(stochasticShadowHistoryRT); kill(stochasticShadowDenoisedRT);
    kill(stochasticShadowHalfRT);
    // ReSTIR shadow reservoirs are half-swapchain-sized (restirShadowPass
    // reallocates at the new size on its next run).
    if (restirReservoirHistory.isValid()) { rhi->destroyBuffer(restirReservoirHistory); restirReservoirHistory = {}; }
    if (restirReservoirScratch.isValid()) { rhi->destroyBuffer(restirReservoirScratch); restirReservoirScratch = {}; }
    restirHistoryValid = false;
    // The recreated shadow targets hold undefined memory until the chain runs.
    stochasticShadowWritten = false;
    stochasticShadowHistoryWritten = false;
    stochasticShadowDenoiseRan = false;
    kill(giResultTexture);
    kill(giRawRT); kill(giScratchRT); kill(giHistoryChainRT[0]); kill(giHistoryChainRT[1]);
    kill(bloomBrightness);
    for (Uint32 i = 0; i < BLOOM_PYRAMID_LEVELS; i++) kill(bloomPyramid[i]);
    kill(lightScatteringRT);
    kill(velocityRT);
    kill(cloudRT); kill(cloudHistoryRT); kill(cloudResolvedRT);
    kill(swapchainDepthBuffer);

    // History/reprojection state is stale at the new resolution.
    aoHistoryValid = false;
    aoHistoryIndex = 0;
    prevViewValid = false;
    prevViewProjValid = false;
    cloudPrevViewProjValid = false;
}

void Renderer::createRenderTargets() {
    Uint32 width = rhi->getSwapchainWidth();
    Uint32 height = rhi->getSwapchainHeight();
    // The RT ambient-occlusion chain and the ray-traced directional shadow are
    // rendered at HALF resolution and bilinearly upsampled when the PBR shader
    // samples them by screen UV — exactly like the native renderer. Running
    // them full-res (as this path used to) made AODenoise's two 5x5 a-trous
    // passes and the RT shadow trace ~4x more expensive than native for no
    // visible gain (AO/soft shadow are low frequency). Point shadows and the
    // colour/normal/depth targets stay full-res, matching native.
    Uint32 halfW = (width + 1) / 2;
    Uint32 halfH = (height + 1) / 2;
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

    // Hi-Z depth pyramid (half-res mip 0, full max-depth mip chain) + a scratch
    // twin used to stage each level (see hizBuildPass: a single sampleable
    // texture can't be read and written in the same pass, so a mip is copied to
    // the scratch and reduced back). R32F color so a compute shader can textureLod
    // it. Only the GPU-occlusion path uses these, but they're cheap to always have.
    {
        if (hizTexture.isValid()) rhi->destroyTexture(hizTexture);
        if (hizScratchTexture.isValid()) rhi->destroyTexture(hizScratchTexture);
        hizWidth = std::max(1u, (width + 1) / 2);
        hizHeight = std::max(1u, (height + 1) / 2);
        hizMipCount = 1;
        for (Uint32 d = std::max(hizWidth, hizHeight); d > 1; d >>= 1) hizMipCount++;

        TextureDesc desc;
        desc.width = hizWidth;
        desc.height = hizHeight;
        desc.mipLevels = hizMipCount;
        desc.format = PixelFormat::R32_FLOAT;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        hizTexture = rhi->createTexture(desc);
        hizScratchTexture = rhi->createTexture(desc);
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
        // Ping-pong twin for the fog pass (read colorRT -> write tempColorRT ->
        // swap, so downstream passes transparently see the fogged result).
        tempColorRT = rhi->createTexture(desc);
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
        // PrePass renders into it (RenderTarget), RT kernels read it (Sampled),
        // and the legacy resolve path may write it via compute (Storage).
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Storage | TextureUsage::Sampled;
        normalRT = rhi->createTexture(desc);
    }

    // Create shadow RT (half-res RT directional shadow, like native)
    {
        TextureDesc desc;
        desc.width = halfW;
        desc.height = halfH;
        desc.format = PixelFormat::RGBA8_UNORM;
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled;
        desc.mipLevels = static_cast<Uint32>(std::floor(std::log2(std::max(halfW, halfH))) + 1);
        shadowRT = rhi->createTexture(desc);
    }

    // RT mirror reflections (half-res; rgb = reflected radiance, a = hit mask).
    // HDR: it carries surfel radiance / env light, composited pre-tonemap.
    {
        TextureDesc desc;
        desc.width = halfW;
        desc.height = halfH;
        desc.format = PixelFormat::RGBA16_FLOAT;
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled;
        reflectionRT = rhi->createTexture(desc);
        // RT refraction twin (same half-res RGBA16F, rgb + hit mask in a).
        refractionRT = rhi->createTexture(desc);
    }

    // Create AO RT (half-res, like native)
    {
        TextureDesc desc;
        desc.width = halfW;
        desc.height = halfH;
        desc.format = PixelFormat::R16_FLOAT;  // Single channel float
        // RenderTarget usage for the Vulkan denoise fragment pass's final write.
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled | TextureUsage::RenderTarget;
        aoRT = rhi->createTexture(desc);
    }

    // Screen-space contact shadow RT (half-res, single channel visibility).
    {
        TextureDesc desc;
        desc.width = halfW;
        desc.height = halfH;
        desc.format = PixelFormat::R8_UNORM;
        // RenderTarget for the Vulkan fullscreen frag path; Storage for the Metal
        // compute path (3d_sscs.metal writes it); Sampled for the main pass read.
        desc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget | TextureUsage::Storage;
        sscsRT = rhi->createTexture(desc);
    }

    // RT AO / point-shadow working set + prepass albedo (RT-capable backends;
    // cheap enough to create unconditionally, only written when RT passes run).
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.sampleCount = 1;

        desc.format = PixelFormat::RGBA8_UNORM;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        albedoRT = rhi->createTexture(desc);  // full-res (prepass MRT)

        // AO working chain: half-res, matching native's aoChainDesc.
        // RenderTarget usage added for the Vulkan fragment-pass twins.
        desc.width = halfW;
        desc.height = halfH;
        desc.format = PixelFormat::R16_FLOAT;
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled | TextureUsage::RenderTarget;
        aoRawRT = rhi->createTexture(desc);
        // History + scratch are RGBA16F like native (3d_ao_temporal.metal packs
        // ao + view-space depth + octahedral normal); the previous R16F here
        // silently dropped the depth/normal channels, so the à-trous edge
        // stops read zeros and the denoise degenerated into a plain blur.
        desc.format = PixelFormat::RGBA16_FLOAT;
        aoScratchRT = rhi->createTexture(desc);
        aoHistoryRT[0] = rhi->createTexture(desc);
        aoHistoryRT[1] = rhi->createTexture(desc);

        // Point shadows stay full-res (native creates these at drawableSize).
        // RGBA16F: the stochastic kernel now packs THREE visibility channels —
        // R = point lights, G = rect area lights, B = spot lights (native keeps
        // its R16F targets; the extra channels just drop there, and its PBR
        // binds shadowFlags=0 so it never reads them).
        desc.format = PixelFormat::RGBA16_FLOAT;
        desc.usage = TextureUsage::Storage | TextureUsage::Sampled;
        desc.width = width;
        desc.height = height;
        stochasticShadowRT = rhi->createTexture(desc);
        stochasticShadowHistoryRT = rhi->createTexture(desc);
        stochasticShadowDenoisedRT = rhi->createTexture(desc);

        // Half-res raw target for the ReSTIR resolve (upsampled into
        // stochasticShadowRT before the accumulator).
        desc.width = (width + 1) / 2;
        desc.height = (height + 1) / 2;
        stochasticShadowHalfRT = rhi->createTexture(desc);
        desc.width = width;
        desc.height = height;

        // GIBS GI result at resolutionScale (compute-written, PBR-sampled).
        if (capabilities.raytracing) {
            TextureDesc gi;
            // native clamps the GI texture to a 64px floor (GIBSManager::createTextures)
            gi.width = std::max(64u, Uint32(width * gibsResolutionScale));
            gi.height = std::max(64u, Uint32(height * gibsResolutionScale));
            gi.format = PixelFormat::RGBA16_FLOAT;
            gi.usage = TextureUsage::Storage | TextureUsage::Sampled;
            gi.sampleCount = 1;
            giResultTexture = rhi->createTexture(gi);
            // SVGF-lite chain (same size/format): gather -> giRawRT ->
            // temporal (history ping-pong) -> a-trous x2 -> giResultTexture.
            giRawRT = rhi->createTexture(gi);
            giScratchRT = rhi->createTexture(gi);
            giHistoryChainRT[0] = rhi->createTexture(gi);
            giHistoryChainRT[1] = rhi->createTexture(gi);
            giHistoryValid = false;  // resolution changed: old history is garbage
        }
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

    // God-ray (light scattering) target, half resolution.
    {
        TextureDesc desc;
        desc.width = std::max(1u, width / 2);
        desc.height = std::max(1u, height / 2);
        desc.format = PixelFormat::RGBA16_FLOAT;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        desc.sampleCount = 1;
        lightScatteringRT = rhi->createTexture(desc);
    }

    // Velocity (motion vectors), full resolution, signed HDR for +/- vectors.
    {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = PixelFormat::RGBA16_FLOAT;
        // RenderTarget for the Vulkan fragment path, Storage for the Metal
        // compute kernel that writes it (MTLTextureUsageShaderWrite).
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled | TextureUsage::Storage;
        desc.sampleCount = 1;
        velocityRT = rhi->createTexture(desc);
    }

    // Volumetric cloud targets (quarter resolution, matching Metal): current
    // raymarch, previous resolved frame (history), and the temporal output.
    // Three RTs instead of Metal's two because Vulkan cannot sample the
    // attachment being rendered (Metal read+wrote history in one pass).
    {
        TextureDesc desc;
        desc.width = std::max(1u, width / 4);
        desc.height = std::max(1u, height / 4);
        desc.format = PixelFormat::RGBA16_FLOAT;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
        desc.sampleCount = 1;
        cloudRT = rhi->createTexture(desc);
        cloudHistoryRT = rhi->createTexture(desc);
        cloudResolvedRT = rhi->createTexture(desc);
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
    // No depth WRITE: the pre-pass already owns the depth buffer (it clears and
    // writes all geometry), and the main pass redraws the SAME geometry. Writing
    // here is not just redundant — combined with early_fragment_tests it is
    // wrong for alpha cutout: early-Z would stamp a MASK caster's depth into a
    // hole texel BEFORE the shader discards it, blocking the sky/background from
    // filling the hole (it showed clear colour). Test-only keeps early-Z's
    // overdraw win without the corruption.
    pipelineDesc.depthWrite = false;
    // LessOrEqual (not Less): the main pass loads the pre-pass depth and redraws
    // the SAME geometry, so fragments arrive at exactly the stored depth and
    // must pass the test (Less would reject every fragment). Matches native's
    // CompareFunctionLessEqual.
    pipelineDesc.depthCompareOp = CompareOp::LessOrEqual;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.frontFaceCounterClockwise = true;
    pipelineDesc.sampleCount = 1;
    // Main geometry renders into the HDR colorRT (RGBA16F); the PostProcess pass
    // tone-maps it to the swapchain. Bake the matching color format into the PSO.
    pipelineDesc.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
    pipelineDesc.hasDepthAttachment = true;
    pipelineDesc.depthAttachmentFormat = PixelFormat::Depth32Float;

    mainPipeline = rhi->createPipeline(pipelineDesc);

    // Bindless MDI twin: same pipeline with the bindless fragment variant.
    //   Metal:  fragmentMain specialized with kBindlessMaterials (argument
    //           table at buffer 13) + the supportIndirectCommandBuffers opt-in
    //           that executeICB requires.
    //   Vulkan: RHIMainBindless.frag.spv (compiled with -DBINDLESS; material
    //           textures from the set-3 runtime descriptor array).
    if (backend == GraphicsBackend::Metal && capabilities.bindlessTextures) {
        ShaderDesc bfd;
        bfd.stage = ShaderStage::Fragment;
        bfd.code = fragShaderCode.data();
        bfd.codeSize = fragShaderCode.size();
        bfd.entryPoint = "fragmentMain";
        bfd.bindlessMaterials = true;
        fragmentShaderBindless = rhi->createShader(bfd);
        PipelineDesc bDesc = pipelineDesc;
        bDesc.fragmentShader = fragmentShaderBindless;
        bDesc.supportsICB = true;
        mainPipelineBindless = rhi->createPipeline(bDesc);
    } else if (backend == GraphicsBackend::Vulkan && capabilities.bindlessTextures) {
        std::string bindlessFragCode = readFile("shaders/RHIMainBindless.frag.spv");
        if (!bindlessFragCode.empty()) {
            ShaderDesc bfd;
            bfd.stage = ShaderStage::Fragment;
            bfd.code = bindlessFragCode.data();
            bfd.codeSize = bindlessFragCode.size();
            bfd.entryPoint = "main";
            fragmentShaderBindless = rhi->createShader(bfd);
            PipelineDesc bDesc = pipelineDesc;
            bDesc.fragmentShader = fragmentShaderBindless;
            mainPipelineBindless = rhi->createPipeline(bDesc);
        }
    }

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
                                                  BlendMode blend,
                                                  PixelFormat colorFormat = PixelFormat::RGBA16_FLOAT) -> PipelineHandle {
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
                d.colorAttachmentFormats = { colorFormat };
                return rhi->createPipeline(d);
            };
            bloomBrightPipeline      = makeFullscreenFragPipeline("shaders/BloomBright.frag.spv",     bloomBrightShader,     BlendMode::Opaque);
            bloomDownsamplePipeline  = makeFullscreenFragPipeline("shaders/BloomDownsample.frag.spv", bloomDownsampleShader, BlendMode::Opaque);
            bloomUpsamplePipeline    = makeFullscreenFragPipeline("shaders/BloomUpsample.frag.spv",   bloomUpsampleShader,   BlendMode::Additive);
            // Hi-Z reduce: fullscreen max-depth downsample into an R32F mip.
            hizReducePipeline        = makeFullscreenFragPipeline("shaders/HiZReduce.frag.spv",       hizReduceFS,           BlendMode::Opaque, PixelFormat::R32_FLOAT);

            // IBL bake pipelines (GLSL twins of the Metal chain). iblCapturePass
            // is backend-agnostic (RHI calls + render-to-array-layer), so once
            // these exist it runs on Vulkan and fills the same cubemaps.
            auto makeVertFragPipeline = [&](const char* vertSpv, const char* fragSpv,
                                            ShaderHandle& outVS, ShaderHandle& outFS) -> PipelineHandle {
                std::string vc = readFile(vertSpv), fc = readFile(fragSpv);
                if (vc.empty() || fc.empty()) return {};
                ShaderDesc vd; vd.stage = ShaderStage::Vertex;
                vd.code = vc.data(); vd.codeSize = vc.size(); vd.entryPoint = "main";
                outVS = rhi->createShader(vd);
                ShaderDesc fd; fd.stage = ShaderStage::Fragment;
                fd.code = fc.data(); fd.codeSize = fc.size(); fd.entryPoint = "main";
                outFS = rhi->createShader(fd);
                PipelineDesc d;
                d.vertexShader = outVS;
                d.fragmentShader = outFS;
                d.vertexLayout.stride = 0;
                d.vertexLayout.attributes = {};
                d.topology = PrimitiveTopology::TriangleList;
                d.blendMode = BlendMode::Opaque;
                d.depthTest = false;
                d.depthWrite = false;
                d.cullMode = CullMode::None;
                d.sampleCount = 1;
                d.hasDepthAttachment = false;
                d.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
                return rhi->createPipeline(d);
            };
            // skyCapture is the atmosphere->cubemap IBL source. It shares the
            // unified IBLCubeFace.vert with the equirect/irradiance/prefilter
            // passes built below — irradiance/prefilter/brdfLUT are created there,
            // NOT here. This block previously rebuilt them with the older
            // Irradiance/Prefilter/Brdf shaders and had them immediately
            // overwritten below, leaking three pipelines every startup.
            skyCapturePipeline = makeVertFragPipeline("shaders/IBLCubeFace.vert.spv", "shaders/SkyCapture.frag.spv", skyCaptureVS, skyCaptureFS);

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

                // Skybox: same fullscreen depth-tested setup, but samples the
                // environment cubemap (SkyType::HDRI). Reuses Sky.vert.
                std::string skyboxFragCode = readFile("shaders/Skybox.frag.spv");
                if (!skyboxFragCode.empty()) {
                    ShaderDesc sfd; sfd.stage = ShaderStage::Fragment; sfd.code = skyboxFragCode.data(); sfd.codeSize = skyboxFragCode.size(); sfd.entryPoint = "main";
                    skyboxFragmentShader = rhi->createShader(sfd);
                    PipelineDesc sd = ad;               // same fullscreen/depth state as atmosphere
                    sd.fragmentShader = skyboxFragmentShader;
                    skyboxPipeline = rhi->createPipeline(sd);
                }

                // Gradient: same fullscreen depth-tested setup, blends a
                // zenith/horizon/ground gradient (SkyType::Gradient). Reuses Sky.vert.
                std::string gradientFragCode = readFile("shaders/Gradient.frag.spv");
                if (!gradientFragCode.empty()) {
                    ShaderDesc gfd; gfd.stage = ShaderStage::Fragment; gfd.code = gradientFragCode.data(); gfd.codeSize = gradientFragCode.size(); gfd.entryPoint = "main";
                    gradientFragmentShader = rhi->createShader(gfd);
                    PipelineDesc gd = ad;               // same fullscreen/depth state as atmosphere
                    gd.fragmentShader = gradientFragmentShader;
                    gradientPipeline = rhi->createPipeline(gd);
                }
            }

            // God rays: fullscreen light-scattering march into the half-res RT.
            lightScatteringPipeline = makeFullscreenFragPipeline(
                "shaders/LightScattering.frag.spv", lightScatteringShader, BlendMode::Opaque);

            // SSAO chain twins (fragment passes — compute can't sample depth):
            // SSAO -> aoRawRT (R16F), temporal -> history (RGBA16F), à-trous
            // denoise runs twice with different target formats (scratch RGBA16F,
            // final aoRT R16F), hence two pipelines over one shader.
            vkSsaoPipeline = makeFullscreenFragPipeline(
                "shaders/SSAO.frag.spv", vkSsaoShader, BlendMode::Opaque, PixelFormat::R16_FLOAT);
            vkSscsPipeline = makeFullscreenFragPipeline(
                "shaders/SSCS.frag.spv", vkSscsShader, BlendMode::Opaque, PixelFormat::R8_UNORM);
            vkAoTemporalPipeline = makeFullscreenFragPipeline(
                "shaders/AOTemporal.frag.spv", vkAoTemporalShader, BlendMode::Opaque, PixelFormat::RGBA16_FLOAT);
            vkAoDenoisePipelineRGBA = makeFullscreenFragPipeline(
                "shaders/AODenoise.frag.spv", vkAoDenoiseShader, BlendMode::Opaque, PixelFormat::RGBA16_FLOAT);
            {
                ShaderHandle dummy;  // shader already created above; reuse the file
                vkAoDenoisePipelineR16 = makeFullscreenFragPipeline(
                    "shaders/AODenoise.frag.spv", dummy, BlendMode::Opaque, PixelFormat::R16_FLOAT);
            }

            // Cheap analytic height fog: fullscreen color+depth -> tempColorRT.
            heightFogPipeline = makeFullscreenFragPipeline(
                "shaders/HeightFog.frag.spv", heightFogShader, BlendMode::Opaque);

            // Expensive per-light volumetric fog: fullscreen color+depth -> tempColorRT.
            volumetricFogPipeline = makeFullscreenFragPipeline(
                "shaders/VolumetricFog.frag.spv", volumetricFogShader, BlendMode::Opaque);

            // Froxel volumetric fog (Vulkan): inject + integrate compute stages
            // write the 3D grid; the composite samples it. volumetricFogPass prefers
            // this over the raymarch above when the pipelines/textures exist. The
            // .comp local_size (4^3 / 8x8) matches the pass's dispatch group counts.
            {
                auto makeFroxelCompute = [&](const char* spv, ShaderHandle& sh) -> ComputePipelineHandle {
                    std::string code = readFile(spv);
                    if (code.empty()) return {};
                    ShaderDesc d; d.stage = ShaderStage::Compute; d.code = code.data();
                    d.codeSize = code.size(); d.entryPoint = "main";
                    sh = rhi->createShader(d);
                    ComputePipelineDesc cd; cd.computeShader = sh;
                    return rhi->createComputePipeline(cd);
                };
                fogFroxelInjectPipeline = makeFroxelCompute("shaders/FroxelInject.comp.spv", fogFroxelInjectShader);
                fogFroxelIntegratePipeline = makeFroxelCompute("shaders/FroxelIntegrate.comp.spv", fogFroxelIntegrateShader);
                fogFroxelCompositePipeline = makeFullscreenFragPipeline(
                    "shaders/FroxelFogComposite.frag.spv", fogFroxelCompositeShader, BlendMode::Opaque);
            }

            // Heterogeneous volume raymarch (EmberGen density grids).
            volumeRaymarchPipeline = makeFullscreenFragPipeline(
                "shaders/VolumeRaymarch.frag.spv", volumeRaymarchShader, BlendMode::Opaque);

            // Camera-motion velocity (motion vectors) from depth.
            velocityPipeline = makeFullscreenFragPipeline(
                "shaders/Velocity.frag.spv", velocityShader, BlendMode::Opaque);

            // Tile light culling (fills the cluster buffer RHIMain.frag reads).
            std::string tlcCode = readFile("shaders/TileLightCull.comp.spv");
            if (!tlcCode.empty()) {
                ShaderDesc td; td.stage = ShaderStage::Compute; td.code = tlcCode.data();
                td.codeSize = tlcCode.size(); td.entryPoint = "main";
                vkTileCullShader = rhi->createShader(td);
                ComputePipelineDesc tcd; tcd.computeShader = vkTileCullShader;
                vkTileCullPipeline = rhi->createComputePipeline(tcd);
            }

            // IBL capture pipelines (SPIR-V). The RHI IBL chain was Metal-only;
            // these bring the HDRI environment path to Vulkan. Each is a
            // fullscreen-triangle pass rendering into a cube face / 2D LUT
            // (RGBA16F, no depth). Sky capture is not ported, so the Vulkan IBL
            // source is HDRI only.
            auto makeIblVkPipeline = [&](const char* vertSpv, const char* fragSpv,
                                         ShaderHandle& vsOut, ShaderHandle& fsOut) -> PipelineHandle {
                std::string vcode = readFile(vertSpv);
                std::string fcode = readFile(fragSpv);
                if (vcode.empty() || fcode.empty()) return {};
                ShaderDesc vd; vd.stage = ShaderStage::Vertex;   vd.code = vcode.data(); vd.codeSize = vcode.size(); vd.entryPoint = "main";
                ShaderDesc fd; fd.stage = ShaderStage::Fragment; fd.code = fcode.data(); fd.codeSize = fcode.size(); fd.entryPoint = "main";
                vsOut = rhi->createShader(vd);
                fsOut = rhi->createShader(fd);
                PipelineDesc d;
                d.vertexShader = vsOut;
                d.fragmentShader = fsOut;
                d.vertexLayout.stride = 0;
                d.vertexLayout.attributes = {};
                d.topology = PrimitiveTopology::TriangleList;
                d.blendMode = BlendMode::Opaque;
                d.depthTest = false;
                d.depthWrite = false;
                d.cullMode = CullMode::None;
                d.sampleCount = 1;
                d.hasDepthAttachment = false;
                d.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
                return rhi->createPipeline(d);
            };
            equirectToCubemapPipeline = makeIblVkPipeline("shaders/IBLCubeFace.vert.spv", "shaders/IBLEquirect.frag.spv",   equirectToCubemapVS, equirectToCubemapFS);
            irradiancePipeline        = makeIblVkPipeline("shaders/IBLCubeFace.vert.spv", "shaders/IBLIrradiance.frag.spv", irradianceVS, irradianceFS);
            prefilterPipeline         = makeIblVkPipeline("shaders/IBLCubeFace.vert.spv", "shaders/IBLPrefilter.frag.spv",  prefilterVS, prefilterFS);
            brdfLUTPipeline           = makeIblVkPipeline("shaders/IBLBRDF.vert.spv",     "shaders/IBLBRDF.frag.spv",        brdfVS, brdfFS);
            // IBL debug: cubemap -> equirect 2D RT (FullScreen.vert + IBLEquirectPreview.frag).
            iblPreviewPipeline        = makeIblVkPipeline("shaders/FullScreen.vert.spv", "shaders/IBLEquirectPreview.frag.spv", iblPreviewVertexShader, iblPreviewFragmentShader);

            // Volumetric clouds: quarter-res raymarch, temporal resolve, and
            // full-res composite — all fullscreen RGBA16F passes.
            cloudRaymarchPipeline = makeFullscreenFragPipeline(
                "shaders/CloudRaymarch.frag.spv", cloudRaymarchShader, BlendMode::Opaque);
            cloudTemporalPipeline = makeFullscreenFragPipeline(
                "shaders/CloudTemporal.frag.spv", cloudTemporalShader, BlendMode::Opaque);
            cloudCompositePipeline = makeFullscreenFragPipeline(
                "shaders/CloudComposite.frag.spv", cloudCompositeShader, BlendMode::Opaque);

            // GPU particles: two compute stages + an instanced-billboard render.
            auto makeCompute = [&](const char* spv, ShaderHandle& sh) -> ComputePipelineHandle {
                std::string code = readFile(spv);
                if (code.empty()) return {};
                ShaderDesc d; d.stage = ShaderStage::Compute; d.code = code.data();
                d.codeSize = code.size(); d.entryPoint = "main";
                sh = rhi->createShader(d);
                ComputePipelineDesc cd; cd.computeShader = sh;
                cd.threadGroupSizeX = 256;  // matches local_size_x in the .comp
                return rhi->createComputePipeline(cd);
            };
            // GPU-driven rendering: frustum-cull compute pass.
            gpuCullPipeline = makeCompute("shaders/GpuCull.comp.spv", gpuCullShader);

            particleForcePipeline     = makeCompute("shaders/ParticleForce.comp.spv", particleForceShader);
            particleIntegratePipeline = makeCompute("shaders/ParticleIntegrate.comp.spv", particleIntegrateShader);

            std::string pvCode = readFile("shaders/Particle.vert.spv");
            std::string pfCode = readFile("shaders/Particle.frag.spv");
            if (!pvCode.empty() && !pfCode.empty()) {
                ShaderDesc pvd; pvd.stage = ShaderStage::Vertex;   pvd.code = pvCode.data(); pvd.codeSize = pvCode.size(); pvd.entryPoint = "main";
                particleVertexShader = rhi->createShader(pvd);
                ShaderDesc pfd; pfd.stage = ShaderStage::Fragment; pfd.code = pfCode.data(); pfd.codeSize = pfCode.size(); pfd.entryPoint = "main";
                particleFragmentShader = rhi->createShader(pfd);

                PipelineDesc pd;
                pd.vertexShader = particleVertexShader;
                pd.fragmentShader = particleFragmentShader;
                pd.vertexLayout.stride = 0;   // billboard verts generated in-shader
                pd.vertexLayout.attributes = {};
                pd.topology = PrimitiveTopology::TriangleList;
                pd.depthTest = true;
                pd.depthWrite = false;               // translucent, don't occlude
                pd.depthCompareOp = CompareOp::LessOrEqual;
                pd.cullMode = CullMode::None;
                pd.sampleCount = 1;
                pd.hasDepthAttachment = true;
                pd.depthAttachmentFormat = PixelFormat::Depth32Float;
                pd.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
                // One pipeline per ParticleBlendMode — indexed by packet.blendMode.
                const BlendMode particleBlends[PARTICLE_BLEND_COUNT] = {
                    BlendMode::Additive, BlendMode::AlphaBlend, BlendMode::Multiply
                };
                for (Uint32 i = 0; i < PARTICLE_BLEND_COUNT; ++i) {
                    pd.blendMode = particleBlends[i];
                    particleRenderPipelines[i] = rhi->createPipeline(pd);
                }
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

    // ------------------------------------------------------------------------
    // PrePass pipeline (both backends): depth + normal + albedo MRT.
    // Vulkan: RHIMain.vert + PrePass.frag; Metal: 3d_depth_only.metal (same
    // binding convention as the main pass: camera 0 / materials 1 /
    // instances 2 / vertices 3 / instanceID bytes 4).
    // ------------------------------------------------------------------------
    {
        std::string ppv, ppf;
        const char* entryV = "main";
        const char* entryF = "main";
        if (backend == GraphicsBackend::Vulkan) {
            ppv = readFile("shaders/RHIMain.vert.spv");
            ppf = readFile("shaders/PrePass.frag.spv");
        } else if (backend == GraphicsBackend::Metal) {
            ppv = readFile("shaders/3d_depth_only.metal");
            ppf = ppv;
            entryV = "vertexMain";
            entryF = "fragmentMain";
        }
        if (!ppv.empty() && !ppf.empty()) {
            ShaderDesc vd; vd.stage = ShaderStage::Vertex;   vd.code = ppv.data(); vd.codeSize = ppv.size(); vd.entryPoint = entryV;
            prePassVertexShader = rhi->createShader(vd);
            ShaderDesc fd; fd.stage = ShaderStage::Fragment; fd.code = ppf.data(); fd.codeSize = ppf.size(); fd.entryPoint = entryF;
            prePassFragmentShader = rhi->createShader(fd);

            PipelineDesc pd;
            pd.vertexShader = prePassVertexShader;
            pd.fragmentShader = prePassFragmentShader;
            pd.vertexLayout.stride = sizeof(Vapor::VertexData);
            pd.vertexLayout.attributes = {
                {0, PixelFormat::RGB32_FLOAT, offsetof(Vapor::VertexData, position)},
                {1, PixelFormat::RG32_FLOAT,  offsetof(Vapor::VertexData, uv)},
                {2, PixelFormat::RGB32_FLOAT, offsetof(Vapor::VertexData, normal)},
                {3, PixelFormat::RGBA32_FLOAT, offsetof(Vapor::VertexData, tangent)},
            };
            pd.topology = PrimitiveTopology::TriangleList;
            pd.blendMode = BlendMode::Opaque;
            pd.depthTest = true;
            pd.depthWrite = true;
            pd.depthCompareOp = CompareOp::Less;
            pd.cullMode = CullMode::Back;
            pd.frontFaceCounterClockwise = true;
            pd.sampleCount = 1;
            pd.hasDepthAttachment = true;
            pd.depthAttachmentFormat = PixelFormat::Depth32Float;
            pd.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT, PixelFormat::RGBA8_UNORM };
            prePassPipeline = rhi->createPipeline(pd);
        }
    }

    // ------------------------------------------------------------------------
    // RT compute pipelines (Metal MSL, RequiresRaytracing-gated at the graph):
    // shadow / AO / AO temporal / AO denoise / stochastic point shadow +
    // temporal. Vulkan skips these passes, so no SPIR-V is needed.
    // ------------------------------------------------------------------------
    if (backend == GraphicsBackend::Metal && capabilities.raytracing) {
        auto makeMetalCompute = [&](const char* path, ShaderHandle& sh,
                                    Uint32 tgX, Uint32 tgY, Uint32 tgZ) -> ComputePipelineHandle {
            std::string code = readFile(path);
            if (code.empty()) return {};
            ShaderDesc d; d.stage = ShaderStage::Compute; d.code = code.data();
            d.codeSize = code.size(); d.entryPoint = "computeMain";
            sh = rhi->createShader(d);
            ComputePipelineDesc cd; cd.computeShader = sh;
            cd.threadGroupSizeX = tgX; cd.threadGroupSizeY = tgY; cd.threadGroupSizeZ = tgZ;
            return rhi->createComputePipeline(cd);
        };
        // Threadgroup shapes mirror the native dispatches exactly.
        raytraceShadowPipeline        = makeMetalCompute("shaders/3d_raytrace_shadow.metal", rtShadowShader, 1, 1, 1);
        sscsComputePipeline           = makeMetalCompute("shaders/3d_sscs.metal", sscsMetalShader, 1, 1, 1);
        raytraceAOPipeline            = makeMetalCompute("shaders/3d_raytrace_ao.metal", rtAOShader, 1, 1, 1);
        raytraceReflectionPipeline    = makeMetalCompute("shaders/3d_raytrace_reflection.metal", rtReflectionShader, 8, 8, 1);
        raytraceRefractionPipeline    = makeMetalCompute("shaders/3d_raytrace_refraction.metal", rtRefractionShader, 8, 8, 1);
        giTemporalPipeline            = makeMetalCompute("shaders/gibs_gi_temporal.metal", giTemporalShader, 8, 8, 1);
        giDenoisePipeline             = makeMetalCompute("shaders/gibs_gi_denoise.metal", giDenoiseShader, 8, 8, 1);
        // SSAO shares the RT AO binding interface (ignores the TLAS slot) and
        // is selected by aoMethod, exactly like native RaytraceAOPass.
        ssaoPipeline                  = makeMetalCompute("shaders/3d_ssao.metal", ssaoShader, 1, 1, 1);
        aoTemporalPipeline            = makeMetalCompute("shaders/3d_ao_temporal.metal", aoTemporalShader, 8, 8, 1);
        aoDenoisePipeline             = makeMetalCompute("shaders/3d_ao_denoise.metal", aoDenoiseShader, 8, 8, 1);
        stochasticShadowPipeline = makeMetalCompute("shaders/3d_stochastic_shadow.metal", stochasticShadowShader, 8, 8, 1);
        stochasticShadowTemporalPipeline   = makeMetalCompute("shaders/3d_stochastic_shadow_temporal.metal", stochasticShadowTemporalShader, 8, 8, 1);
        stochasticShadowDenoisePipeline    = makeMetalCompute("shaders/3d_stochastic_shadow_denoise.metal", stochasticShadowDenoiseShader, 8, 8, 1);
        // ReSTIR is optional with a live legacy fallback — a compile failure
        // here must not take down renderer init like the required kernels do.
        try {
            restirShadowTemporalPipeline = makeMetalCompute("shaders/3d_restir_shadow_temporal.metal", restirShadowTemporalShader, 8, 8, 1);
            restirShadowResolvePipeline  = makeMetalCompute("shaders/3d_restir_shadow_resolve.metal", restirShadowResolveShader, 8, 8, 1);
            stochasticShadowUpsamplePipeline  = makeMetalCompute("shaders/3d_stochastic_shadow_upsample.metal", stochasticShadowUpsampleShader, 8, 8, 1);
        } catch (const std::exception& e) {
            restirShadowTemporalPipeline = {};
            restirShadowResolvePipeline = {};
            stochasticShadowUpsamplePipeline = {};
            fmt::print(stderr, "ReSTIR shadow pipelines unavailable ({}), legacy stochastic kernel stays active\n", e.what());
        }

        // GIBS surfel GI kernels (entry points differ per file).
        auto makeNamedCompute = [&](const char* path, const char* entry, ShaderHandle& sh,
                                    Uint32 tgX, Uint32 tgY, Uint32 tgZ) -> ComputePipelineHandle {
            std::string code = readFile(path);
            if (code.empty()) return {};
            ShaderDesc d; d.stage = ShaderStage::Compute; d.code = code.data();
            d.codeSize = code.size(); d.entryPoint = entry;
            sh = rhi->createShader(d);
            ComputePipelineDesc cd; cd.computeShader = sh;
            cd.threadGroupSizeX = tgX; cd.threadGroupSizeY = tgY; cd.threadGroupSizeZ = tgZ;
            return rhi->createComputePipeline(cd);
        };
        surfelGenPipeline      = makeNamedCompute("shaders/gibs_surfel_generation.metal", "surfelGeneration", gibsShaders[0], 8, 8, 1);
        surfelUpdatePipeline   = makeNamedCompute("shaders/gibs_surfel_generation.metal", "surfelUpdate", gibsUpdateShader, 256, 1, 1);
        surfelClearPipeline    = makeNamedCompute("shaders/gibs_spatial_hash.metal", "clearCellHeads", gibsShaders[1], 256, 1, 1);
        surfelInsertPipeline   = makeNamedCompute("shaders/gibs_spatial_hash.metal", "insertSurfels", gibsShaders[2], 256, 1, 1);
        surfelRTPipeline       = makeNamedCompute("shaders/gibs_raytracing.metal", "surfelRaytracing", gibsShaders[3], 64, 1, 1);
        surfelTemporalPipeline = makeNamedCompute("shaders/gibs_temporal.metal", "surfelTemporalSmooth", gibsShaders[4], 256, 1, 1);
        giSamplePipeline       = makeNamedCompute("shaders/gibs_sample.metal", "giSample", gibsShaders[5], 8, 8, 1);
    }

    // ------------------------------------------------------------------------
    // Metal-only (non-RT) pipelines: clustered light culling (the Metal PBR
    // shader's cluster contract) and the sun/lens flare. GLSL twins for the
    // Vulkan backend land with the IBL round.
    // ------------------------------------------------------------------------
    if (backend == GraphicsBackend::Metal) {
        // IBL-from-sky chain (fullscreen-ish per-face captures into cubemaps).
        auto makeIblPipeline = [&](const char* path, ShaderHandle& vs, ShaderHandle& fs) -> PipelineHandle {
            std::string code = readFile(path);
            if (code.empty()) return {};
            ShaderDesc vd; vd.stage = ShaderStage::Vertex;   vd.code = code.data(); vd.codeSize = code.size(); vd.entryPoint = "vertexMain";
            vs = rhi->createShader(vd);
            ShaderDesc fd; fd.stage = ShaderStage::Fragment; fd.code = code.data(); fd.codeSize = code.size(); fd.entryPoint = "fragmentMain";
            fs = rhi->createShader(fd);
            PipelineDesc d;
            d.vertexShader = vs;
            d.fragmentShader = fs;
            d.vertexLayout.stride = 0;
            d.vertexLayout.attributes = {};
            d.topology = PrimitiveTopology::TriangleList;
            d.blendMode = BlendMode::Opaque;
            d.depthTest = false;
            d.depthWrite = false;
            d.cullMode = CullMode::None;
            d.sampleCount = 1;
            d.hasDepthAttachment = false;
            d.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
            return rhi->createPipeline(d);
        };
        skyCapturePipeline = makeIblPipeline("shaders/3d_sky_capture.metal", skyCaptureVS, skyCaptureFS);
        // Equirect HDRI -> cubemap face (reuses the native renderer's shader).
        equirectToCubemapPipeline = makeIblPipeline("shaders/3d_equirect_to_cubemap.metal", equirectToCubemapVS, equirectToCubemapFS);
        irradiancePipeline = makeIblPipeline("shaders/3d_irradiance_convolution.metal", irradianceVS, irradianceFS);
        prefilterPipeline  = makeIblPipeline("shaders/3d_prefilter_envmap.metal", prefilterVS, prefilterFS);
        brdfLUTPipeline    = makeIblPipeline("shaders/3d_brdf_lut.metal", brdfVS, brdfFS);

        std::string tcCode = readFile("shaders/3d_tile_light_cull.metal");
        if (!tcCode.empty()) {
            ShaderDesc d; d.stage = ShaderStage::Compute; d.code = tcCode.data();
            d.codeSize = tcCode.size(); d.entryPoint = "computeMain";
            tileCullingShader = rhi->createShader(d);
            ComputePipelineDesc cd; cd.computeShader = tileCullingShader;
            tileCullingPipeline = rhi->createComputePipeline(cd);
        }

        // GPU-driven frustum cull (Metal). Not RT-gated — GPU culling works on
        // any compute-capable device. Threadgroup 64 matches gpuCullPass's
        // dispatch of (n + 63) / 64 groups.
        std::string gcCode = readFile("shaders/3d_gpu_cull.metal");
        if (!gcCode.empty()) {
            ShaderDesc d; d.stage = ShaderStage::Compute; d.code = gcCode.data();
            d.codeSize = gcCode.size(); d.entryPoint = "computeMain";
            gpuCullShader = rhi->createShader(d);
            ComputePipelineDesc cd; cd.computeShader = gpuCullShader;
            cd.threadGroupSizeX = 64;
            gpuCullPipeline = rhi->createComputePipeline(cd);

            // ICB variant of the same cull: encodes draws straight into the
            // scene's indirect command buffer (see computeMainICB).
            if (capabilities.indirectCommandBuffers) {
                d.entryPoint = "computeMainICB";
                gpuCullICBShader = rhi->createShader(d);
                ComputePipelineDesc icd; icd.computeShader = gpuCullICBShader;
                icd.threadGroupSizeX = 64;
                gpuCullICBPipeline = rhi->createComputePipeline(icd);
            }
        }

        // --------------------------------------------------------------------
        // Post/effect chain from the native MSL files. The generic creation
        // below this block reads SPIR-V, which RHI_Metal can't build — without
        // these the Metal frame fell back to Main-direct-to-swapchain with no
        // sky, shadow, bloom, fog, god rays or post-processing.
        // --------------------------------------------------------------------
        auto makeMetalPass = [&](const char* path, const char* vsEntry, const char* fsEntry,
                                 BlendMode blend, std::vector<PixelFormat> colorFmts,
                                 bool depthTest, CompareOp depthCmp) -> PipelineHandle {
            std::string code = readFile(path);
            if (code.empty()) return {};
            ShaderDesc vd; vd.stage = ShaderStage::Vertex;   vd.code = code.data(); vd.codeSize = code.size(); vd.entryPoint = vsEntry;
            ShaderHandle vs = rhi->createShader(vd);
            ShaderDesc fdd; fdd.stage = ShaderStage::Fragment; fdd.code = code.data(); fdd.codeSize = code.size(); fdd.entryPoint = fsEntry;
            ShaderHandle fs = rhi->createShader(fdd);
            metalPassShaders.push_back(vs);
            metalPassShaders.push_back(fs);
            PipelineDesc d;
            d.vertexShader = vs;
            d.fragmentShader = fs;
            d.vertexLayout.stride = 0;
            d.vertexLayout.attributes = {};
            d.topology = PrimitiveTopology::TriangleList;
            d.blendMode = blend;
            d.depthTest = depthTest;
            d.depthWrite = false;
            d.depthCompareOp = depthCmp;
            d.cullMode = CullMode::None;
            d.sampleCount = 1;
            d.hasDepthAttachment = depthTest;
            if (depthTest) d.depthAttachmentFormat = PixelFormat::Depth32Float;
            d.colorAttachmentFormats = colorFmts;
            return rhi->createPipeline(d);
        };

        postProcessPipeline = makeMetalPass("shaders/3d_post_process.metal", "vertexMain", "fragmentMain",
                                            BlendMode::Opaque, { PixelFormat::Swapchain }, false, CompareOp::Less);
        bloomBrightPipeline = makeMetalPass("shaders/3d_bloom_brightness.metal", "vertexMain", "fragmentMain",
                                            BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        bloomDownsamplePipeline = makeMetalPass("shaders/3d_bloom_downsample.metal", "vertexMain", "fragmentMain",
                                                BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        // Hi-Z reduce: fullscreen max-depth downsample into an R32F mip.
        hizReducePipeline = makeMetalPass("shaders/3d_hiz_reduce.metal", "vertexMain", "fragmentMain",
                                          BlendMode::Opaque, { PixelFormat::R32_FLOAT }, false, CompareOp::Less);

        // Native upsample blends in-shader (texBlend at texture(1)) — Opaque,
        // unlike the Vulkan twin's additive-blend pipeline.
        bloomUpsamplePipeline = makeMetalPass("shaders/3d_bloom_upsample.metal", "vertexMain", "fragmentMain",
                                              BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        bloomCompositePipeline = makeMetalPass("shaders/3d_bloom_composite.metal", "vertexMain", "fragmentMain",
                                               BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        atmospherePipeline = makeMetalPass("shaders/3d_atmosphere.metal", "vertexMain", "fragmentMain",
                                           BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, true, CompareOp::LessOrEqual);
        // Skybox: visible sky from environmentCubemap (SkyType::HDRI), same
        // fullscreen depth-tested state as the atmosphere pass.
        skyboxPipeline = makeMetalPass("shaders/3d_skybox.metal", "vertexMain", "fragmentMain",
                                       BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, true, CompareOp::LessOrEqual);
        // Gradient: zenith/horizon/ground gradient sky (SkyType::Gradient), same
        // fullscreen depth-tested state as the atmosphere pass.
        gradientPipeline = makeMetalPass("shaders/3d_gradient.metal", "vertexMain", "fragmentMain",
                                         BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, true, CompareOp::LessOrEqual);
        // IBL debug: cubemap -> equirect 2D RT.
        iblPreviewPipeline = makeMetalPass("shaders/3d_ibl_equirect.metal", "vertexMain", "fragmentMain",
                                           BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        lightScatteringPipeline = makeMetalPass("shaders/3d_light_scattering.metal", "vertexMain", "fragmentMain",
                                                BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        heightFogPipeline = makeMetalPass("shaders/3d_height_fog.metal", "heightFogVertex", "heightFogFragment",
                                          BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        volumetricFogPipeline = makeMetalPass("shaders/3d_volumetric_fog.metal", "volumetricFogVertex", "simpleFogFragment",
                                              BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        // Froxel volumetric fog (Metal): inject + integrate compute kernels reused
        // from the same file, plus the froxel composite (volumetricFogFragment).
        // Not RT-gated, so a local compute-pipeline builder (makeNamedCompute lives
        // in the raytracing block above and is out of scope here).
        {
            auto makeFogCompute = [&](const char* entry, ShaderHandle& sh,
                                      Uint32 tgX, Uint32 tgY, Uint32 tgZ) -> ComputePipelineHandle {
                std::string code = readFile("shaders/3d_volumetric_fog.metal");
                if (code.empty()) return {};
                ShaderDesc d; d.stage = ShaderStage::Compute; d.code = code.data();
                d.codeSize = code.size(); d.entryPoint = entry;
                sh = rhi->createShader(d);
                ComputePipelineDesc cd; cd.computeShader = sh;
                cd.threadGroupSizeX = tgX; cd.threadGroupSizeY = tgY; cd.threadGroupSizeZ = tgZ;
                return rhi->createComputePipeline(cd);
            };
            fogFroxelInjectPipeline = makeFogCompute("froxelInjection", fogFroxelInjectShader, 4, 4, 4);
            fogFroxelIntegratePipeline = makeFogCompute("scatteringIntegration", fogFroxelIntegrateShader, 8, 8, 1);
            fogFroxelCompositePipeline = makeMetalPass("shaders/3d_volumetric_fog.metal",
                                                       "volumetricFogVertex", "volumetricFogFragment",
                                                       BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        }
        volumeRaymarchPipeline = makeMetalPass("shaders/3d_volume_raymarch.metal", "volumeRaymarchVertex", "volumeRaymarchFragment",
                                               BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        // Volumetric clouds (off by default): quarter-res raymarch ->
        // temporal resolve -> upscale composite, all from the native file.
        cloudRaymarchPipeline = makeMetalPass("shaders/3d_volumetric_clouds.metal", "cloudVertex", "cloudFragmentLowRes",
                                              BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        cloudTemporalPipeline = makeMetalPass("shaders/3d_volumetric_clouds.metal", "cloudVertex", "cloudTemporalResolve",
                                              BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);
        cloudCompositePipeline = makeMetalPass("shaders/3d_volumetric_clouds.metal", "cloudVertex", "cloudUpscaleComposite",
                                               BlendMode::Opaque, { PixelFormat::RGBA16_FLOAT }, false, CompareOp::Less);

        // PSSM shadow depth (3d_pssm_shadow_depth.metal): raw-fetched vertices,
        // depth-only with front-face culling, alpha-tested via texAlbedo.
        {
            std::string code = readFile("shaders/3d_pssm_shadow_depth.metal");
            if (!code.empty()) {
                ShaderDesc vd; vd.stage = ShaderStage::Vertex;   vd.code = code.data(); vd.codeSize = code.size(); vd.entryPoint = "vertexMain";
                ShaderHandle vs = rhi->createShader(vd);
                ShaderDesc fdd; fdd.stage = ShaderStage::Fragment; fdd.code = code.data(); fdd.codeSize = code.size(); fdd.entryPoint = "fragmentMain";
                ShaderHandle fs = rhi->createShader(fdd);
                metalPassShaders.push_back(vs);
                metalPassShaders.push_back(fs);
                PipelineDesc d;
                d.vertexShader = vs;
                d.fragmentShader = fs;
                d.vertexLayout.stride = 0;
                d.vertexLayout.attributes = {};
                d.topology = PrimitiveTopology::TriangleList;
                d.blendMode = BlendMode::Opaque;
                d.depthTest = true;
                d.depthWrite = true;
                d.depthCompareOp = CompareOp::Less;
                d.cullMode = CullMode::Front;
                d.frontFaceCounterClockwise = true;
                d.sampleCount = 1;
                d.hasDepthAttachment = true;
                d.depthAttachmentFormat = PixelFormat::Depth32Float;
                d.colorAttachmentFormats = {};
                shadowPipeline = rhi->createPipeline(d);
            }
        }

        // Velocity: native compute kernel (8x8 threadgroups, bounds-checked).
        {
            std::string code = readFile("shaders/3d_velocity.metal");
            if (!code.empty()) {
                ShaderDesc d; d.stage = ShaderStage::Compute; d.code = code.data();
                d.codeSize = code.size(); d.entryPoint = "computeMain";
                ShaderHandle sh = rhi->createShader(d);
                metalPassShaders.push_back(sh);
                ComputePipelineDesc cd; cd.computeShader = sh;
                cd.threadGroupSizeX = 8; cd.threadGroupSizeY = 8; cd.threadGroupSizeZ = 1;
                velocityComputePipeline = rhi->createComputePipeline(cd);
            }
        }

        // GPU particles from 3d_particle.metal: two compute kernels
        // (particleForce / particleIntegrate, 256-wide) + an instanced
        // billboard render (particleVertex / particleFragment). Only the
        // Vulkan block created these before, so Metal-RHI showed no particles.
        {
            std::string code = readFile("shaders/3d_particle.metal");
            if (!code.empty()) {
                auto makePC = [&](const char* entry) -> ComputePipelineHandle {
                    ShaderDesc d; d.stage = ShaderStage::Compute; d.code = code.data();
                    d.codeSize = code.size(); d.entryPoint = entry;
                    ShaderHandle sh = rhi->createShader(d);
                    metalPassShaders.push_back(sh);
                    ComputePipelineDesc cd; cd.computeShader = sh;
                    cd.threadGroupSizeX = 256; cd.threadGroupSizeY = 1; cd.threadGroupSizeZ = 1;
                    return rhi->createComputePipeline(cd);
                };
                particleForcePipeline     = makePC("particleForce");
                particleIntegratePipeline = makePC("particleIntegrate");

                ShaderDesc pvd; pvd.stage = ShaderStage::Vertex;   pvd.code = code.data(); pvd.codeSize = code.size(); pvd.entryPoint = "particleVertex";
                particleVertexShader = rhi->createShader(pvd);
                ShaderDesc pfd; pfd.stage = ShaderStage::Fragment; pfd.code = code.data(); pfd.codeSize = code.size(); pfd.entryPoint = "particleFragment";
                particleFragmentShader = rhi->createShader(pfd);
                metalPassShaders.push_back(particleVertexShader);
                metalPassShaders.push_back(particleFragmentShader);

                PipelineDesc pd;
                pd.vertexShader = particleVertexShader;
                pd.fragmentShader = particleFragmentShader;
                pd.vertexLayout.stride = 0;   // billboard verts generated in-shader
                pd.vertexLayout.attributes = {};
                pd.topology = PrimitiveTopology::TriangleList;
                pd.depthTest = true;
                pd.depthWrite = false;
                pd.depthCompareOp = CompareOp::LessOrEqual;
                pd.cullMode = CullMode::None;
                pd.sampleCount = 1;
                pd.hasDepthAttachment = true;
                pd.depthAttachmentFormat = PixelFormat::Depth32Float;
                pd.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
                const BlendMode particleBlends[PARTICLE_BLEND_COUNT] = {
                    BlendMode::Additive, BlendMode::AlphaBlend, BlendMode::Multiply
                };
                for (Uint32 i = 0; i < PARTICLE_BLEND_COUNT; ++i) {
                    pd.blendMode = particleBlends[i];
                    particleRenderPipelines[i] = rhi->createPipeline(pd);
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // Sun/lens flare (clean-room redesign, both backends): SunFlare.frag on
    // Vulkan, sunflare_rhi.metal on Metal — the same algorithm twice. Additive
    // over the HDR colorRT; occlusion sampled from scene depth in-shader.
    // ------------------------------------------------------------------------
    {
        std::string sfv, sff;
        const char* entryV = "main";
        const char* entryF = "main";
        if (backend == GraphicsBackend::Vulkan) {
            sfv = readFile("shaders/FullScreen.vert.spv");
            sff = readFile("shaders/SunFlare.frag.spv");
        } else if (backend == GraphicsBackend::Metal) {
            sfv = readFile("shaders/sunflare_rhi.metal");
            sff = sfv;
            entryV = "vertexMain";
            entryF = "fragmentMain";
        }
        if (!sfv.empty() && !sff.empty()) {
            ShaderDesc vd; vd.stage = ShaderStage::Vertex;   vd.code = sfv.data(); vd.codeSize = sfv.size(); vd.entryPoint = entryV;
            sunFlareVertexShader = rhi->createShader(vd);
            ShaderDesc fd; fd.stage = ShaderStage::Fragment; fd.code = sff.data(); fd.codeSize = sff.size(); fd.entryPoint = entryF;
            sunFlareFragmentShader = rhi->createShader(fd);
            PipelineDesc pd;
            pd.vertexShader = sunFlareVertexShader;
            pd.fragmentShader = sunFlareFragmentShader;
            pd.vertexLayout.stride = 0;
            pd.vertexLayout.attributes = {};
            pd.topology = PrimitiveTopology::TriangleList;
            pd.blendMode = BlendMode::Additive;
            pd.depthTest = false;
            pd.depthWrite = false;
            pd.cullMode = CullMode::None;
            pd.sampleCount = 1;
            pd.hasDepthAttachment = false;
            pd.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
            sunFlarePipeline = rhi->createPipeline(pd);
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

    // Create new texture with a full mip chain. Native (renderer_metal.cpp
    // createTexture) sizes material textures to calculateMipmapLevelCount =
    // floor(log2(max(w,h))) + 1 and blits the chain with generateMipmaps; the
    // RHI path previously left mipLevels at 1, so minified surfaces sampled
    // only the base level and shimmered/aliased. Match native exactly.
    Uint32 mipLevels = static_cast<Uint32>(
        std::floor(std::log2(std::max(image->width, image->height)))) + 1u;
    TextureDesc texDesc;
    texDesc.width = image->width;
    texDesc.height = image->height;
    texDesc.format = PixelFormat::RGBA8_UNORM;
    texDesc.usage = TextureUsage::Sampled;
    texDesc.mipLevels = mipLevels;
    TextureHandle texHandle = rhi->createTexture(texDesc);

    if (!image->byteArray.empty()) {
        // Uploads the base level (mip 0); generateMipmaps fills the rest.
        rhi->updateTexture(texHandle, image->byteArray.data(), image->byteArray.size());
        if (mipLevels > 1) {
            rhi->generateMipmaps(texHandle);
        }
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

void Renderer::bindMaterialAlbedo(MaterialId materialId) {
    if (materialId >= materials.size()) return;
    const RenderMaterial& material = materials[materialId];
    if (material.albedoTexture < textures.size()) {
        const RenderTexture& tex = textures[material.albedoTexture];
        rhi->setTexture(0, 0, tex.handle, tex.sampler);  // texture(0) / set2 b0
    }
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

std::unique_ptr<IRenderer> Vapor::createRenderer(GraphicsBackend backend, SDL_Window* window) {
#ifdef __APPLE__
    // Metal now routes through the RHI renderer by default (the target
    // architecture: renderer -> RHI -> {rhi_metal, rhi_vulkan}). Set
    // VAPOR_METAL_NATIVE=1 to fall back to the legacy full-feature native
    // renderer (45 passes) — kept for A/B comparison and for the handful of
    // passes not yet on the RHI path.
    if (backend == GraphicsBackend::Metal && std::getenv("VAPOR_METAL_NATIVE")) {
        return createRendererMetal(window);
    }
#endif

    std::unique_ptr<RHI> rhi;

    switch (backend) {
        case GraphicsBackend::Vulkan:
            rhi = std::unique_ptr<RHI>(createRHIVulkan());
            break;
#ifdef __APPLE__
        case GraphicsBackend::Metal:
            rhi = std::unique_ptr<RHI>(createRHIMetal());
            break;
#endif
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
                // ImGui cycles ImageCount sets of vertex/index buffers and
                // destroys a slot's buffer when it needs to grow. That is only
                // safe if the slot's previous frame has retired — so the count
                // must be at least the engine's frames-in-flight. The old
                // hardcoded 2 (vs 3 in flight) had ImGui resizing buffers a
                // still-pending frame was drawing from: validation errors
                // (VUID-vkDestroyBuffer-00922) and UI flicker under churn.
                Uint32 imageCount = rhi->getMaxFramesInFlight();

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
        renderer->setWindow(window);
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

void Renderer::stage(std::shared_ptr<RenderScene> scene) {
    if (!scene) return;
    currentScene = scene;  // for the ImGui Scene Materials/Lights editors

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
                matData.transmission = mesh->material->transmission;
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

void Renderer::draw(std::shared_ptr<RenderScene> scene, Camera& camera) {
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

void Renderer::draw(entt::registry& registry, std::shared_ptr<RenderScene> scene, Camera& camera) {
    if (!scene) return;
    lastDrawRegistry = &registry;  // renderToTexture collects through this

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

void Renderer::submitSceneLights(const std::shared_ptr<RenderScene>& scene) {
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
    for (const auto& l : scene->spotLights) {
        if (spotLights.size() >= maxSpotLights) break;
        spotLights.push_back(l);
    }
}

// Non-ECS collection is intentionally a no-op: the scene-node tree has been
// retired — game objects live in the ECS, and every collector should go
// through the registry overload below (renderToTexture uses lastDrawRegistry).
void Renderer::collectDrawables(std::shared_ptr<RenderScene> scene) {
}

void Renderer::collectDrawables(entt::registry& registry, std::shared_ptr<RenderScene> scene) {
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

    // Sprites are NOT collected here: the game's Sprite2DRenderSystem submits
    // them via drawQuad2D with the atlas texture and full world transform.
    // (A leftover placeholder here used to draw every Sprite2DComponent a second
    // time as an axis-aligned white quad — ghost sprite on top of the real one.)
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
    drawCpuTimingsImGui();

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
    // GPU-driven-prepass modes skip the CPU cull, so there's no CPU "visible"
    // count — show "-" (the real post-cull count is in the GPUDRV --stats source).
    if (gpuDrivenPrePassActive()) {
        ImGui::Text("Drawables: - / %u visible", lastFrameStats.totalDrawables);
    } else {
        ImGui::Text("Drawables: %u / %u visible",
                    lastFrameStats.visibleDrawables, lastFrameStats.totalDrawables);
    }
    // Main-pass path + submission count: the direct signal that MDI/GPU-driven is
    // engaged. Switching Off->Indirect+MDI should collapse this from ~drawable
    // count to ~material count.
    ImGui::Text("Main pass: %s | %u draw calls", lastFrameStats.mainPath, lastFrameStats.mainDrawCalls);
    ImGui::Text("Scene lights: dir %u | point %u | rect %u | spot %u",
                lastFrameStats.directionalLights, lastFrameStats.pointLights,
                lastFrameStats.rectLights, lastFrameStats.spotLights);
    ImGui::Text("Raytracing: %s | Compute: %s | GPU timestamps: %s",
                capabilities.raytracing ? "yes" : "no",
                capabilities.computeShaders ? "yes" : "no",
                capabilities.gpuTimestamps ? "yes" : "no");
    ImGui::Text("Frame: %u", frameNumber);

    // GPU-driven geometry submission — mutually-exclusive method (a mesh can't go
    // through both the vertex pipeline and mesh shaders). Off = CPU cull +
    // drawIndexed (existing path, untouched). Requires compute.
    if (capabilities.computeShaders) {
        ImGui::Text("GPU-driven method:");
        if (ImGui::RadioButton("Off (CPU cull)", gpuDrivenMode == GpuDrivenMode::Off))
            gpuDrivenMode = GpuDrivenMode::Off;
        ImGui::SameLine();
        if (ImGui::RadioButton("Indirect + MDI", gpuDrivenMode == GpuDrivenMode::Indirect))
            gpuDrivenMode = GpuDrivenMode::Indirect;
        ImGui::SameLine();
        // Bindless MDI: same compute cull, but the whole scene in ONE
        // submission with bindlessly-fetched material textures. Metal = ICB
        // replay + argument tables; Vulkan = one native multi-draw + set-3
        // descriptor array.
        const bool bindlessSelectable = capabilities.bindlessTextures &&
            mainPipelineBindless.isValid() &&
            (backend == GraphicsBackend::Metal
                 ? (capabilities.indirectCommandBuffers && gpuCullICBPipeline.isValid())
                 : capabilities.multiDrawIndirect);
        ImGui::BeginDisabled(!bindlessSelectable);
        if (ImGui::RadioButton("Bindless MDI", gpuDrivenMode == GpuDrivenMode::BindlessMDI))
            gpuDrivenMode = GpuDrivenMode::BindlessMDI;
        ImGui::EndDisabled();
        if (!bindlessSelectable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Needs bindless textures (Metal Tier-2 argument buffers / "
                              "Vulkan descriptor indexing)");
        }
        if (gpuDrivenMode == GpuDrivenMode::BindlessMDI && !bindlessSelectable)
            gpuDrivenMode = GpuDrivenMode::Off;  // never stick on an unavailable mode

        // MDI is a sub-option of the plain Indirect method (single-call
        // multi-draw per material over the merged scene buffers; Vulkan
        // native, Metal per-command loop). Bindless MDI is its own mode above.
        const bool mdiAvailable = gpuDrivenMode == GpuDrivenMode::Indirect &&
            ((backend == GraphicsBackend::Vulkan && capabilities.multiDrawIndirect) ||
             backend == GraphicsBackend::Metal);
        ImGui::BeginDisabled(!mdiAvailable);
        ImGui::Checkbox("  -> MDI (per material)", &gpuDrivenMDI);
        ImGui::EndDisabled();
        if (!mdiAvailable) gpuDrivenMDI = false;

        // Hi-Z occlusion is orthogonal — it refines whichever GPU-driven mode is
        // active (object cull in Indirect).
        ImGui::BeginDisabled(!gpuDrivenActive());
        ImGui::Checkbox("Hi-Z occlusion culling", &gpuOcclusionCulling);
        ImGui::EndDisabled();
        if (!gpuDrivenActive()) gpuOcclusionCulling = false;
    } else {
        ImGui::BeginDisabled();
        ImGui::Text("GPU-driven method: (requires compute)");
        ImGui::EndDisabled();
    }

    // Aspect-correct texture preview, shared by the RTs and Shadow Debug nodes.
    Uint32 rtW = rhi->getSwapchainWidth();
    Uint32 rtH = rhi->getSwapchainHeight();
    float rtAspect = rtH > 0 ? static_cast<float>(rtW) / static_cast<float>(rtH) : 1.0f;
    auto preview = [&](const char* label, TextureHandle tex) {
        if (!tex.isValid()) return;
        if (ImGui::TreeNode(label)) {
            // Swapchain dims, NOT the texture's (several RTs are half-res).
            ImGui::Text("%u x %u (swapchain)", rtW, rtH);
            if (void* id = getImGuiTextureID(tex)) {
                // Opaque backdrop: several RTs clear their alpha to 0 (the
                // prepass normal/albedo MRT, half-res RTs), so ImGui::Image
                // would alpha-blend the transparent (no-geometry) regions with
                // the panel and read as "empty gray". Fill black first so those
                // regions show the true cleared RGB, not the panel background.
                ImVec2 sz(320, 320 / rtAspect);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(0, 0, 0, 255));
                ImGui::Image((ImTextureID)(intptr_t)id, sz);
            } else {
                ImGui::TextDisabled("(preview unavailable on this backend)");
            }
            ImGui::TreePop();
        }
    };

    // Render-target viewer
    if (ImGui::TreeNode("RTs")) {
        preview("Color RT", colorRT);
        preview("Normal RT", normalRT);
        preview("Reflection RT", reflectionRT);
        preview("Refraction RT", refractionRT);
        preview("AO RT", aoRT);
        preview("Velocity RT", velocityRT);
        preview("God Rays RT", lightScatteringRT);
        preview("Cloud RT", cloudHistoryRT);
        ImGui::TreePop();
    }

    // ------------------------------------------------------------------------
    // Per-pass parameter sections — native drawDebugImGui parity (grouping and
    // labels match renderer_metal.cpp so users find the same knobs).
    // ------------------------------------------------------------------------

    // Light Culling Debug (above Shadow Debug): live per-tile point-light counts
    // from the tile cull, plus the perf-isolation toggle for the point-light
    // loop. The avg is exactly the loop length the Main pass runs per pixel.
    {
        bool open = ImGui::TreeNode("Light Culling Debug");
        lightCullDebugOpen = open;  // gates the throttled readback in tileCullingPass
        if (open) {
            ImGui::Text("Scene point lights: %zu", pointLights.size());
            const Uint32 tiles = clusterGridSizeX * clusterGridSizeY;
            ImGui::Text("Tiles: %u (%ux%u)", tiles, clusterGridSizeX, clusterGridSizeY);
            ImGui::Text("Culled per tile:  avg %u   (min %u / max %u)",
                        cullAvgLights, cullMinLights, cullMaxLights);
            ImGui::Text("Non-empty tiles: %u / %u", cullNonEmptyTiles, tiles);
            ImGui::TextDisabled("avg = point-light loop length per pixel (refreshed ~15f)");
            bool skipPoint = (mainDebugFlags & 1u) != 0u;
            if (ImGui::Checkbox("Skip point-light loop (perf isolation)", &skipPoint))
                mainDebugFlags = (mainDebugFlags & ~1u) | (skipPoint ? 1u : 0u);
            ImGui::TreePop();
        }
    }

    if (ImGui::TreeNode("Shadow Debug")) {
        // Shadow scope, one control (state derived from the two flags it drives):
        //   Off              = no shadows          (mainDebugFlags bit1 set)
        //   Directional only = sun/PSSM only       (default)
        //   All shadows      = + stochastic RT      (stochastic: point/rect/spot)
        int shadowMode = (mainDebugFlags & 2u) ? 0 : (stochasticShadowsEnabled ? 2 : 1);
        if (ImGui::Combo("Shadows", &shadowMode, "Off\0Directional only\0All shadows\0")) {
            mainDebugFlags = (shadowMode == 0) ? (mainDebugFlags | 2u) : (mainDebugFlags & ~2u);
            stochasticShadowsEnabled = (shadowMode == 2);
        }
        if (shadowMode == 2)
            ImGui::TextDisabled("stochastic RT shadows noisy until ReSTIR denoise (Metal RT only)");
        // Condition BEFORE TreeNode: TreeNode() pushes when expanded and then
        // demands a matching TreePop(). Testing shadowMode after it with && would
        // short-circuit past the TreePop when a node is open but the mode does
        // not match — the missing-TreePop imbalance. Gating first means TreeNode
        // only runs (and only pushes) when the node should show at all.
        if ((shadowMode == 1 || shadowMode == 2) && ImGui::TreeNode("Directional shadow")) {
            ImGui::SliderFloat("Near shadow distance", &pssmRTMaxDist, 5.0f, 200.0f);
            // 0 = hard single-ray sun shadow (legacy); >0 = cone-sampled penumbra
            // (4 rays/pixel; real sun ~0.0047 rad). Metal RT only; no denoiser
            // yet, so large radii show noise in the penumbra.
            ImGui::SliderFloat("RT sun angular radius", &rtSunAngularRadius, 0.0f, 0.05f, "%.4f");
            {
                // PCF taps for the PSSM cascades + near map (4/8/16/32 Poisson)
                const char* pcfLabels[] = { "4", "8", "16", "32" };
                const Uint32 pcfValues[] = { 4u, 8u, 16u, 32u };
                int idx = 2;
                for (int i = 0; i < 4; ++i) if (pssmPcfSampleCount == pcfValues[i]) idx = i;
                if (ImGui::Combo("PCF samples", &idx, pcfLabels, 4)) pssmPcfSampleCount = pcfValues[idx];
            }
            ImGui::SliderFloat("Cascade blend", &pssmCascadeBlendRange, 0.0f, 10.0f);
            ImGui::Checkbox("Visualize cascades", &pssmDebugVisualize);
            if (TextureHandle vt = capabilities.raytracing ? shadowRT : debugView("nearShadow", nearShadowMap, TextureSwizzle::RRR1, 0); vt.isValid()) {
                if (void* id = getImGuiTextureID(vt)) {
                    ImGui::Image((ImTextureID)(intptr_t)id, ImVec2(320, 320 / rtAspect));
                }
            }
            // PSSM cascades: one 2D grayscale view per array layer of the 3-cascade
            // depth array (createTextureView returns invalid for a missing layer, so
            // the preview simply skips it).
            for (Uint32 c = 0; c < 3u; ++c) {
                char key[24];
                std::snprintf(key, sizeof(key), "pssmC%u", c);
                char label[40];
                std::snprintf(label, sizeof(label), "PSSM Cascade %u (depth)", c);
                preview(label, debugView(key, pssmShadowArrayTexture, TextureSwizzle::RRR1, c));
            }
            ImGui::TreePop();
        }
        if (shadowMode == 2 && ImGui::TreeNode("Stochastic shadows")) {
            const bool restirAvailable = restirShadowTemporalPipeline.isValid() &&
                                         restirShadowResolvePipeline.isValid() &&
                                         stochasticShadowUpsamplePipeline.isValid();
            // No history invalidation needed on toggles: restirShadowPass
            // trusts history only when it also ran the previous frame.
            ImGui::Checkbox("ReSTIR denoise (reservoir reuse)", &restirShadowsEnabled);
            if (restirShadowsEnabled && restirAvailable) {
                int cand = static_cast<int>(restirPointCandidates);
                if (ImGui::SliderInt("Light candidates", &cand, 1, 16))
                    restirPointCandidates = static_cast<Uint32>(cand);
                int taps = static_cast<int>(restirSpatialTaps);
                if (ImGui::SliderInt("Spatial taps", &taps, 0, 8))
                    restirSpatialTaps = static_cast<Uint32>(taps);
                ImGui::SliderFloat("Spatial radius (px)", &restirSpatialRadius, 2.0f, 32.0f);
                ImGui::SliderFloat("History clamp (xM)", &restirPointMClamp, 1.0f, 40.0f);
                if (restirReservoirHistory.isValid()) {
                    const Uint32 rw = (rhi->getSwapchainWidth() + 1) / 2;
                    const Uint32 rh = (rhi->getSwapchainHeight() + 1) / 2;
                    const double mb = double(rw) * double(rh)
                                      * sizeof(ShadowReservoirSetCPU) * 2.0 / (1024.0 * 1024.0);
                    ImGui::TextDisabled("reservoirs: %ux%u (half res), %.0f MB", rw, rh, mb);
                }
                // Live chain status — a stage silently skipping (missing
                // pipeline, TLAS not ready) shows up here instead of only as
                // unexplained noise.
                ImGui::TextDisabled("chain: %s > upsample > accumulate > %s",
                                    (restirLastFrame == frameNumber) ? "restir(1/2res)" : "legacy/skip",
                                    stochasticShadowDenoiseRan ? "denoise" : "raw");
            } else if (restirShadowsEnabled) {
                ImGui::TextDisabled("ReSTIR pipelines unavailable (Metal RT only)");
            } else {
                ImGui::TextDisabled("legacy uniform light picks (noisy) — ReSTIR off");
            }
            // Debug view of the stochastic shadow output
            int psd = static_cast<int>(stochasticShadowDebugMode);
            if (ImGui::Combo("View", &psd,
                             "Visibility\0Light-count heatmap\0ReSTIR winner id\0ReSTIR confidence (M)\0")) {
                stochasticShadowDebugMode = static_cast<Uint32>(psd);
            }
            if (stochasticShadowDebugMode == 1) {
                ImGui::TextWrapped("Heatmap: black = tile has 0 lights, brighter = more (8+ ~ white). "
                                   "Shown in 'Stochastic Shadow (raw)' below.");
            } else if (stochasticShadowDebugMode >= 2) {
                ImGui::TextWrapped("ReSTIR-only view. Winner id: color bands per selected light — "
                                   "stable bands mean the reservoir has locked on. Confidence: reservoir "
                                   "M vs the history clamp. Note a domain with no winner reads as 0 here "
                                   "but 1.0 (lit) in Visibility, since 'nothing selected' = 'no shadow'. "
                                   "Like the heatmap, the view replaces the shadow factors, so scene "
                                   "lighting is affected while it is active.");
            }
            if (TextureHandle vt = debugView("psRaw", stochasticShadowHalfRT, TextureSwizzle::Identity, 0); vt.isValid()) {
                if (void* id = getImGuiTextureID(vt)) {
                    ImGui::Image((ImTextureID)(intptr_t)id, ImVec2(320, 320 / rtAspect));
                }
            }
            ImGui::TreePop();
        }
        if ((shadowMode == 1 || shadowMode == 2) && ImGui::TreeNode("Contact shadows (SSCS)")) {
            ImGui::Checkbox("Enabled", &sscsEnabled);
            if (sscsEnabled) {
                ImGui::SliderFloat("SSCS length", &sscsLength, 0.05f, 2.0f);
                ImGui::SliderFloat("SSCS thickness", &sscsThickness, 0.05f, 2.0f);
                if (TextureHandle vt = debugView("sscs", sscsRT, TextureSwizzle::RRR1, 0); vt.isValid()) {
                    if (void* id = getImGuiTextureID(vt)) {
                        ImGui::Image((ImTextureID)(intptr_t)id, ImVec2(320, 320 / rtAspect));
                    }
                }
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    // Scene material editor — edits the renderer's live copies; updateBuffers()
    // re-uploads MaterialData every frame, so changes apply immediately.
    if (ImGui::TreeNode("Scene Materials")) {
        for (size_t i = 0; i < materials.size(); i++) {
            RenderMaterial& m = materials[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode("", "Material %zu", i)) {
                ImGui::ColorEdit4("Base Color Factor", &m.baseColorFactor.x);
                ImGui::DragFloat("Normal Scale", &m.normalScale, 0.05f, 0.0f, 5.0f);
                ImGui::DragFloat("Roughness Factor", &m.roughnessFactor, 0.05f, 0.0f, 5.0f);
                ImGui::DragFloat("Metallic Factor", &m.metallicFactor, 0.05f, 0.0f, 5.0f);
                ImGui::DragFloat("Occlusion Strength", &m.occlusionStrength, 0.05f, 0.0f, 5.0f);
                ImGui::ColorEdit3("Emissive Color Factor", &m.emissiveFactor.x);
                ImGui::DragFloat("Emissive Strength", &m.emissiveStrength, 0.05f, 0.0f, 5.0f);
                ImGui::DragFloat("Subsurface", &m.subsurface, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Specular", &m.specular, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Specular Tint", &m.specularTint, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Anisotropic", &m.anisotropic, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Sheen", &m.sheen, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Sheen Tint", &m.sheenTint, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Clearcoat", &m.clearcoat, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Clearcoat Gloss", &m.clearcoatGloss, 0.01f, 0.0f, 1.0f);
                // KHR_materials_transmission (RT refraction; importer doesn't
                // parse the extension yet — this slider is the test hook).
                ImGui::DragFloat("Transmission", &m.transmission, 0.01f, 0.0f, 1.0f);
                ImGui::Checkbox("Use IBL", &m.useIBL);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // Scene light editor — edits the shared Scene light lists, which
    // submitSceneLights() re-reads every frame (same flow as native).
    if (currentScene && ImGui::TreeNode("Scene Lights")) {
        int lid = 0;
        for (auto& l : currentScene->directionalLights) {
            ImGui::PushID(lid++);
            if (ImGui::TreeNode("", "Directional %d", lid)) {
                ImGui::DragFloat3("Direction", &l.direction.x, 0.1f);
                ImGui::ColorEdit3("Color", &l.color.x);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f, 10000.0f);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        for (auto& l : currentScene->pointLights) {
            ImGui::PushID(lid++);
            if (ImGui::TreeNode("", "Point %d", lid)) {
                ImGui::DragFloat3("Position", &l.position.x, 0.1f);
                ImGui::ColorEdit3("Color", &l.color.x);
                ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0001f, 10000.0f);
                ImGui::DragFloat("Radius", &l.radius, 0.1f, 0.0001f, 1000.0f);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // Atmosphere — edits atmosphereData (re-uploaded every frame in
    // updateBuffers). Changing it refreshes the sky-derived IBL like native.
    if (ImGui::TreeNode("Atmosphere")) {
        bool ch = false;
        ch |= ImGui::DragFloat3("Sun Direction", &atmosphereData.sunDirection.x, 0.01f, -1.0f, 1.0f);
        ch |= ImGui::DragFloat("Sun Intensity", &atmosphereData.sunIntensity, 0.5f, 0.0f, 100.0f);
        ch |= ImGui::ColorEdit3("Sun Color", &atmosphereData.sunColor.x);
        ch |= ImGui::DragFloat("Exposure", &atmosphereData.exposure, 0.01f, 0.01f, 10.0f);
        ch |= ImGui::ColorEdit3("Ground Color", &atmosphereData.groundColor.x);
        if (ImGui::TreeNode("Advanced")) {
            ch |= ImGui::DragFloat("Planet Radius (m)", &atmosphereData.planetRadius, 1000.0f, 1e3f, 1e8f);
            ch |= ImGui::DragFloat("Atmosphere Radius (m)", &atmosphereData.atmosphereRadius, 1000.0f, 1e3f, 1e8f);
            ch |= ImGui::DragFloat("Rayleigh Scale Height", &atmosphereData.rayleighScaleHeight, 100.0f, 100.0f, 50000.0f);
            ch |= ImGui::DragFloat("Mie Scale Height", &atmosphereData.mieScaleHeight, 100.0f, 100.0f, 10000.0f);
            ch |= ImGui::DragFloat("Mie Direction (g)", &atmosphereData.miePreferredDirection, 0.01f, -0.999f, 0.999f);
            glm::vec3 rayleighScaled = atmosphereData.rayleighCoefficients * 1e6f;
            if (ImGui::DragFloat3("Rayleigh R/G/B (x1e-6)", &rayleighScaled.x, 0.1f, 0.0f, 100.0f)) {
                atmosphereData.rayleighCoefficients = rayleighScaled * 1e-6f;
                ch = true;
            }
            float mieScaled = atmosphereData.mieCoefficient * 1e6f;
            if (ImGui::DragFloat("Mie Coeff (x1e-6)", &mieScaled, 0.1f, 0.0f, 100.0f)) {
                atmosphereData.mieCoefficient = mieScaled * 1e-6f;
                ch = true;
            }
            if (ImGui::Button("Reset to Earth Defaults")) {
                glm::vec3 keepDir = atmosphereData.sunDirection;
                glm::vec3 keepCol = atmosphereData.sunColor;
                float keepInt = atmosphereData.sunIntensity;
                atmosphereData = AtmosphereRenderData{};
                atmosphereData.sunDirection = keepDir;
                atmosphereData.sunColor = keepCol;
                atmosphereData.sunIntensity = keepInt;
                ch = true;
            }
            ImGui::TreePop();
        }
        if (ImGui::Button("Refresh IBL")) iblNeedsUpdate = true;
        ImGui::SameLine();
        // Sun-driven auto rebake (SkySystem's throttled requestIBLUpdate). Off by
        // default: a moving sun would otherwise re-bake the IBL every threshold step.
        ImGui::Checkbox("Auto rebake", &m_iblAutoRebake);
        if (ch) iblNeedsUpdate = true;  // sky changed -> recapture IBL (native behavior)
        // Debug: the baked environment cubemap, unwrapped to equirect. Off by
        // default — the pass stalls (single-buffered RT read by ImGui), so only
        // render + show it while this is ticked.
        ImGui::Checkbox("Show environment cubemap (equirect)", &m_iblPreviewEnabled);
        if (m_iblPreviewEnabled && iblPreviewRT.isValid()) {
            if (void* id = getImGuiTextureID(iblPreviewRT)) {
                ImGui::Image((ImTextureID)(intptr_t)id, ImVec2(320, 160));
            } else {
                ImGui::TextDisabled("(IBL preview unavailable on this backend)");
            }
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Water Settings")) {
        ImGui::TextDisabled("(water pass not ported to the RHI renderer yet)");
        ImGui::TreePop();
    }

    // RT mirror reflections (Metal RT only — no-op without a TLAS). Hits shade
    // from the GIBS surfel cache, so enable GI for full reflections; with GI
    // off, only sky/environment misses reflect.
    if (ImGui::TreeNode("RT Reflections")) {
        ImGui::Checkbox("Enabled", &rtReflectionsEnabled);
        ImGui::SliderFloat("Intensity", &rtReflectionIntensity, 0.0f, 2.0f);
        ImGui::TextDisabled("standalone hit shading (base color + sun); GI adds indirect bounce");
        ImGui::TreePop();
    }

    // RT refractions (Metal RT only): renders KHR_materials_transmission with
    // a fixed IOR of 1.5. The trace only runs while some material has
    // transmission > 0 — use the Scene Materials editor to set one.
    if (ImGui::TreeNode("RT Refractions")) {
        ImGui::Checkbox("Enabled", &rtRefractionsEnabled);
        ImGui::SliderFloat("Intensity", &rtRefractionIntensity, 0.0f, 2.0f);
        ImGui::TextDisabled(sceneHasTransmission
                                ? "active (a material transmits)"
                                : "idle: no material has transmission > 0");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Ambient Occlusion")) {
        ImGui::Checkbox("Enabled", &aoEnabled);
        if (capabilities.raytracing) {
            ImGui::Combo("Method", &aoMethod, "Ray Traced\0Screen Space\0");
        } else {
            ImGui::BeginDisabled();
            int forced = 1;
            ImGui::Combo("Method", &forced, "Ray Traced\0Screen Space\0");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(no RT: SSAO forced)");
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Global Illumination (GIBS)")) {
        if (!capabilities.raytracing) ImGui::TextDisabled("(requires raytracing)");
        ImGui::Checkbox("Enabled", &gibsEnabled);
        ImGui::Text("Active surfels: %u / %u", gibsActiveSurfels, gibsMaxSurfels);
        if (ImGui::Button("Reset Surfels")) gibsResetRequested = true;
        // Temporal reprojection + edge-aware a-trous over the GI gather —
        // toggle off to see the raw surfel discs/flicker for comparison.
        ImGui::Checkbox("Screen-space denoise (SVGF-lite)", &gibsDenoiseEnabled);
        if (ImGui::Combo("Quality", &gibsQuality, "Low\0Medium\0High\0Ultra\0")) {
            Uint32 newMax; Uint32 newRays; float newScale;
            getGIBSQualitySettings(static_cast<GIBSQuality>(gibsQuality), newMax, newRays, newScale);
            // Buffers were sized at init for the default (Medium) preset; clamp
            // like native so kernels never write past the allocated pool.
            gibsMaxSurfels = std::min(newMax, 500000u);
            gibsRaysPerSurfel = newRays;
            if (newScale != gibsResolutionScale) {
                gibsResolutionScale = newScale;
                // Force a render-target rebuild next beginFrame so the GI
                // texture is recreated at the new scale (UINT32_MAX, not 0 —
                // zero means "first frame, targets already built").
                lastRTWidth = UINT32_MAX;
            }
            gibsResetRequested = true;  // surfel pool contents are preset-dependent
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Light Scattering (God Rays)")) {
        ImGui::Checkbox("Enabled", &lightScatteringEnabled);
        int samples = static_cast<int>(lightScatteringSettings.numSamples);
        if (ImGui::SliderInt("Samples", &samples, 8, 128))
            lightScatteringSettings.numSamples = static_cast<Uint32>(samples);
        ImGui::DragFloat("Max Distance", &lightScatteringSettings.maxDistance, 0.01f, 0.1f, 2.0f);
        ImGui::DragFloat("Density", &lightScatteringSettings.density, 0.01f, 0.0f, 5.0f);
        ImGui::DragFloat("Weight", &lightScatteringSettings.weight, 0.001f, 0.001f, 0.1f);
        ImGui::DragFloat("Decay", &lightScatteringSettings.decay, 0.001f, 0.9f, 1.0f);
        ImGui::DragFloat("Exposure", &lightScatteringSettings.exposure, 0.01f, 0.0f, 2.0f);
        ImGui::DragFloat("Sun Intensity", &lightScatteringSettings.sunIntensity, 0.1f, 0.0f, 10.0f);
        ImGui::DragFloat("Mie G (Phase)", &lightScatteringSettings.mieG, 0.01f, -0.99f, 0.99f);
        ImGui::DragFloat("Depth Threshold", &lightScatteringSettings.depthThreshold, 0.0001f, 0.99f, 1.0f, "%.4f");
        ImGui::DragFloat("Temporal Jitter", &lightScatteringSettings.jitter, 0.01f, 0.0f, 1.0f);
        if (ImGui::Button("Reset to Defaults")) lightScatteringSettings = LightScatteringRenderData{};
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Height Fog")) {
        // Cheap analytic exponential height fog (the common-case global fog).
        ImGui::Checkbox("Enabled", &heightFogEnabled);
        ImGui::DragFloat("Density", &heightFogSettings.fogColorDensity.a, 0.001f, 0.0f, 0.5f);
        ImGui::DragFloat("Height Falloff", &heightFogSettings.params.x, 0.01f, 0.001f, 1.0f);
        ImGui::DragFloat("Base Height", &heightFogSettings.params.y, 1.0f, -100.0f, 100.0f);
        ImGui::DragFloat("Anisotropy", &heightFogSettings.params.z, 0.01f, -0.99f, 0.99f);
        ImGui::DragFloat("Ambient Intensity", &heightFogSettings.params.w, 0.01f, 0.0f, 2.0f);
        ImGui::ColorEdit3("Fog Color", &heightFogSettings.fogColorDensity.x);
        if (ImGui::Button("Reset to Defaults")) heightFogSettings = HeightFogRenderData{};
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Volumetric Fog")) {
        // ECS-driven: VolumetricFogSystem pushes the scene's fog volumes and gates
        // the pass on whether any exist. This panel only selects the render path.
        ImGui::Text(volumetricFogEnabled ? "Active - %d volume(s)" : "Inactive (no fog volumes)",
                    static_cast<int>(volumetricFogVolumes.size()));
        if (backend == GraphicsBackend::Metal) {
            ImGui::Checkbox("Use froxel grid (off = raymarch)", &fogUseFroxel);
        } else {
            ImGui::TextDisabled("Vulkan: fullscreen raymarch (froxel is Metal-only for now)");
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Volume")) {
        ImGui::Checkbox("Enabled", &volumeRenderEnabled);
        ImGui::TextDisabled(volumeDensityTexture.id == volumeTestTexture.id
                                ? "density: built-in test grid (64^3)"
                                : "density: external grid");
        ImGui::DragFloat3("Box Min", &volumeSettings.boxMin.x, 0.1f, -100.0f, 100.0f);
        ImGui::DragFloat3("Box Max", &volumeSettings.boxMax.x, 0.1f, -100.0f, 100.0f);
        ImGui::DragFloat("Density Scale", &volumeSettings.boxMin.w, 0.1f, 0.0f, 100.0f);
        ImGui::DragFloat("Anisotropy", &volumeSettings.boxMax.w, 0.01f, -0.99f, 0.99f);
        ImGui::ColorEdit3("Albedo", &volumeSettings.albedo.x);
        ImGui::DragFloat("Ambient Intensity", &volumeSettings.albedo.w, 0.01f, 0.0f, 2.0f);
        int volSteps = static_cast<int>(volumeSettings.params.x);
        if (ImGui::SliderInt("Steps", &volSteps, 16, 128)) volumeSettings.params.x = float(volSteps);
        int volShadowSteps = static_cast<int>(volumeSettings.params.y);
        if (ImGui::SliderInt("Self-shadow steps", &volShadowSteps, 0, 16)) volumeSettings.params.y = float(volShadowSteps);
        if (ImGui::Button("Reset to Defaults")) {
            bool wasEnabled = volumeRenderEnabled;
            volumeSettings = VolumeRenderData{};
            volumeRenderEnabled = wasEnabled;
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Volumetric Clouds")) {
        ImGui::Checkbox("Enabled", &volumetricCloudsEnabled);
        if (ImGui::DragFloat("Bottom (m)", &cloudSettings.cloudLayerBottom, 100.0f, 0.0f, 10000.0f) |
            ImGui::DragFloat("Top (m)", &cloudSettings.cloudLayerTop, 100.0f, 0.0f, 15000.0f)) {
            cloudSettings.cloudLayerThickness =
                std::max(1.0f, cloudSettings.cloudLayerTop - cloudSettings.cloudLayerBottom);
        }
        ImGui::DragFloat("Coverage", &cloudSettings.cloudCoverage, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Density", &cloudSettings.cloudDensity, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Type (Stratus-Cumulus)", &cloudSettings.cloudType, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Ambient", &cloudSettings.ambientIntensity, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Silver Lining", &cloudSettings.silverLiningIntensity, 0.01f, 0.0f, 2.0f);
        // RHI-only extras (kept from the original Effects panel)
        ImGui::SliderFloat("Temporal blend", &cloudSettings.temporalBlend, 0.01f, 1.0f);
        int steps = static_cast<int>(cloudSettings.primarySteps);
        if (ImGui::SliderInt("Primary steps", &steps, 8, 128))
            cloudSettings.primarySteps = static_cast<Uint32>(steps);
        ImGui::TreePop();
    }

    // The RHI sun flare is a clean-room redesign with a reduced knob set;
    // the native controls that map onto existing fields are exposed here.
    if (ImGui::TreeNode("Sun Flare (Lens Flare)")) {
        ImGui::Checkbox("Enabled", &sunFlareEnabled);
        ImGui::DragFloat("Intensity", &sunFlareSettings.intensity, 0.01f, 0.0f, 4.0f);
        ImGui::ColorEdit3("Sun Color", &sunFlareSettings.sunColor.x);
        ImGui::DragFloat("Glow Size", &sunFlareSettings.glowSize, 0.005f, 0.0f, 2.0f);
        ImGui::DragFloat("Halo Radius", &sunFlareSettings.haloRadius, 0.005f, 0.0f, 1.0f);
        ImGui::DragFloat("Ghost Spacing", &sunFlareSettings.ghostSpacing, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat("Streak Intensity", &sunFlareSettings.streakIntensity, 0.01f, 0.0f, 1.0f);
        ImGui::TreePop();
    }

    // Particle system controls (Visible/Pause/Emit) live in the game's Particles
    // window — the renderer only exposes them as setters (pure mechanism), since
    // Pause and Emit must also drive the CPU-side ECS timers.

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

    // Aggregate passes that share a name into one row (with an occurrence count).
    // Several passes fan out over multiple invocations — the Hi-Z build issues
    // one "HiZBuild" pass per mip, bloom one "BloomDownsample" per level — and a
    // row per invocation buries the useful per-effect total. Insertion order is
    // preserved so the table still reads top-to-bottom in frame order.
    struct Agg { std::string name; double ms; int count; };
    std::vector<Agg> agg;
    for (const auto& t : timings) {
        auto it = std::find_if(agg.begin(), agg.end(),
                               [&](const Agg& a) { return a.name == t.name; });
        if (it == agg.end()) agg.push_back({ t.name, t.gpuTimeMs, 1 });
        else { it->ms += t.gpuTimeMs; it->count++; }
    }

    double totalMs = 0.0;
    double maxMs = 0.001;
    for (const auto& a : agg) {
        totalMs += a.ms;
        maxMs = std::max(maxMs, a.ms);
    }
    // Up to three numbers, each a different thing on a pipelined GPU:
    //   busy = interval union of pass windows -> occupancy; compare THIS to the
    //          frame period (~equal = GPU-bound). Reported by backends that
    //          sample per-pass windows (Vulkan; Metal if StartOfFragment works).
    //   span = min sample .. max sample -> per-frame LATENCY; pipelined frames
    //          overlap, so span > frame period at steady state is healthy.
    //   sum  = per-pass total. Additive == span in Metal's chained scheme
    //          (busy==0); double-counts overlap in the window scheme (busy>0).
    double spanMs = rhi->getGpuFrameSpanMs();
    double busyMs = rhi->getGpuFrameBusyMs();
    if (spanMs > 0.0) {
        if (busyMs > 0.0) {
            ImGui::Text("GPU busy: %.3f ms", busyMs);
            ImGui::SameLine();
            ImGui::TextDisabled("(occupancy; ~frame period when GPU-bound)");
        }
        ImGui::Text("GPU frame latency (span): %.3f ms", spanMs);
        ImGui::SameLine();
        ImGui::TextDisabled("(overlaps adjacent frames)");
        if (busyMs > 0.0)
            ImGui::TextDisabled("Sum of pass windows: %.3f ms (overlap double-counts)", totalMs);
        else
            ImGui::TextDisabled("Sum of passes: %.3f ms (= span)", totalMs);
    } else {
        ImGui::Text("Total GPU: %.3f ms", totalMs);
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##gpu_pass_timings", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& a : agg) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (a.count > 1) ImGui::Text("%s (x%d)", a.name.c_str(), a.count);
            else ImGui::TextUnformatted(a.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", a.ms);
            ImGui::TableSetColumnIndex(2);
            ImGui::ProgressBar(static_cast<float>(a.ms / maxMs), ImVec2(-1.0f, 0.0f), "");
        }
        ImGui::EndTable();
    }
}

// Per-pass CPU (command-recording) time, mirroring drawGpuTimingsImGui. This is
// how long the CPU spends building each pass on the (single) render thread — the
// number that decides whether multithreaded command recording would help.
void Renderer::drawCpuTimingsImGui() {
    if (!ImGui::CollapsingHeader("CPU Pass Timings"))
        return;

    const auto& cpuTimings = renderGraph.getPassCpuTimings();
    double graphMs = 0.0;
    double maxMs = 0.001;
    for (const auto& t : cpuTimings) {
        graphMs += t.second;
        maxMs = std::max(maxMs, t.second);
    }
    const double cpuTotalMs = m_cpuPreGraphMs + graphMs;

    ImGui::Text("CPU frame (submit): %.3f ms", cpuTotalMs);
    ImGui::Text("  cull + sort + upload: %.3f ms", m_cpuPreGraphMs);
    ImGui::Text("  render graph:        %.3f ms", graphMs);

    // Bottleneck readout: compare against total GPU time when it's available.
    if (rhi->isGpuTimingSupported() && rhi->isGpuTimingEnabled()) {
        double gpuTotalMs = 0.0;
        for (const auto& t : rhi->getGpuPassTimings()) gpuTotalMs += t.gpuTimeMs;
        const char* verdict = "roughly balanced";
        if (gpuTotalMs > cpuTotalMs * 1.3) verdict = "GPU-bound";
        else if (cpuTotalMs > gpuTotalMs * 1.3) verdict = "CPU-bound";
        ImGui::Separator();
        ImGui::Text("GPU total: %.3f ms  |  CPU total: %.3f ms", gpuTotalMs, cpuTotalMs);
        ImGui::Text("=> %s", verdict);
        ImGui::TextDisabled("Multithreaded command recording only helps when CPU-bound.");
    } else {
        ImGui::TextDisabled("Enable GPU Pass Timings to see the CPU-vs-GPU verdict.");
    }

    ImGui::Separator();
    if (ImGui::BeginTable("##cpu_pass_timings", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& t : cpuTimings) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(t.first.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", t.second);
            ImGui::TableSetColumnIndex(2);
            ImGui::ProgressBar(static_cast<float>(t.second / maxMs), ImVec2(-1.0f, 0.0f), "");
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
        // 2D canvas coordinates are LOGICAL window units, like the native
        // Metal CanvasPass (SDL_GetWindowSize). On high-DPI displays the
        // swapchain is larger than the window; projecting with the swapchain
        // extent would shrink and shift every sprite by the DPI scale.
        int w = 0, h = 0;
        if (window) {
            SDL_GetWindowSize(window, &w, &h);
        }
        if (w <= 0 || h <= 0) {
            w = static_cast<int>(rhi->getSwapchainWidth());
            h = static_cast<int>(rhi->getSwapchainHeight());
        }
        glm::mat4 viewProj = glm::orthoZO(
            0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, -1.0f, 1.0f
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
    batch3D.setTexture(texture);
    if (texCoords) {
        // Corner order: BL, BR, TR, TL (quad local -0.5..+0.5, +Y up).
        batch3D.addQuad(transform, texCoords, tintColor, entityID);
    } else {
        batch3D.addQuad(transform, tintColor, entityID);
    }
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
    // `position` is the TOP-LEFT corner (native Metal renderer convention —
    // the centered variant here rendered rects offset up-left by size/2).
    glm::vec2 topLeft = position;
    glm::vec2 topRight = position + glm::vec2(size.x, 0.0f);
    glm::vec2 bottomRight = position + size;
    glm::vec2 bottomLeft = position + glm::vec2(0.0f, size.y);

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
    batch2D.addTriangle(p0, p1, p2, color);
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

    // Draw each character as a textured quad, all in the atlas segment (the
    // segment texture is what flush() samples — without this the glyphs land
    // in the previous segment and render as solid blocks).
    batch2D.setTexture(fontTexture);

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

        // Glyph quad position: yOffset is relative to the baseline, which sits
        // ascent below the caller's top-of-line position (matches the native
        // Metal renderer).
        float xPos = cursorX + glyph->xOffset * scale;
        float yPos = cursorY + (glyph->yOffset + fontData->ascent) * scale;
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

    batch2D.setTexture(TextureHandle{});
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

    // Draw each character as a billboard, all in the atlas texture segment.
    batch3D.setTexture(fontTexture);

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

    batch3D.setTexture(TextureHandle{});
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

    // Create color texture. Always RGBA16F: renderToTexture() draws with the
    // main/batch pipelines, whose color format is baked as RGBA16F — a
    // different RTT format is a Vulkan validation error (dynamic-rendering
    // pipelines must match their attachment formats exactly). HDR also means
    // isHDR render textures need no special casing, and sampling is identical.
    TextureDesc colorDesc;
    colorDesc.width = desc.width;
    colorDesc.height = desc.height;
    colorDesc.format = PixelFormat::RGBA16_FLOAT;
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
    std::shared_ptr<RenderScene> scene,
    Camera& camera,
    const glm::vec4& clearColor
) {
    if (target.id >= renderTextures.size()) {
        return; // Invalid handle
    }

    auto& resource = renderTextures[target.id];

    // The PBR pipeline bakes RGBA16F color + Depth32F depth attachments into
    // the PSO, so a depth-less render texture can't bind it.
    if (!mainPipeline.isValid() || !resource.colorTexture.isValid() ||
        !resource.hasDepth || !resource.depthTexture.isValid() ||
        !rttCameraBuffer.isValid() || !rttInstanceBuffer.isValid()) {
        return;
    }

    // Save current camera state (collectDrawables/performCulling read it)
    CameraRenderData previousCamera = currentCamera;

    // Set up camera for this render
    CameraRenderData rtCamera{};
    rtCamera.proj = camera.getProjMatrix();
    rtCamera.view = camera.getViewMatrix();
    rtCamera.invProj = glm::inverse(rtCamera.proj);
    rtCamera.invView = glm::inverse(rtCamera.view);
    rtCamera.nearPlane = camera.near();
    rtCamera.farPlane = camera.far();
    rtCamera.position = camera.getEye();
    currentCamera = rtCamera;

    // Cull for THIS view and build ITS instance array into the dedicated RTT
    // buffers. frameDrawables/visibleDrawables are per-draw scratch (cleared
    // again below so the main draw's APPENDING collect starts from empty).
    // Collection goes straight through the ECS registry the game last drew
    // with — the scene-node tree is being retired and collects nothing.
    frameDrawables.clear();
    visibleDrawables.clear();
    if (lastDrawRegistry) {
        collectDrawables(*lastDrawRegistry, scene);
    }
    performCulling();
    sortDrawables();

    std::vector<Vapor::InstanceData> rttInstances;
    rttInstances.reserve(std::min<size_t>(visibleDrawables.size(), MAX_INSTANCES));
    std::vector<Uint32> rttDraws;  // drawable index per instance slot
    rttDraws.reserve(rttInstances.capacity());
    for (Uint32 drawableIdx : visibleDrawables) {
        if (rttInstances.size() >= MAX_INSTANCES) break;
        const Drawable& drawable = frameDrawables[drawableIdx];
        if (drawable.mesh >= meshes.size()) continue;
        const RenderMesh& mesh = meshes[drawable.mesh];
        if (!mesh.vertexBuffer.isValid()) continue;
        Vapor::InstanceData instance;
        instance.model = drawable.transform;
        instance.color = drawable.color;
        instance.vertexOffset = 0;
        instance.indexOffset = 0;
        instance.rtVertexOffset = mesh.vertexOffset;  // always valid: RT hit shading
        instance.rtIndexOffset = mesh.indexOffset;    //   fetches merged geometry
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
        rttInstances.push_back(instance);
        rttDraws.push_back(drawableIdx);
    }

    rhi->updateBuffer(rttCameraBuffer, &rtCamera, 0, sizeof(CameraRenderData));
    if (!rttInstances.empty()) {
        rhi->updateBuffer(rttInstanceBuffer, rttInstances.data(), 0,
                          rttInstances.size() * sizeof(Vapor::InstanceData));
    }

    // Begin render pass with render texture as target
    RenderPassDesc passDesc;
    passDesc.name = "RenderToTexture";
    passDesc.colorAttachments.push_back(resource.colorTexture);
    passDesc.clearColors.push_back(clearColor);
    passDesc.loadColor.push_back(false); // Clear, don't load
    passDesc.depthAttachment = resource.depthTexture;
    passDesc.clearDepth = 1.0f;
    passDesc.loadDepth = false; // Clear depth
    rhi->beginRenderPass(passDesc);

    if (!rttInstances.empty()) {
        rhi->bindPipeline(mainPipeline);

        // Full main-pass binding contract (mirrors mainRenderPass), with the
        // RTT camera/instance buffers and every screen-space input neutralized
        // — shadow/AO/SSCS/point-shadow whites, GIBS/reflection/refraction off.
        // Those buffers hold the MAIN view; sampling them here would paste the
        // main view's screen-space data over this view.
        rhi->setVertexBuffer(0, rttCameraBuffer, 0, sizeof(CameraRenderData));
        rhi->setVertexBuffer(1, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
        rhi->setVertexBuffer(2, rttInstanceBuffer, 0, sizeof(Vapor::InstanceData) * rttInstances.size());

        rhi->setFragmentBuffer(0, directionalLightBuffer, 0, sizeof(DirectionalLightData) * maxDirectionalLights);
        rhi->setFragmentBuffer(1, pointLightBuffer, 0, sizeof(PointLightData) * maxPointLights);
        rhi->setFragmentBuffer(2, clusterBuffer);
        rhi->setFragmentBuffer(3, rttCameraBuffer, 0, sizeof(CameraRenderData));
        // Materials for the Metal PBR fragment's per-fragment fetch (buffer 11);
        // no-ops on Vulkan. See the main-pass note.
        rhi->setFragmentBuffer(19, materialUniformBuffer, 0, sizeof(Vapor::MaterialData) * MAX_INSTANCES);
        glm::vec2 rttScreenSize(static_cast<float>(resource.width), static_cast<float>(resource.height));
        rhi->setFragmentBytes(&rttScreenSize, sizeof(glm::vec2), 4);
        glm::uvec3 gridSize(clusterGridSizeX, clusterGridSizeY, clusterGridSizeZ);
        rhi->setFragmentBytes(&gridSize, sizeof(glm::uvec3), 5);
        float time = 0.0f;
        rhi->setFragmentBytes(&time, sizeof(float), 6);
        rhi->setFragmentBuffer(7, rectLightBuffer);
        // Rect/spot loops off: their world data would be fine from any view,
        // but this frame's CPU light lists may not be submitted yet (the demo
        // calls renderToTexture before draw()), so the counts aren't reliable.
        Uint32 rttRectCount = 0;
        rhi->setFragmentBytes(&rttRectCount, sizeof(Uint32), 8);
        rhi->setFragmentBuffer(9, pssmDataBuffer, 0, sizeof(PSSMRenderData));
        Uint32 rttGibsOn = 0;  // GI gather is a main-view screen-space texture
        rhi->setFragmentBytes(&rttGibsOn, sizeof(Uint32), 10);
        // Directional count for RHIMain.frag's loop: from the scene directly
        // (the renderer's per-frame list is empty until the main draw submits).
        Uint32 rttDirCount = scene ? static_cast<Uint32>(scene->directionalLights.size()) : 0u;
        rhi->setFragmentBytes(&rttDirCount, sizeof(Uint32), 11);

        // Neutral texture defaults (same table as mainRenderPass).
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

        // Skip the tile-culled point-light loop (bit0): the cluster buffer is
        // built for the MAIN camera's screen tiles, meaningless in this view.
        Uint32 rttDebugFlags = mainDebugFlags | 1u;

        if (backend == GraphicsBackend::Vulkan) {
            // RHIMain.frag contract (see mainRenderPass for the slot notes).
            if (pssmDataBuffer.isValid()) rhi->setFragmentBuffer(2, pssmDataBuffer, 0, sizeof(PSSMRenderData));
            if (pssmShadowArrayTexture.isValid()) rhi->setTexture(0, 6, pssmShadowArrayTexture, shadowSampler);
            if (clusterBuffer.isValid()) rhi->setFragmentBuffer(4, clusterBuffer);
            if (lightCullDataBuffer.isValid()) rhi->setFragmentBuffer(5, lightCullDataBuffer, 0, sizeof(Vapor::LightCullData));
            rhi->setTexture(0, 7, whiteTex, clampSampler);   // AO neutral
            rhi->setTexture(0, 8, whiteTex, clampSampler);   // SSCS neutral
            // Near-field shadow map is light-space (world POV) — valid here.
            rhi->setTexture(0, 9, nearShadowMap.isValid() ? nearShadowMap : whiteTex, shadowSampler);
            if (irradianceMap.isValid()) rhi->setTexture(0, 10, irradianceMap, clampSampler);
            if (prefilterMap.isValid()) rhi->setTexture(0, 11, prefilterMap, clampSampler);
            if (brdfLUTTex.isValid())    rhi->setTexture(0, 12, brdfLUTTex, clampSampler);
            rhi->setFragmentBytes(&rttDebugFlags, sizeof(Uint32), 2);
            // Spot/rect buffers must be bound (declared in RHIMain.frag); zero
            // counts keep both loops off — their per-frame data belongs to the
            // main view and may not be submitted yet.
            if (spotLightBuffer.isValid())
                rhi->setFragmentBuffer(6, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
            if (rectLightBuffer.isValid()) rhi->setFragmentBuffer(7, rectLightBuffer);
            glm::uvec2 rttSpotRectCounts(0u, 0u);
            rhi->setFragmentBytes(&rttSpotRectCounts, sizeof(glm::uvec2), 1);
        } else {
            rhi->setFragmentBytes(&rttDebugFlags, sizeof(Uint32), 12);
            // Real IBL cubes: image-based ambience is view-independent.
            if (m_iblReady) {
                if (irradianceMap.isValid()) rhi->setTexture(0, 8, irradianceMap, clampSampler);
                if (prefilterMap.isValid()) rhi->setTexture(0, 9, prefilterMap, clampSampler);
                if (brdfLUTTex.isValid()) rhi->setTexture(0, 10, brdfLUTTex, clampSampler);
            }
            // texSSCS(15) is min()'d unconditionally — MUST be bound; white = lit.
            rhi->setTexture(0, 15, whiteTex, clampSampler);
            // Spot buffer (16) + counts/flags (15) are declared in the shared
            // PBR shader — bind placeholders with zero counts/flags. (16, not 14:
            // buffer 14 is the bindless SystemTexs slot — see the shader note.)
            rhi->setFragmentBuffer(16, spotLightBuffer, 0, sizeof(Vapor::SpotLight) * maxSpotLights);
            glm::uvec2 rttSpotRectParams(0u, 0u);
            rhi->setFragmentBytes(&rttSpotRectParams, sizeof(glm::uvec2), 15);
            // RT reflection/refraction composite params (buffer 17/18) are plain
            // args on the shared PBR shader — bind disabled (x=0) so the
            // composite is skipped; the RT screen-space inputs are the main
            // view's anyway and would smear across this off-screen view.
            glm::vec2 rttReflParams(0.0f, 0.0f);
            rhi->setFragmentBytes(&rttReflParams, sizeof(glm::vec2), 17);
            glm::vec2 rttRefrParams(0.0f, 0.0f);
            rhi->setFragmentBytes(&rttRefrParams, sizeof(glm::vec2), 18);
        }

        // Draw: instance IDs are sequential in rttInstances order.
        for (Uint32 iid = 0; iid < rttDraws.size(); iid++) {
            const Drawable& drawable = frameDrawables[rttDraws[iid]];
            const RenderMesh& mesh = meshes[drawable.mesh];
            if (drawable.material < materials.size()) {
                bindMaterial(drawable.material);
            }
            rhi->bindVertexBuffer(mesh.vertexBuffer, 3, 0);
            rhi->setVertexBytes(&iid, sizeof(Uint32), 4);
            if (mesh.indexBuffer.isValid()) {
                rhi->bindIndexBuffer(mesh.indexBuffer, 0);
                rhi->drawIndexed(mesh.indexCount, 1, 0, 0, 0);
            } else {
                rhi->draw(mesh.vertexCount, 1, 0, 0);
            }
        }
    }

    // Deliberately NOT flushing the global 2D/3D batches here: they hold quads
    // queued for the MAIN view (HUD sprites, world canvas). Flushing them into
    // this offscreen target both stole them from the main pass (the "Vulkan has
    // the render texture but no UI, Metal has UI but no render texture"
    // asymmetry) and drew screen-space HUD into a world-space TV, which is
    // semantically wrong. The render texture shows the scene from its own
    // camera via the mesh loop above; if batch content inside an RTT is ever
    // wanted, it needs an explicit scoped API, not the shared queues.

    rhi->endRenderPass();

    // Leave the shared per-frame lists EMPTY: beginFrame owns the clear and the
    // main draw's ECS collect APPENDS — leftovers here would double every
    // drawable in the main view.
    frameDrawables.clear();
    visibleDrawables.clear();

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
    if (!img) {
        fmt::print(stderr, "createTexture: null image — returning invalid handle\n");
        return {};
    }
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
    rhiBackend = backend;

    // Create vertex buffers — one per frame slot (data is rewritten every
    // frame; a single buffer would race the in-flight frames' draws).
    BufferDesc vbDesc;
    vbDesc.size = sizeof(Vertex2D) * MaxVertices;
    vbDesc.usage = BufferUsage::Vertex;
    vbDesc.memoryUsage = MemoryUsage::CPUtoGPU;
    for (uint32_t i = 0; i < kSlots; i++) {
        vertexBufferSlots[i] = rhi->createBuffer(vbDesc);
    }
    vertexBuffer = vertexBufferSlots[0];

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
    // Batches flush inside the main pass (HDR colorRT) and renderToTexture
    // (also RGBA16F) — bake the matching format, not the swapchain default
    // (mismatch = VUID-...-dynamicRenderingUnusedAttachments-08910 per draw).
    pipelineDesc.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
    pipelineDesc.hasDepthAttachment = true;
    pipelineDesc.depthAttachmentFormat = PixelFormat::Depth32Float;

    pipeline = rhi->createPipeline(pipelineDesc);

    fmt::print("BatchRenderer initialized ({} mode)\n", is3D ? "3D" : "2D");
}

void Renderer::BatchRenderer::nextFrame() {
    slotIndex = (slotIndex + 1) % kSlots;
    vertexBuffer = vertexBufferSlots[slotIndex];
}

void Renderer::BatchRenderer::shutdown(RHI* rhi) {
    for (uint32_t i = 0; i < kSlots; i++) {
        if (vertexBufferSlots[i].isValid()) {
            rhi->destroyBuffer(vertexBufferSlots[i]);
            vertexBufferSlots[i] = {};
        }
    }
    vertexBuffer = {};
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

void Renderer::BatchRenderer::flush(RHI* rhi, const glm::mat4& viewProj, PipelineHandle overridePipeline) {
    if (quadCount == 0) return;

    // Upload vertex data
    rhi->updateBuffer(vertexBuffer, vertices.data(), 0, sizeof(Vertex2D) * vertices.size());

    // Bind pipeline (override = the swapchain/UI variant)
    rhi->bindPipeline(overridePipeline.isValid() ? overridePipeline : pipeline);

    // View-projection: 2d_batch.metal declares vertices at buffer(0) and the
    // uniforms at buffer(1) — sending the matrix to index 0 on Metal left
    // buffer(1) UNBOUND (the vertex-buffer bind below overwrote index 0), so
    // every batch draw read an unbound buffer: repeated GPU faults until the
    // OS ignored the whole queue. Vulkan's contract is push constants [0,64)
    // + vertex input binding 0 — separate namespaces, so index 0 for both.
    if (rhiBackend == GraphicsBackend::Metal) {
        rhi->setVertexBytes(&viewProj, sizeof(glm::mat4), 1);
    } else {
        rhi->setVertexBytes(&viewProj, sizeof(glm::mat4), 0);
    }

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

void Renderer::BatchRenderer::addTriangle(
    const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2,
    const glm::vec4& color, int entityID
) {
    if (quadCount >= MaxQuads) {
        if (canAutoFlush && currentRHI) {
            flush(currentRHI, currentViewProj);
        } else {
            return;
        }
    }

    // Shares the quad index pattern (0,1,2, 2,3,0): duplicating p2 into the
    // fourth vertex makes the second triangle zero-area, so exactly the
    // requested triangle is rasterized.
    const glm::vec2 corners[4] = { p0, p1, p2, p2 };
    for (int i = 0; i < 4; i++) {
        Vertex2D v;
        v.position = glm::vec3(corners[i], 0.0f);
        v.color = color;
        v.texCoord = glm::vec2(0.0f, 0.0f);
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
