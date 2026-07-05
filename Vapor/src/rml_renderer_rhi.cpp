#include "Vapor/rml_renderer_rhi.hpp"
#include "helper.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fmt/core.h>
#include <algorithm>
#include <cstring>

namespace Vapor {

RmlRendererRHI::RmlRendererRHI(RHI* rhi, GraphicsBackend backend)
    : m_rhi(rhi), m_backend(backend) {
    m_transform = Rml::Matrix4f::Identity();
}

RmlRendererRHI::~RmlRendererRHI() {
    shutdown();
}

bool RmlRendererRHI::initialize() {
    if (!m_rhi) return false;

    // Linear clamp sampler for UI textures.
    SamplerDesc sd;
    sd.minFilter = FilterMode::Linear;
    sd.magFilter = FilterMode::Linear;
    sd.mipFilter = FilterMode::Nearest;
    sd.addressModeU = AddressMode::ClampToEdge;
    sd.addressModeV = AddressMode::ClampToEdge;
    sd.addressModeW = AddressMode::ClampToEdge;
    m_sampler = m_rhi->createSampler(sd);

    // 1x1 white for untextured geometry.
    TextureDesc td;
    td.width = 1;
    td.height = 1;
    td.format = PixelFormat::RGBA8_UNORM;
    td.usage = TextureUsage::Sampled;
    m_whiteTexture = m_rhi->createTexture(td);
    Uint32 white = 0xFFFFFFFF;
    m_rhi->updateTexture(m_whiteTexture, &white, sizeof(white));

    // Shaders: SPIR-V on Vulkan, MSL source on Metal (rmlui_rhi.metal fetches
    // vertices raw from buffer(1) since RHI_Metal pipelines carry no vertex
    // descriptor; Vulkan consumes the vertex layout below).
    std::string vsCode, fsCode;
    const char* entryV = "main";
    const char* entryF = "main";
    if (m_backend == GraphicsBackend::Metal) {
        vsCode = readFile("shaders/rmlui_rhi.metal");
        fsCode = vsCode;
        entryV = "vertexMain";
        entryF = "fragmentMain";
    } else {
        vsCode = readFile("shaders/RmlUi.vert.spv");
        fsCode = readFile("shaders/RmlUi.frag.spv");
    }
    if (vsCode.empty() || fsCode.empty()) {
        fmt::print("[RmlRendererRHI] Failed to load RmlUI shaders\n");
        return false;
    }

    ShaderDesc vd;
    vd.stage = ShaderStage::Vertex;
    vd.code = vsCode.data();
    vd.codeSize = vsCode.size();
    vd.entryPoint = entryV;
    m_vertexShader = m_rhi->createShader(vd);

    ShaderDesc fd;
    fd.stage = ShaderStage::Fragment;
    fd.code = fsCode.data();
    fd.codeSize = fsCode.size();
    fd.entryPoint = entryF;
    m_fragmentShader = m_rhi->createShader(fd);

    // Alpha-blended UI over the swapchain, no depth.
    PipelineDesc pd;
    pd.vertexShader = m_vertexShader;
    pd.fragmentShader = m_fragmentShader;
    pd.vertexLayout.stride = sizeof(Rml::Vertex);
    pd.vertexLayout.attributes = {
        {0, PixelFormat::RG32_FLOAT, offsetof(Rml::Vertex, position)},
        {1, PixelFormat::RGBA8_UNORM, offsetof(Rml::Vertex, colour)},
        {2, PixelFormat::RG32_FLOAT, offsetof(Rml::Vertex, tex_coord)},
    };
    pd.topology = PrimitiveTopology::TriangleList;
    pd.blendMode = BlendMode::AlphaBlend;
    pd.depthTest = false;
    pd.depthWrite = false;
    pd.cullMode = CullMode::None;
    pd.sampleCount = 1;
    pd.hasDepthAttachment = false;
    pd.colorAttachmentFormats = { PixelFormat::Swapchain };
    m_pipeline = m_rhi->createPipeline(pd);

    return m_pipeline.isValid();
}

void RmlRendererRHI::shutdown() {
    if (!m_rhi) return;
    for (auto& [h, g] : m_geometry) {
        if (g.vertexBuffer.isValid()) m_rhi->destroyBuffer(g.vertexBuffer);
        if (g.indexBuffer.isValid()) m_rhi->destroyBuffer(g.indexBuffer);
    }
    m_geometry.clear();
    for (auto& [h, t] : m_textures) {
        if (t.texture.isValid()) m_rhi->destroyTexture(t.texture);
    }
    m_textures.clear();
    if (m_whiteTexture.isValid()) { m_rhi->destroyTexture(m_whiteTexture); m_whiteTexture = {}; }
    if (m_sampler.isValid()) { m_rhi->destroySampler(m_sampler); m_sampler = {}; }
    // Pipeline/shaders are reclaimed wholesale by RHI shutdown.
}

void RmlRendererRHI::beginFrame(int logicalWidth, int logicalHeight, int fbWidth, int fbHeight) {
    m_logicalWidth = logicalWidth;
    m_logicalHeight = logicalHeight;
    m_fbWidth = fbWidth;
    m_fbHeight = fbHeight;
    m_scaleX = logicalWidth > 0 ? float(fbWidth) / logicalWidth : 1.0f;
    m_scaleY = logicalHeight > 0 ? float(fbHeight) / logicalHeight : 1.0f;

    // UI projection, y-down in Rml coordinates. The Vulkan RHI renders with a
    // negative-height viewport (GL convention), so counter-flip Y there; the
    // Metal backend matches the native RmlRendererMetal projection.
    if (m_backend == GraphicsBackend::Vulkan) {
        m_projection = glm::orthoZO(0.0f, float(m_logicalWidth), 0.0f, float(m_logicalHeight), -1.0f, 1.0f);
    } else {
        m_projection = glm::ortho(0.0f, float(m_logicalWidth), float(m_logicalHeight), 0.0f, -1.0f, 1.0f);
    }
    m_inFrame = true;
}

void RmlRendererRHI::endFrame() {
    m_inFrame = false;
}

// ── Rml::RenderInterface ─────────────────────────────────────────────────────

Rml::CompiledGeometryHandle RmlRendererRHI::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    if (!m_rhi) return 0;

    BufferDesc vb;
    vb.size = vertices.size() * sizeof(Rml::Vertex);
    vb.usage = BufferUsage::Vertex;
    vb.memoryUsage = MemoryUsage::CPUtoGPU;
    BufferHandle vbo = m_rhi->createBuffer(vb);
    m_rhi->updateBuffer(vbo, vertices.data(), 0, vb.size);

    BufferDesc ib;
    ib.size = indices.size() * sizeof(int);
    ib.usage = BufferUsage::Index;
    ib.memoryUsage = MemoryUsage::CPUtoGPU;
    BufferHandle ibo = m_rhi->createBuffer(ib);
    m_rhi->updateBuffer(ibo, indices.data(), 0, ib.size);

    Rml::CompiledGeometryHandle h = m_nextGeomHandle++;
    m_geometry[h] = { vbo, ibo, static_cast<Uint32>(indices.size()) };
    return h;
}

void RmlRendererRHI::applyScissor() {
    if (m_scissor.enabled) {
        int x, y, w, hgt;
        if (m_backend == GraphicsBackend::Metal) {
            // Same mapping the native RmlRendererMetal uses (proven on device).
            x = int(m_scissor.x * m_scaleX);
            y = int(m_fbHeight - (m_scissor.y + m_scissor.height) * m_scaleY);
        } else {
            // Vulkan framebuffer space is top-left origin, like Rml.
            x = int(m_scissor.x * m_scaleX);
            y = int(m_scissor.y * m_scaleY);
        }
        w = int(m_scissor.width * m_scaleX);
        hgt = int(m_scissor.height * m_scaleY);
        // Clamp to the framebuffer (backends reject out-of-bounds scissors).
        x = std::max(0, x);
        y = std::max(0, y);
        w = std::min(w, m_fbWidth - x);
        hgt = std::min(hgt, m_fbHeight - y);
        if (w <= 0 || hgt <= 0) { w = 1; hgt = 1; }
        m_rhi->setScissor(x, y, Uint32(w), Uint32(hgt));
    } else {
        m_rhi->setScissor(0, 0, Uint32(m_fbWidth), Uint32(m_fbHeight));
    }
}

void RmlRendererRHI::RenderGeometry(
    Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
    Rml::TextureHandle texture) {
    if (!m_rhi || !m_inFrame || !m_pipeline.isValid()) return;
    auto it = m_geometry.find(geometry);
    if (it == m_geometry.end()) return;
    const auto& geom = it->second;

    m_rhi->bindPipeline(m_pipeline);
    applyScissor();

    // Premultiply projection * transform * translation into one mat4 so it
    // fits the 64-byte vertex push-constant range on Vulkan.
    glm::mat4 xform = glm::make_mat4(m_transform.data());
    xform = glm::translate(xform, glm::vec3(translation.x, translation.y, 0.0f));
    glm::mat4 mvp = m_projection * xform;
    m_rhi->setVertexBytes(&mvp, sizeof(glm::mat4), 0);  // Metal buffer(0) / VK push [0,64)

    m_rhi->bindVertexBuffer(geom.vertexBuffer, 1, 0);   // Metal buffer(1) / VK input binding 0
    m_rhi->bindIndexBuffer(geom.indexBuffer, 0);

    TextureHandle tex = m_whiteTexture;
    if (texture) {
        auto ti = m_textures.find(texture);
        if (ti != m_textures.end()) tex = ti->second.texture;
    }
    m_rhi->setTexture(0, 0, tex, m_sampler);

    if (geom.indexCount > 0) {
        m_rhi->drawIndexed(geom.indexCount, 1, 0, 0, 0);
    }
}

void RmlRendererRHI::ReleaseGeometry(Rml::CompiledGeometryHandle h) {
    auto it = m_geometry.find(h);
    if (it != m_geometry.end()) {
        if (it->second.vertexBuffer.isValid()) m_rhi->destroyBuffer(it->second.vertexBuffer);
        if (it->second.indexBuffer.isValid()) m_rhi->destroyBuffer(it->second.indexBuffer);
        m_geometry.erase(it);
    }
}

void RmlRendererRHI::EnableScissorRegion(bool enable) {
    m_scissor.enabled = enable;
}

void RmlRendererRHI::SetScissorRegion(Rml::Rectanglei r) {
    m_scissor.x = r.Left();
    m_scissor.y = r.Top();
    m_scissor.width = r.Width();
    m_scissor.height = r.Height();
}

Rml::TextureHandle RmlRendererRHI::LoadTexture(Rml::Vector2i&, const Rml::String&) {
    return 0;
}

Rml::TextureHandle RmlRendererRHI::GenerateTexture(
    Rml::Span<const Rml::byte> source, Rml::Vector2i dims) {
    if (!m_rhi) return 0;
    TextureDesc td;
    td.width = static_cast<Uint32>(dims.x);
    td.height = static_cast<Uint32>(dims.y);
    td.format = PixelFormat::RGBA8_UNORM;
    td.usage = TextureUsage::Sampled;
    TextureHandle tex = m_rhi->createTexture(td);
    if (!tex.isValid()) return 0;
    m_rhi->updateTexture(tex, source.data(), size_t(dims.x) * dims.y * 4);

    Rml::TextureHandle h = m_nextTexHandle++;
    m_textures[h] = { tex, dims.x, dims.y };
    return h;
}

void RmlRendererRHI::ReleaseTexture(Rml::TextureHandle h) {
    auto it = m_textures.find(h);
    if (it != m_textures.end()) {
        if (it->second.texture.isValid()) m_rhi->destroyTexture(it->second.texture);
        m_textures.erase(it);
    }
}

void RmlRendererRHI::SetTransform(const Rml::Matrix4f* t) {
    m_transform = t ? *t : Rml::Matrix4f::Identity();
}

} // namespace Vapor
