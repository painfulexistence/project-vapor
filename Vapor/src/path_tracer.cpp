#include "path_tracer.hpp"
#include "helper.hpp"
#include <fmt/core.h>

namespace Vapor {

namespace {

// Mirrors PathTraceParams in 3d_path_trace.metal. All-scalar and 4-byte
// aligned, so it rides setComputeBytes without padding surprises.
struct PathTraceParamsGPU {
    Uint32 sampleOffset;
    Uint32 samplesPerFrame;
    Uint32 maxBounces;
    Uint32 dirLightCount;
    float sunAngularRadius;
    float rayBias;
    float rayMaxDistance;
    float envIntensity;
    float fireflyClamp;
    Uint32 hasBindlessGeo;
    Uint32 resetAccumulation;
    Uint32 pointLightCount;
    Uint32 spotLightCount;
    Uint32 rectLightCount;
    Uint32 _pad0;
    Uint32 _pad1;
};
static_assert(sizeof(PathTraceParamsGPU) == 64, "PathTraceParams layout must match the MSL struct");

constexpr Uint32 kThreadGroupSize = 8;

} // namespace

void PathTracer::init(RHI* rhi) {
    m_rhi = rhi;
    if (!m_rhi) return;

    const std::string code = readFile("shaders/3d_path_trace.metal");
    if (code.empty()) {
        fmt::print(stderr, "Path tracer: shaders/3d_path_trace.metal not found — photo mode unavailable\n");
        return;
    }

    auto makeKernel = [&](const char* entry, ShaderHandle& shader) -> ComputePipelineHandle {
        ShaderDesc sd;
        sd.stage = ShaderStage::Compute;
        sd.code = code.data();
        sd.codeSize = code.size();
        sd.entryPoint = entry;
        shader = m_rhi->createShader(sd);
        ComputePipelineDesc cd;
        cd.computeShader = shader;
        cd.threadGroupSizeX = kThreadGroupSize;
        cd.threadGroupSizeY = kThreadGroupSize;
        cd.threadGroupSizeZ = 1;
        return m_rhi->createComputePipeline(cd);
    };

    // A compile failure here must not take down renderer init: photo mode is
    // optional, and the game frame does not depend on any of this.
    try {
        m_accumulatePipeline = makeKernel("pathTraceAccumulate", m_accumulateShader);
        m_resolvePipeline = makeKernel("pathTraceResolve", m_resolveShader);
    } catch (const std::exception& e) {
        m_accumulatePipeline = {};
        m_resolvePipeline = {};
        fmt::print(stderr, "Path tracer kernels unavailable ({}) — photo mode disabled\n", e.what());
    }
}

void PathTracer::shutdown() {
    destroyTargets();
    if (!m_rhi) return;
    if (m_accumulatePipeline.isValid()) m_rhi->destroyComputePipeline(m_accumulatePipeline);
    if (m_resolvePipeline.isValid()) m_rhi->destroyComputePipeline(m_resolvePipeline);
    if (m_accumulateShader.isValid()) m_rhi->destroyShader(m_accumulateShader);
    if (m_resolveShader.isValid()) m_rhi->destroyShader(m_resolveShader);
    m_accumulatePipeline = {};
    m_resolvePipeline = {};
    m_accumulateShader = {};
    m_resolveShader = {};
    m_rhi = nullptr;
}

void PathTracer::createTargets(Uint32 width, Uint32 height) {
    if (!m_rhi || width == 0 || height == 0) return;
    if (m_accumTexture.isValid() && width == m_width && height == m_height) return;

    destroyTargets();

    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    // RGBA32F: rgb is a running SUM of radiance over every sample, not an
    // average. At a few thousand samples a 16-bit mantissa stops resolving the
    // newest contribution and the image quietly stops converging.
    desc.format = PixelFormat::RGBA32_FLOAT;
    desc.usage = TextureUsage::Storage | TextureUsage::Sampled;
    desc.mipLevels = 1;
    m_accumTexture = m_rhi->createTexture(desc);

    m_width = width;
    m_height = height;
    // The new texture holds uninitialized memory, so nothing may be added to it.
    m_accumulator.invalidate();
}

void PathTracer::destroyTargets() {
    if (m_rhi && m_accumTexture.isValid()) {
        m_rhi->destroyTexture(m_accumTexture);
    }
    m_accumTexture = {};
    m_width = 0;
    m_height = 0;
    m_accumulator.invalidate();
}

void PathTracer::trace(const PathTraceSceneView& view) {
    if (!isAvailable() || !view.isTraceable()) return;

    const Uint32 spp = m_accumulator.samplesToTrace(settings.samplesPerFrame, settings.maxSamples);
    if (spp == 0) return;  // converged: leave the finished image alone

    PathTraceParamsGPU params{};
    params.sampleOffset = m_accumulator.sampleCount();
    params.samplesPerFrame = spp;
    params.maxBounces = settings.maxBounces;
    params.dirLightCount = view.directionalLightCount;
    params.sunAngularRadius = settings.sunAngularRadius;
    params.rayBias = settings.rayBias;
    params.rayMaxDistance = settings.rayMaxDistance;
    params.envIntensity = settings.envIntensity;
    params.fireflyClamp = settings.fireflyClamp;
    params.hasBindlessGeo = view.hasGeometry() ? 1u : 0u;
    params.resetAccumulation = m_accumulator.resetPending() ? 1u : 0u;
    params.pointLightCount = view.pointLightCount;
    params.spotLightCount = view.spotLightCount;
    params.rectLightCount = view.rectLightCount;

    m_rhi->beginComputePass("PathTrace");
    m_rhi->bindComputePipeline(m_accumulatePipeline);
    m_rhi->setComputeTexture(0, m_accumTexture);
    m_rhi->setComputeTexture(1, view.environment);
    m_rhi->setComputeBuffer(0, view.camera, 0, sizeof(CameraRenderData));
    m_rhi->setAccelerationStructure(1, view.tlas);
    m_rhi->setComputeBytes(&params, sizeof(params), 2);
    m_rhi->setComputeBuffer(3, view.instances, 0, view.instanceRange);
    m_rhi->setComputeBuffer(4, view.materials, 0, view.materialRange);
    m_rhi->setComputeBuffer(5, view.directionalLights, 0, view.directionalLightRange);
    if (view.hasGeometry()) {
        m_rhi->setComputeBuffer(6, view.mergedVertices);
        m_rhi->setComputeBuffer(7, view.mergedIndices);
        m_rhi->bindComputeTextureArgumentTable(view.materialTextureTable, 8);
    }
    m_rhi->setComputeBuffer(9, view.pointLights, 0, view.pointLightRange);
    m_rhi->setComputeBuffer(10, view.spotLights, 0, view.spotLightRange);
    m_rhi->setComputeBuffer(11, view.rectLights, 0, view.rectLightRange);
    m_rhi->dispatch((m_width + kThreadGroupSize - 1) / kThreadGroupSize,
                    (m_height + kThreadGroupSize - 1) / kThreadGroupSize, 1);
    m_rhi->endComputePass();
    m_rhi->computeBarrier();  // accumulator writes -> resolve reads

    m_accumulator.recordTraced(spp);
}

void PathTracer::resolve(TextureHandle target) {
    if (!m_resolvePipeline.isValid() || !m_accumTexture.isValid() || !target.isValid()) return;
    // Nothing has been traced yet — resolving would divide by a zero sample
    // count and blank the target the raster frame just drew.
    if (!m_accumulator.hasImage()) return;

    m_rhi->beginComputePass("PathTraceResolve");
    m_rhi->bindComputePipeline(m_resolvePipeline);
    m_rhi->setComputeTexture(0, m_accumTexture);
    m_rhi->setComputeTexture(1, target);
    m_rhi->dispatch((m_width + kThreadGroupSize - 1) / kThreadGroupSize,
                    (m_height + kThreadGroupSize - 1) / kThreadGroupSize, 1);
    m_rhi->endComputePass();
    m_rhi->computeBarrier();  // resolve writes -> post-process reads
}

} // namespace Vapor
