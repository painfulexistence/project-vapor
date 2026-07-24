#include "Vapor/tessellation.hpp"
#include "Vapor/cbt.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/helper.hpp"  // readFile
#include "Vapor/renderer.hpp"
#include <algorithm>
#include <bit>
#include <fmt/core.h>

// ============================================================================
// Adaptive GPU tessellation (CBT/LEB) — runtime pipeline.
//
// Per frame and per tessellated mesh, the TessUpdate compute pass runs:
//   classify (indirect over last frame's leaf count; split/merge bit writes)
//   -> sum reduction (popcount + one halving dispatch per level)
//   -> args (leaf count -> next classify dispatch, leafPrep dispatch, draw
//      command, mesh task grid — all GPU-written, no CPU round trip)
//   -> leafPrep (indirect over the NEW leaf count; corner cache + frustum
//      visibility; skipped on the mesh path, whose object stage does both).
// The Tess render pass then draws every mesh through one of two routes:
//   mesh path     drawMeshTasksIndirect  (object stage decodes+culls+compacts)
//   compute path  drawIndexedIndirect    (one grid instance per leaf)
// Buffers are GPU-resident after creation — no per-frame CPU writes, no
// frames-in-flight slotting: successive frames' GPU work serializes on the
// queue and the CBT is the only persistent state.
// ============================================================================

namespace Vapor {

// ---- builders --------------------------------------------------------------

std::vector<TessRootGpu> buildTessRoots(const Mesh& mesh) {
    const auto topo = buildFanRoots(mesh.indices);
    std::vector<TessRootGpu> roots(topo.size());
    for (size_t i = 0; i < topo.size(); ++i) {
        const TessRootTopology& t = topo[i];
        // Face centroid corner (v1): average the face's three vertices.
        glm::vec3 cPos(0), cNrm(0);
        glm::vec2 cUv(0);
        for (int k = 0; k < 3; ++k) {
            const VertexData& v = mesh.vertices[mesh.indices[t.face * 3 + k]];
            cPos += glm::vec3(v.position);
            cNrm += glm::vec3(v.normal);
            cUv += glm::vec2(v.uv);
        }
        cPos /= 3.0f;
        cUv /= 3.0f;
        cNrm = (glm::dot(cNrm, cNrm) > 0.0f) ? glm::normalize(cNrm) : glm::vec3(0, 1, 0);

        TessRootGpu& r = roots[i];
        for (int c = 0; c < 3; ++c) {
            glm::vec3 pos, nrm;
            glm::vec2 uv;
            if (t.corner[c] == UINT32_MAX) {
                pos = cPos;
                nrm = cNrm;
                uv = cUv;
            } else {
                const VertexData& v = mesh.vertices[t.corner[c]];
                pos = glm::vec3(v.position);
                nrm = glm::vec3(v.normal);
                uv = glm::vec2(v.uv);
            }
            r.posU[c] = glm::vec4(pos, uv.x);
            r.nrmV[c] = glm::vec4(nrm, uv.y);
        }
        r.left = t.adjacency.left;
        r.right = t.adjacency.right;
        r.edge = t.adjacency.edge;
        r.node = t.adjacency.node;
    }
    return roots;
}

void buildTessGrid(std::vector<glm::vec2>& outVerts, std::vector<Uint32>& outIndices) {
    // Mirror of tessGridVertexRC / tessGridBarycentric / tessGridTriangle in
    // 3d_tess_lib.metal: verts store (w1, w2); triangles are wound CCW in
    // object space (the (c, r) parameter plane is mirrored).
    constexpr Uint32 S = kTessGridSegs;
    outVerts.clear();
    outIndices.clear();
    for (Uint32 r = 0; r <= S; ++r) {
        for (Uint32 c = 0; c <= S - r; ++c) {
            outVerts.push_back(glm::vec2(float(r) / float(S), float(c) / float(S)));
        }
    }
    const auto vid = [](Uint32 r, Uint32 c) { return r * (S + 1) - (r * (r - 1)) / 2 + c; };
    for (Uint32 r = 0; r < S; ++r) {
        for (Uint32 c = 0; c < S - r; ++c) {
            outIndices.insert(outIndices.end(), { vid(r, c), vid(r + 1, c), vid(r, c + 1) });
            if (c < S - r - 1) {  // downward triangles: one fewer per row
                outIndices.insert(outIndices.end(),
                                  { vid(r, c + 1), vid(r + 1, c), vid(r + 1, c + 1) });
            }
        }
    }
}

// ---- pipelines -------------------------------------------------------------

void Renderer::createTessellationPipelines() {
    // Compute chain (3d_tess_update.metal).
    std::string upd = readFile("shaders/3d_tess_update.metal");
    if (upd.empty()) return;
    ShaderDesc d;
    d.stage = ShaderStage::Compute;
    d.code = upd.data();
    d.codeSize = upd.size();
    const auto makeKernel = [&](const char* entry, ShaderHandle& sh) {
        d.entryPoint = entry;
        sh = rhi->createShader(d);
        ComputePipelineDesc cd;
        cd.computeShader = sh;
        cd.threadGroupSizeX = 64;
        return rhi->createComputePipeline(cd);
    };
    tessClassifyPipeline = makeKernel("tessClassify", tessClassifyShader);
    tessReduceFirstPipeline = makeKernel("tessReduceFirst", tessReduceFirstShader);
    tessReduceLevelPipeline = makeKernel("tessReduceLevel", tessReduceLevelShader);
    tessLeafPrepPipeline = makeKernel("tessLeafPrep", tessLeafPrepShader);
    {
        d.entryPoint = "tessPrepareArgs";
        tessArgsShader = rhi->createShader(d);
        ComputePipelineDesc cd;
        cd.computeShader = tessArgsShader;
        cd.threadGroupSizeX = 1;  // single-thread kernel
        tessArgsPipeline = rhi->createComputePipeline(cd);
    }

    // Instanced compute path (3d_tess_render.metal): vertex-pulls the grid,
    // so no vertex layout. Same render-state contract as the meshlet main
    // pass: HDR color + depth. Cull off for bring-up (displacement can turn
    // triangles anyway); the geometry is still emitted with consistent CCW
    // winding for when it's enabled.
    std::string rnd = readFile("shaders/3d_tess_render.metal");
    if (!rnd.empty()) {
        ShaderDesc v;
        v.stage = ShaderStage::Vertex;
        v.code = rnd.data();
        v.codeSize = rnd.size();
        v.entryPoint = "tessVertexMain";
        tessVertexShader = rhi->createShader(v);
        v.stage = ShaderStage::Fragment;
        v.entryPoint = "tessFragmentMain";
        tessFragmentShader = rhi->createShader(v);
        PipelineDesc p;
        p.vertexShader = tessVertexShader;
        p.fragmentShader = tessFragmentShader;
        p.depthTest = true;
        p.depthWrite = true;
        p.depthCompareOp = CompareOp::LessOrEqual;
        p.hasDepthAttachment = true;
        p.cullMode = CullMode::None;
        p.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
        tessRenderPipeline = rhi->createPipeline(p);
    }

    // Mesh/task path (3d_tess_mesh.metal), only where supported.
    if (capabilities.meshShaders) {
        std::string msh = readFile("shaders/3d_tess_mesh.metal");
        if (!msh.empty()) {
            ShaderDesc m;
            m.code = msh.data();
            m.codeSize = msh.size();
            m.stage = ShaderStage::Task;
            m.entryPoint = "tessObjectMain";
            tessObjectShader = rhi->createShader(m);
            m.stage = ShaderStage::Mesh;
            m.entryPoint = "tessMeshMain";
            tessMeshShader = rhi->createShader(m);
            m.stage = ShaderStage::Fragment;
            m.entryPoint = "tessMeshFragmentMain";
            tessMeshFragShader = rhi->createShader(m);
            MeshPipelineDesc mp;
            mp.taskShader = tessObjectShader;
            mp.meshShader = tessMeshShader;
            mp.fragmentShader = tessMeshFragShader;
            mp.taskThreadgroupSize = 32;
            mp.meshThreadgroupSize = 64;  // >= max(45 verts, 64 tris)
            mp.payloadBytes = sizeof(Uint32) * 32;  // TessPayload
            mp.maxMeshThreadgroupsPerObject = 32;
            mp.depthTest = true;
            mp.depthWrite = true;
            mp.depthCompareOp = CompareOp::LessOrEqual;
            mp.hasDepthAttachment = true;
            mp.cullMode = CullMode::None;
            mp.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
            tessMeshPipeline = rhi->createMeshPipeline(mp);
        }
    }

    ensureTessGridBuffers();
}

void Renderer::createTessellationPipelinesVulkan() {
    // GLSL twins of the MSL kernels (TessClassify/TessReduce*/TessPrepareArgs/
    // TessLeafPrep.comp + TessRender.vert/.frag), precompiled to SPIR-V by the
    // asset pipeline. Two deliberate contract differences from Metal, mirrored
    // in the shaders' binding comments: the CBT is one SSBO at b0 (no separate
    // atomic view at b1), and TessParams travel in a small per-instance buffer
    // (compute b4 / vertex b3) because Vulkan's 64-byte push range can't carry
    // the 112-byte struct that Metal passes as inline bytes. The mesh/task
    // path stays Metal-only; tessMeshPathActive() falls back to this instanced
    // route on its own.
    auto makeKernel = [&](const char* spv, ShaderHandle& sh, Uint32 groupX) -> ComputePipelineHandle {
        std::string code = readFile(spv);
        if (code.empty()) return {};
        ShaderDesc d;
        d.stage = ShaderStage::Compute;
        d.code = code.data();
        d.codeSize = code.size();
        d.entryPoint = "main";
        sh = rhi->createShader(d);
        ComputePipelineDesc cd;
        cd.computeShader = sh;
        cd.threadGroupSizeX = groupX;  // matches local_size_x in the .comp
        return rhi->createComputePipeline(cd);
    };
    tessClassifyPipeline = makeKernel("shaders/TessClassify.comp.spv", tessClassifyShader, 64);
    tessReduceFirstPipeline = makeKernel("shaders/TessReduceFirst.comp.spv", tessReduceFirstShader, 64);
    tessReduceLevelPipeline = makeKernel("shaders/TessReduceLevel.comp.spv", tessReduceLevelShader, 64);
    tessArgsPipeline = makeKernel("shaders/TessPrepareArgs.comp.spv", tessArgsShader, 1);
    tessLeafPrepPipeline = makeKernel("shaders/TessLeafPrep.comp.spv", tessLeafPrepShader, 64);

    std::string tv = readFile("shaders/TessRender.vert.spv");
    std::string tf = readFile("shaders/TessRender.frag.spv");
    if (!tv.empty() && !tf.empty()) {
        ShaderDesc vd;
        vd.stage = ShaderStage::Vertex;
        vd.code = tv.data();
        vd.codeSize = tv.size();
        vd.entryPoint = "main";
        tessVertexShader = rhi->createShader(vd);
        ShaderDesc fd;
        fd.stage = ShaderStage::Fragment;
        fd.code = tf.data();
        fd.codeSize = tf.size();
        fd.entryPoint = "main";
        tessFragmentShader = rhi->createShader(fd);

        // Same render-state contract as the Metal instanced pipeline (HDR
        // color + depth, cull off), with the Vulkan-required explicit formats
        // and the no-vertex-layout shape the grass/particle passes proved out.
        PipelineDesc p;
        p.vertexShader = tessVertexShader;
        p.fragmentShader = tessFragmentShader;
        p.vertexLayout.stride = 0;  // vertex-pulled, no vertex descriptor
        p.vertexLayout.attributes = {};
        p.topology = PrimitiveTopology::TriangleList;
        p.blendMode = BlendMode::Opaque;
        p.depthTest = true;
        p.depthWrite = true;
        p.depthCompareOp = CompareOp::LessOrEqual;
        p.cullMode = CullMode::None;
        p.sampleCount = 1;
        p.hasDepthAttachment = true;
        p.depthAttachmentFormat = PixelFormat::Depth32Float;
        p.colorAttachmentFormats = { PixelFormat::RGBA16_FLOAT };
        tessRenderPipeline = rhi->createPipeline(p);
    }

    tessParamsViaBuffer = true;
    ensureTessGridBuffers();
}

void Renderer::ensureTessGridBuffers() {
    if (tessGridVertexBuffer.isValid()) return;
    std::vector<glm::vec2> verts;
    std::vector<Uint32> indices;
    buildTessGrid(verts, indices);
    tessGridIndexCount = static_cast<Uint32>(indices.size());

    BufferDesc vb;
    vb.size = verts.size() * sizeof(glm::vec2);
    vb.usage = BufferUsage::Storage;  // vertex-pulled, not a vertex-descriptor input
    vb.memoryUsage = MemoryUsage::GPU;
    tessGridVertexBuffer = rhi->createBuffer(vb);
    rhi->updateBuffer(tessGridVertexBuffer, verts.data(), 0, vb.size);

    BufferDesc ib;
    ib.size = indices.size() * sizeof(Uint32);
    ib.usage = BufferUsage::Index;
    ib.memoryUsage = MemoryUsage::GPU;
    tessGridIndexBuffer = rhi->createBuffer(ib);
    rhi->updateBuffer(tessGridIndexBuffer, indices.data(), 0, ib.size);
    rhi->flushUploads();
}

// ---- instance lifecycle ----------------------------------------------------

Uint32 Renderer::createTessellatedMesh(const Mesh& mesh, const TessellationDesc& desc) {
    if (mesh.indices.empty() || mesh.vertices.empty()) return 0;
    if (!tessClassifyPipeline.isValid()) {
        fmt::print("[Tess] pipelines unavailable on this backend; createTessellatedMesh skipped\n");
        return 0;
    }
    ensureTessGridBuffers();

    std::vector<TessRootGpu> roots = buildTessRoots(mesh);
    const Uint32 rootCount = static_cast<Uint32>(roots.size());
    const Uint32 rootDepth =
        rootCount <= 1 ? 0 : static_cast<Uint32>(std::bit_width(rootCount - 1));
    const Uint32 maxDepth = std::clamp(desc.maxDepth, std::max(rootDepth + 2, 6u), 25u);
    if (rootDepth >= maxDepth) {
        fmt::print("[Tess] mesh has too many faces ({} roots) for maxDepth {}\n",
                   rootCount, maxDepth);
        return 0;
    }

    TessInstance t;
    t.id = m_nextTessMeshId++;
    t.maxDepth = maxDepth;
    t.rootDepth = rootDepth;
    t.rootCount = rootCount;
    t.maxLeaves = std::max(desc.maxLeaves, rootCount);
    t.displacementScale = desc.displacementScale;
    t.model = desc.model;
    t.terrainHeightfield = desc.terrainHeightfield;
    t.terrainFrequency = desc.terrainFrequency;
    t.terrainOctaves = desc.terrainOctaves;
    t.terrainSeed = desc.terrainSeed;

    // CBT storage, initialized on the CPU (roots marked + reduced) and
    // GPU-resident afterwards.
    CBT cbt(maxDepth, rootCount);
    BufferDesc cb;
    cb.size = cbt.raw().size() * sizeof(Uint32);
    cb.usage = BufferUsage::Storage;
    cb.memoryUsage = MemoryUsage::GPU;
    t.cbtBuffer = rhi->createBuffer(cb);
    rhi->updateBuffer(t.cbtBuffer, cbt.raw().data(), 0, cb.size);

    BufferDesc rb;
    rb.size = roots.size() * sizeof(TessRootGpu);
    rb.usage = BufferUsage::Storage;
    rb.memoryUsage = MemoryUsage::GPU;
    t.rootBuffer = rhi->createBuffer(rb);
    rhi->updateBuffer(t.rootBuffer, roots.data(), 0, rb.size);

    // Initial indirect args (frame 1's classify runs before the first
    // tessPrepareArgs): everything sized for the root-only tree.
    Uint32 args[kTessArgsSize / sizeof(Uint32)] = {};
    args[0] = (rootCount + 63) / 64;  // classify
    args[1] = 1;
    args[2] = 1;
    args[3] = (rootCount + 63) / 64;  // leafPrep
    args[4] = 1;
    args[5] = 1;
    args[6] = tessGridIndexCount;     // draw: indexCount
    args[7] = rootCount;              // draw: instanceCount
    args[11] = (rootCount + 31) / 32; // meshTasks
    args[12] = 1;
    args[13] = 1;
    args[14] = rootCount;             // leafCount
    BufferDesc ab;
    ab.size = kTessArgsSize;
    ab.usage = BufferUsage::Indirect;
    ab.memoryUsage = MemoryUsage::GPU;
    t.argsBuffer = rhi->createBuffer(ab);
    rhi->updateBuffer(t.argsBuffer, args, 0, ab.size);

    BufferDesc lb;
    lb.size = size_t(t.maxLeaves) * sizeof(TessLeafDataGpu);
    lb.usage = BufferUsage::Storage;
    lb.memoryUsage = MemoryUsage::GPU;
    t.leafDataBuffer = rhi->createBuffer(lb);

    // TessParams mirror (see the TessInstance field): only read where inline
    // bytes can't carry the struct, but created everywhere — 112 bytes.
    BufferDesc pb;
    pb.size = sizeof(TessParamsGpu);
    pb.usage = BufferUsage::Storage;
    pb.memoryUsage = MemoryUsage::GPU;
    t.paramsBuffer = rhi->createBuffer(pb);

    rhi->flushUploads();
    m_tessInstances.push_back(t);
    fmt::print("[Tess] mesh {}: {} roots (rootDepth {}), maxDepth {}, CBT {} KB\n",
               t.id, rootCount, rootDepth, maxDepth, cb.size / 1024);
    return t.id;
}

void Renderer::destroyTessellatedMesh(Uint32 id) {
    for (auto it = m_tessInstances.begin(); it != m_tessInstances.end(); ++it) {
        if (it->id != id) continue;
        rhi->destroyBuffer(it->cbtBuffer);
        rhi->destroyBuffer(it->rootBuffer);
        rhi->destroyBuffer(it->argsBuffer);
        rhi->destroyBuffer(it->leafDataBuffer);
        rhi->destroyBuffer(it->paramsBuffer);
        m_tessInstances.erase(it);
        return;
    }
}

void Renderer::setTessellatedMeshTransform(Uint32 id, const glm::mat4& model) {
    for (auto& t : m_tessInstances) {
        if (t.id == id) {
            t.model = model;
            return;
        }
    }
}

// ---- per-frame passes ------------------------------------------------------

bool Renderer::tessMeshPathActive() const {
    return tessPreferMeshShaders && capabilities.meshShaders && tessMeshPipeline.isValid();
}

TessParamsGpu Renderer::tessFillParams(const TessInstance& t) const {
    TessParamsGpu p{};
    p.model = t.model;
    p.maxDepth = t.maxDepth;
    p.rootDepth = t.rootDepth;
    p.rootCount = t.rootCount;
    p.maxLeaves = t.maxLeaves;
    p.splitPixels = tessSplitPixels;
    p.screenHeight = float(rhi->getSwapchainHeight());
    p.displacementScale = t.displacementScale;
    p.flags = (tessFreeze ? kTessFlagFreeze : 0u)
        | (t.terrainHeightfield ? kTessFlagTerrain : 0u);
    p.gridIndexCount = tessGridIndexCount;
    p.terrainFrequency = t.terrainFrequency;
    p.terrainSeed = t.terrainSeed;
    p.terrainOctaves = static_cast<Uint32>(std::max(t.terrainOctaves, 1));
    return p;
}

void Renderer::tessUpdatePass() {
    if (m_tessInstances.empty()) return;
    if (!tessClassifyPipeline.isValid() || !tessReduceFirstPipeline.isValid() ||
        !tessReduceLevelPipeline.isValid() || !tessArgsPipeline.isValid()) {
        return;
    }
    const bool meshPath = tessMeshPathActive();

    // Vulkan carries TessParams in the per-instance buffer (its 64-byte
    // compute push range can't fit the 112-byte struct) — refresh each one
    // before the encoder opens; the whole frame reads the same contents.
    if (tessParamsViaBuffer) {
        for (const TessInstance& t : m_tessInstances) {
            const TessParamsGpu p = tessFillParams(t);
            rhi->updateBuffer(t.paramsBuffer, &p, 0, sizeof(p));
        }
    }

    rhi->beginComputePass("TessUpdate");
    for (const TessInstance& t : m_tessInstances) {
        const TessParamsGpu p = tessFillParams(t);
        // Metal: TessParams as inline bytes; Vulkan: the per-instance buffer.
        const auto bindParams = [&] {
            if (tessParamsViaBuffer) rhi->setComputeBuffer(4, t.paramsBuffer);
            else rhi->setComputeBytes(&p, sizeof(p), 4);
        };

        // Classify: split/merge bit writes, sized by LAST frame's leaf count
        // (the args written by the previous frame's tessPrepareArgs).
        rhi->bindComputePipeline(tessClassifyPipeline);
        rhi->setComputeBuffer(0, t.cbtBuffer);      // read view
        rhi->setComputeBuffer(1, t.cbtBuffer);      // atomic write view (same buffer; unused on Vulkan)
        rhi->setComputeBuffer(2, t.rootBuffer);
        rhi->setComputeBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
        bindParams();
        rhi->dispatchIndirect(t.argsBuffer, kTessArgsClassifyOffset);
        rhi->computeBarrier();

        // Sum reduction: popcount into the deepest count level, then one
        // halving dispatch per level. All tiny; barrier-chained.
        struct { Uint32 maxDepth, level, pad0, pad1; } rp = { t.maxDepth, 0, 0, 0 };
        const Uint32 s = t.maxDepth - CBT::kCollapse;
        rhi->bindComputePipeline(tessReduceFirstPipeline);
        rhi->setComputeBuffer(0, t.cbtBuffer);
        rhi->setComputeBytes(&rp, sizeof(rp), 4);
        rhi->dispatch(((1u << s) + 63) / 64, 1, 1);
        rhi->computeBarrier();
        rhi->bindComputePipeline(tessReduceLevelPipeline);
        for (Uint32 lvl = s; lvl-- > 0;) {
            rp.level = lvl;
            rhi->setComputeBuffer(0, t.cbtBuffer);
            rhi->setComputeBytes(&rp, sizeof(rp), 4);
            rhi->dispatch(((1u << lvl) + 63) / 64, 1, 1);
            rhi->computeBarrier();
        }

        // Args: leaf count -> all indirect arguments for this frame's draw
        // and the next frame's classify.
        rhi->bindComputePipeline(tessArgsPipeline);
        rhi->setComputeBuffer(0, t.cbtBuffer);
        bindParams();
        rhi->setComputeBuffer(5, t.argsBuffer);
        rhi->dispatch(1, 1, 1);
        rhi->computeBarrier();

        // Leaf corner cache for the instanced path (the mesh path's object
        // stage decodes + culls inline instead).
        if (!meshPath && tessLeafPrepPipeline.isValid()) {
            rhi->bindComputePipeline(tessLeafPrepPipeline);
            rhi->setComputeBuffer(0, t.cbtBuffer);
            rhi->setComputeBuffer(2, t.rootBuffer);
            rhi->setComputeBuffer(3, cameraUniformBuffer, 0, sizeof(CameraRenderData));
            bindParams();
            rhi->setComputeBuffer(6, t.leafDataBuffer);
            rhi->dispatchIndirect(t.argsBuffer, kTessArgsLeafPrepOffset);
        }
    }
    rhi->endComputePass();
    rhi->computeBarrier();  // compute writes -> indirect args + vertex reads
}

void Renderer::tessRenderPass() {
    if (m_tessInstances.empty()) return;
    const bool meshPath = tessMeshPathActive();
    if (!meshPath && !tessRenderPipeline.isValid()) return;
    if (!colorRT.isValid() || !depthStencilRT.isValid()) return;

    RenderPassDesc rp;
    rp.name = "Tess";
    rp.colorAttachments.push_back(colorRT);
    rp.loadColor.push_back(true);
    rp.depthAttachment = depthStencilRT;
    rp.loadDepth = true;
    rhi->beginRenderPass(rp);
    for (const TessInstance& t : m_tessInstances) {
        const TessParamsGpu p = tessFillParams(t);
        if (meshPath) {
            // setVertexBuffer/Bytes route to BOTH object and mesh stages for
            // mesh pipelines (see RHI_Metal). Task grid size is GPU-written.
            rhi->bindPipeline(tessMeshPipeline);
            rhi->setVertexBuffer(0, t.cbtBuffer);
            rhi->setVertexBuffer(1, cameraUniformBuffer, 0, sizeof(CameraRenderData));
            rhi->setVertexBuffer(2, t.rootBuffer);
            rhi->setVertexBytes(&p, sizeof(p), 3);
            rhi->drawMeshTasksIndirect(t.argsBuffer, kTessArgsMeshTasksOffset);
        } else {
            // One grid instance per leaf; instanceCount is GPU-written.
            rhi->bindPipeline(tessRenderPipeline);
            rhi->setVertexBuffer(0, tessGridVertexBuffer);
            rhi->setVertexBuffer(1, cameraUniformBuffer, 0, sizeof(CameraRenderData));
            rhi->setVertexBuffer(2, t.leafDataBuffer);
            if (tessParamsViaBuffer) rhi->setVertexBuffer(3, t.paramsBuffer);
            else rhi->setVertexBytes(&p, sizeof(p), 3);
            rhi->bindIndexBuffer(tessGridIndexBuffer);
            rhi->drawIndexedIndirect(t.argsBuffer, kTessArgsDrawOffset, 1,
                                     sizeof(Vapor::DrawCommand));
        }
    }
    rhi->endRenderPass();
}

} // namespace Vapor
