#ifdef __APPLE__

#include "Vapor/rml_renderer_metal.hpp"
#include "helper.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fmt/core.h>

namespace Vapor {

RmlRendererMetal::RmlRendererMetal(MTL::Device* device) : m_device(device) {
    m_transform = Rml::Matrix4f::Identity();
}

RmlRendererMetal::~RmlRendererMetal() {
    shutdown();
}

bool RmlRendererMetal::initialize() {
    if (!m_device) return false;
    createDefaultWhiteTexture();
    createPipelineState();
    return m_pipelineState.get() != nullptr;
}

void RmlRendererMetal::shutdown() {
    m_geometry.clear();
    m_textures.clear();
    m_pipelineState.reset();
    m_depthStencilState.reset();
    m_defaultWhiteTexture.reset();
}

void RmlRendererMetal::setEncoder(MTL::RenderCommandEncoder* encoder,
                                   int logicalWidth, int logicalHeight,
                                   int fbWidth, int fbHeight) {
    m_encoder       = encoder;
    m_logicalWidth  = logicalWidth;
    m_logicalHeight = logicalHeight;
    m_scaleX = logicalWidth  > 0 ? static_cast<float>(fbWidth)  / logicalWidth  : 1.0f;
    m_scaleY = logicalHeight > 0 ? static_cast<float>(fbHeight) / logicalHeight : 1.0f;

    MTL::Viewport vp;
    vp.originX = 0; vp.originY = 0;
    vp.width   = fbWidth; vp.height = fbHeight;
    vp.znear   = 0; vp.zfar = 1;
    m_encoder->setViewport(vp);

    MTL::ScissorRect sr;
    sr.x = 0; sr.y = 0;
    sr.width  = static_cast<NS::UInteger>(fbWidth);
    sr.height = static_cast<NS::UInteger>(fbHeight);
    m_encoder->setScissorRect(sr);
}

void RmlRendererMetal::clearEncoder() {
    m_encoder = nullptr;
}

// ── Rml::RenderInterface ─────────────────────────────────────────────────────

Rml::CompiledGeometryHandle RmlRendererMetal::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices,
    Rml::Span<const int>         indices)
{
    if (!m_device) return 0;

    auto vb = NS::TransferPtr(m_device->newBuffer(
        vertices.size() * sizeof(Rml::Vertex), MTL::ResourceStorageModeShared));
    memcpy(vb->contents(), vertices.data(), vertices.size() * sizeof(Rml::Vertex));

    auto ib = NS::TransferPtr(m_device->newBuffer(
        indices.size() * sizeof(int), MTL::ResourceStorageModeShared));
    memcpy(ib->contents(), indices.data(), indices.size() * sizeof(int));

    Rml::CompiledGeometryHandle h = m_nextGeomHandle++;
    m_geometry[h] = { vb, ib, static_cast<NS::UInteger>(indices.size()) };
    return h;
}

void RmlRendererMetal::RenderGeometry(
    Rml::CompiledGeometryHandle geometry,
    Rml::Vector2f               translation,
    Rml::TextureHandle          texture)
{
    if (!m_encoder || !m_pipelineState) return;

    auto it = m_geometry.find(geometry);
    if (it == m_geometry.end()) return;
    const auto& geom = it->second;

    m_encoder->setRenderPipelineState(m_pipelineState.get());
    m_encoder->setDepthStencilState(m_depthStencilState.get());
    m_encoder->setCullMode(MTL::CullModeNone);

    glm::mat4 proj  = glm::ortho(0.0f, (float)m_logicalWidth, (float)m_logicalHeight, 0.0f, -1.0f, 1.0f);
    glm::mat4 xform = glm::make_mat4(m_transform.data());
    xform = glm::translate(xform, glm::vec3(translation.x, translation.y, 0.0f));

    struct Uniforms { glm::mat4 proj; glm::mat4 xform; } u;
    u.proj = proj; u.xform = xform;
    m_encoder->setVertexBytes(&u, sizeof(u), 0);
    m_encoder->setVertexBuffer(geom.vertexBuffer.get(), 0, 1);

    MTL::Texture* tex = m_defaultWhiteTexture.get();
    if (texture) {
        auto ti = m_textures.find(texture);
        if (ti != m_textures.end()) tex = ti->second.texture.get();
    }
    m_encoder->setFragmentTexture(tex, 0);

    if (m_scissor.enabled) {
        int fbH = static_cast<int>(m_logicalHeight * m_scaleY);
        MTL::ScissorRect sr;
        sr.x      = static_cast<NS::UInteger>(m_scissor.x * m_scaleX);
        sr.y      = static_cast<NS::UInteger>(fbH - (m_scissor.y + m_scissor.height) * m_scaleY);
        sr.width  = static_cast<NS::UInteger>(m_scissor.width  * m_scaleX);
        sr.height = static_cast<NS::UInteger>(m_scissor.height * m_scaleY);
        m_encoder->setScissorRect(sr);
    }

    if (geom.indexCount > 0) {
        m_encoder->drawIndexedPrimitives(
            MTL::PrimitiveTypeTriangle, geom.indexCount,
            MTL::IndexTypeUInt32, geom.indexBuffer.get(), 0);
    }
}

void RmlRendererMetal::ReleaseGeometry(Rml::CompiledGeometryHandle h) {
    m_geometry.erase(h);
}

void RmlRendererMetal::EnableScissorRegion(bool enable) {
    m_scissor.enabled = enable;
}

void RmlRendererMetal::SetScissorRegion(Rml::Rectanglei r) {
    m_scissor.x = r.Left(); m_scissor.y = r.Top();
    m_scissor.width = r.Width(); m_scissor.height = r.Height();
}

Rml::TextureHandle RmlRendererMetal::LoadTexture(Rml::Vector2i&, const Rml::String&) {
    return 0;
}

Rml::TextureHandle RmlRendererMetal::GenerateTexture(
    Rml::Span<const Rml::byte> source,
    Rml::Vector2i              dims)
{
    if (!m_device) return 0;
    auto desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    desc->setTextureType(MTL::TextureType2D);
    desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    desc->setWidth(dims.x); desc->setHeight(dims.y);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    auto tex = NS::TransferPtr(m_device->newTexture(desc.get()));
    if (!tex) return 0;
    MTL::Region region(0, 0, 0, dims.x, dims.y, 1);
    tex->replaceRegion(region, 0, source.data(), dims.x * 4);
    Rml::TextureHandle h = m_nextTexHandle++;
    m_textures[h] = { tex, dims.x, dims.y };
    return h;
}

void RmlRendererMetal::ReleaseTexture(Rml::TextureHandle h) {
    m_textures.erase(h);
}

void RmlRendererMetal::SetTransform(const Rml::Matrix4f* t) {
    m_transform = t ? *t : Rml::Matrix4f::Identity();
}

// ── External texture support ─────────────────────────────────────────────────

Rml::TextureHandle RmlRendererMetal::registerExternalTexture(MTL::Texture* texture, int width, int height) {
    if (!texture) return 0;
    Rml::TextureHandle h = m_nextTexHandle++;
    m_textures[h] = { NS::RetainPtr(texture), width, height };
    return h;
}

void RmlRendererMetal::updateExternalTexture(Rml::TextureHandle handle, MTL::Texture* texture, int width, int height) {
    auto it = m_textures.find(handle);
    if (it != m_textures.end()) {
        it->second.texture = NS::RetainPtr(texture);
        it->second.width   = width;
        it->second.height  = height;
    }
}

// ── Private ──────────────────────────────────────────────────────────────────

void RmlRendererMetal::createDefaultWhiteTexture() {
    auto desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    desc->setTextureType(MTL::TextureType2D);
    desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    desc->setWidth(1); desc->setHeight(1);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    m_defaultWhiteTexture = NS::TransferPtr(m_device->newTexture(desc.get()));
    if (m_defaultWhiteTexture) {
        uint8_t px[4] = {255, 255, 255, 255};
        MTL::Region r(0, 0, 0, 1, 1, 1);
        m_defaultWhiteTexture->replaceRegion(r, 0, px, 4);
    }
}

void RmlRendererMetal::createPipelineState() {
    std::string src = readFile("shaders/rmlui.metal");
    if (src.empty()) {
        fmt::print("[RmlRendererMetal] Failed to load rmlui.metal shader\n");
        return;
    }

    auto code = NS::String::string(src.data(), NS::StringEncoding::UTF8StringEncoding);
    NS::Error* err = nullptr;
    auto lib = NS::TransferPtr(m_device->newLibrary(code, nullptr, &err));
    if (!lib) {
        fmt::print("[RmlRendererMetal] Failed to compile rmlui.metal\n");
        return;
    }

    auto vf = lib->newFunction(NS::String::string("vertexMain",   NS::StringEncoding::UTF8StringEncoding));
    auto ff = lib->newFunction(NS::String::string("fragmentMain", NS::StringEncoding::UTF8StringEncoding));
    if (!vf || !ff) {
        if (vf) vf->release();
        if (ff) ff->release();
        return;
    }

    auto vd = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
    auto pa = vd->attributes()->object(0);
    pa->setFormat(MTL::VertexFormatFloat2);
    pa->setOffset(offsetof(Rml::Vertex, position)); pa->setBufferIndex(1);
    auto ca = vd->attributes()->object(1);
    ca->setFormat(MTL::VertexFormatUChar4Normalized);
    ca->setOffset(offsetof(Rml::Vertex, colour)); ca->setBufferIndex(1);
    auto ta = vd->attributes()->object(2);
    ta->setFormat(MTL::VertexFormatFloat2);
    ta->setOffset(offsetof(Rml::Vertex, tex_coord)); ta->setBufferIndex(1);
    auto lay = vd->layouts()->object(1);
    lay->setStride(sizeof(Rml::Vertex));
    lay->setStepFunction(MTL::VertexStepFunctionPerVertex); lay->setStepRate(1);

    auto pd = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    pd->setVertexFunction(vf); pd->setFragmentFunction(ff);
    pd->setVertexDescriptor(vd.get());
    auto col = pd->colorAttachments()->object(0);
    col->setPixelFormat(MTL::PixelFormatRGBA8Unorm_sRGB);
    col->setBlendingEnabled(true);
    col->setRgbBlendOperation(MTL::BlendOperationAdd);
    col->setAlphaBlendOperation(MTL::BlendOperationAdd);
    col->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
    col->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
    col->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    col->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    m_pipelineState = NS::TransferPtr(m_device->newRenderPipelineState(pd.get(), &err));

    auto ds = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
    ds->setDepthCompareFunction(MTL::CompareFunctionAlways);
    ds->setDepthWriteEnabled(false);
    m_depthStencilState = NS::TransferPtr(m_device->newDepthStencilState(ds.get()));

    vf->release();
    ff->release();
    // Note: lib is managed by NS::TransferPtr, do not call release()
}

} // namespace Vapor

#endif // __APPLE__
