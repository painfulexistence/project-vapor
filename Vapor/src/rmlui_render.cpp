#include "Vapor/rmlui_render.hpp"
#include <fmt/core.h>
#include "stb/stb_image.h"

namespace Vapor {

RmlUi_RenderInterface::RmlUi_RenderInterface() {
}

RmlUi_RenderInterface::~RmlUi_RenderInterface() {
    if (renderer) {
        // Release all geometries
        for (auto& [id, geom] : compiledGeometries) {
            renderer->rmluiReleaseGeometry(id);
        }
        compiledGeometries.clear();
    }
}

void RmlUi_RenderInterface::setRenderer(RmlUiRenderer* r) {
    renderer = r;
    if (renderer) {
        renderer->rmluiInit();
    }
}

void RmlUi_RenderInterface::setViewportDimensions(int width, int height) {
    viewportWidth = width;
    viewportHeight = height;
    if (renderer) {
        renderer->rmluiSetViewport(width, height);
    }
}

void RmlUi_RenderInterface::beginFrame() {
    if (renderer) {
        renderer->rmluiBeginFrame();
    }
}

void RmlUi_RenderInterface::endFrame() {
    if (renderer) {
        renderer->rmluiEndFrame();
    }
}

Rml::CompiledGeometryHandle RmlUi_RenderInterface::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices,
    Rml::Span<const int> indices
) {
    if (!renderer) {
        return Rml::CompiledGeometryHandle(0);
    }

    // Convert RmlUi vertices to our format
    std::vector<RmlUiVertex> convertedVertices;
    convertedVertices.reserve(vertices.size());

    for (const auto& v : vertices) {
        RmlUiVertex vertex;
        vertex.position = glm::vec2(v.position.x, v.position.y);
        vertex.color = glm::u8vec4(v.colour.red, v.colour.green, v.colour.blue, v.colour.alpha);
        vertex.texCoord = glm::vec2(v.tex_coord.x, v.tex_coord.y);
        convertedVertices.push_back(vertex);
    }

    // Convert indices
    std::vector<Uint32> convertedIndices;
    convertedIndices.reserve(indices.size());
    for (int idx : indices) {
        convertedIndices.push_back(static_cast<Uint32>(idx));
    }

    // Create geometry in renderer
    Uint32 geometryId = renderer->rmluiCreateGeometry(convertedVertices, convertedIndices);

    // Store compiled geometry data for reference
    RmlUiCompiledGeometry compiledGeom;
    compiledGeom.vertices = std::move(convertedVertices);
    compiledGeom.indices = std::move(convertedIndices);
    compiledGeom.textureId = 0;
    compiledGeometries[geometryId] = std::move(compiledGeom);

    return Rml::CompiledGeometryHandle(geometryId);
}

void RmlUi_RenderInterface::RenderGeometry(
    Rml::CompiledGeometryHandle geometry,
    Rml::Vector2f translation,
    Rml::TextureHandle texture
) {
    if (!renderer) {
        return;
    }

    Uint32 geometryId = static_cast<Uint32>(geometry);
    Uint32 textureId = static_cast<Uint32>(texture);
    bool hasTexture = (textureId != 0);

    // Apply scissor if enabled
    if (scissorEnabled) {
        renderer->rmluiEnableScissor(scissorX, scissorY, scissorWidth, scissorHeight);
    } else {
        renderer->rmluiDisableScissor();
    }

    renderer->rmluiRenderGeometry(
        geometryId,
        glm::vec2(translation.x, translation.y),
        textureId,
        hasTexture
    );
}

void RmlUi_RenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    if (!renderer) {
        return;
    }

    Uint32 geometryId = static_cast<Uint32>(geometry);
    renderer->rmluiReleaseGeometry(geometryId);
    compiledGeometries.erase(geometryId);
}

Rml::TextureHandle RmlUi_RenderInterface::LoadTexture(
    Rml::Vector2i& texture_dimensions,
    const Rml::String& source
) {
    if (!renderer) {
        return Rml::TextureHandle(0);
    }

    // Load image using stb_image
    int width, height, channels;
    unsigned char* data = stbi_load(source.c_str(), &width, &height, &channels, 4);

    if (!data) {
        fmt::print("[RmlUi] Failed to load texture: {}\n", source);
        return Rml::TextureHandle(0);
    }

    texture_dimensions.x = width;
    texture_dimensions.y = height;

    Uint32 textureId = renderer->rmluiCreateTexture(
        static_cast<Uint32>(width),
        static_cast<Uint32>(height),
        data
    );

    stbi_image_free(data);

    return Rml::TextureHandle(textureId);
}

Rml::TextureHandle RmlUi_RenderInterface::GenerateTexture(
    Rml::Span<const Rml::byte> source,
    Rml::Vector2i source_dimensions
) {
    if (!renderer) {
        return Rml::TextureHandle(0);
    }

    Uint32 textureId = renderer->rmluiCreateTexture(
        static_cast<Uint32>(source_dimensions.x),
        static_cast<Uint32>(source_dimensions.y),
        source.data()
    );

    return Rml::TextureHandle(textureId);
}

void RmlUi_RenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
    if (!renderer) {
        return;
    }

    Uint32 textureId = static_cast<Uint32>(texture);
    if (textureId != 0) {
        renderer->rmluiReleaseTexture(textureId);
    }
}

void RmlUi_RenderInterface::EnableScissorRegion(bool enable) {
    scissorEnabled = enable;
}

void RmlUi_RenderInterface::SetScissorRegion(Rml::Rectanglei region) {
    scissorX = region.Left();
    scissorY = region.Top();
    scissorWidth = region.Width();
    scissorHeight = region.Height();
}

} // namespace Vapor
