#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include "SDL3/SDL_filesystem.h"
#include "Vapor/font_manager.hpp"
#include <cmath>
#include <fmt/format.h>
#include <fstream>

void FontManager::initialize(MTL::Device* device) {
    m_device = device;
}

FontHandle FontManager::loadFont(const std::string& filename, float baseSize, int firstChar, int numChars) {
    std::string path = SDL_GetBasePath() + filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        fmt::print(stderr, "[FontManager] Failed to open font file: {}\n", path);
        return FontHandle{};
    }

    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> fontData(fileSize);
    if (!file.read(reinterpret_cast<char*>(fontData.data()), fileSize)) {
        fmt::print(stderr, "[FontManager] Failed to read font file: {}\n", path);
        return FontHandle{};
    }

    // Create font entry
    FontHandle handle{ m_nextFontID++ };
    Font& font = m_fonts[handle.rid];
    font.fontSize = baseSize;

    // Bake the font atlas
    if (!bakeFontAtlas(font, fontData.data(), baseSize, firstChar, numChars)) {
        m_fonts.erase(handle.rid);
        return FontHandle{};
    }

    fmt::print(
        "[FontManager] Loaded font: {} (size: {}, atlas: {}x{})\n",
        path,
        baseSize,
        font.textureWidth,
        font.textureHeight
    );

    return handle;
}

void FontManager::unloadFont(FontHandle handle) {
    if (!handle.isValid()) return;

    auto it = m_fonts.find(handle.rid);
    if (it != m_fonts.end()) {
        m_fonts.erase(it);
    }

    auto atlasIt = m_atlasData.find(handle.rid);
    if (atlasIt != m_atlasData.end()) {
        m_atlasData.erase(atlasIt);
    }
}

Font* FontManager::getFont(FontHandle handle) {
    if (!handle.isValid()) return nullptr;
    auto it = m_fonts.find(handle.rid);
    return (it != m_fonts.end()) ? &it->second : nullptr;
}

TextureHandle FontManager::getFontTexture(FontHandle handle) {
    Font* font = getFont(handle);
    return font ? font->textureHandle : TextureHandle{};
}

glm::vec2 FontManager::measureText(FontHandle handle, const std::string& text, float scale) {
    Font* font = getFont(handle);
    if (!font) return glm::vec2(0.0f);

    float width = 0.0f;
    float maxHeight = 0.0f;

    for (char c : text) {
        auto it = font->glyphs.find(static_cast<int>(c));
        if (it != font->glyphs.end()) {
            width += it->second.advance * scale;
            float h = it->second.height * scale;
            if (h > maxHeight) maxHeight = h;
        }
    }

    return glm::vec2(width, maxHeight > 0 ? maxHeight : font->lineHeight * scale);
}

const Glyph* FontManager::getGlyph(FontHandle handle, int codepoint) {
    Font* font = getFont(handle);
    if (!font) return nullptr;

    auto it = font->glyphs.find(codepoint);
    return (it != font->glyphs.end()) ? &it->second : nullptr;
}

void FontManager::setFontTextureHandle(FontHandle fontHandle, TextureHandle texHandle) {
    Font* font = getFont(fontHandle);
    if (font) {
        font->textureHandle = texHandle;
    }
    // Clear the temporary atlas data since texture has been created
    m_atlasData.erase(fontHandle.rid);
}

const FontManager::AtlasData* FontManager::getAtlasData(FontHandle handle) const {
    auto it = m_atlasData.find(handle.rid);
    return (it != m_atlasData.end()) ? &it->second : nullptr;
}

bool FontManager::bakeFontAtlas(
    Font& font, const unsigned char* fontData, float fontSize, int firstChar, int numChars
) {
    // Initialize stb_truetype
    stbtt_fontinfo fontInfo;
    if (!stbtt_InitFont(&fontInfo, fontData, 0)) {
        fmt::print(stderr, "[FontManager] Failed to initialize font\n");
        return false;
    }

    // Get font metrics
    float scale = stbtt_ScaleForPixelHeight(&fontInfo, fontSize);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);

    font.ascent = ascent * scale;
    font.descent = descent * scale;
    font.lineHeight = (ascent - descent + lineGap) * scale;

    // Calculate atlas size (power of 2)
    int glyphsPerRow = static_cast<int>(std::ceil(std::sqrt(numChars)));
    int atlasSize = 1;
    while (atlasSize < glyphsPerRow * static_cast<int>(fontSize * 1.5f)) {
        atlasSize *= 2;
    }
    atlasSize = std::min(atlasSize, 2048);// Cap at 2048

    font.textureWidth = atlasSize;
    font.textureHeight = atlasSize;

    // Allocate atlas bitmap (single channel)
    std::vector<unsigned char> atlasBitmap(atlasSize * atlasSize, 0);

    // Pack glyphs
    int x = 1, y = 1;
    int rowHeight = 0;
    int padding = 2;

    for (int i = 0; i < numChars; i++) {
        int codepoint = firstChar + i;

        // Get glyph metrics
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&fontInfo, codepoint, &advance, &lsb);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&fontInfo, codepoint, scale, scale, &x0, &y0, &x1, &y1);

        int glyphWidth = x1 - x0;
        int glyphHeight = y1 - y0;

        // Check if glyph fits in current row
        if (x + glyphWidth + padding > atlasSize) {
            x = 1;
            y += rowHeight + padding;
            rowHeight = 0;
        }

        // Check if atlas is full
        if (y + glyphHeight + padding > atlasSize) {
            fmt::print(stderr, "[FontManager] Atlas too small for all glyphs\n");
            break;
        }

        // Render glyph to atlas
        if (glyphWidth > 0 && glyphHeight > 0) {
            stbtt_MakeCodepointBitmap(
                &fontInfo, &atlasBitmap[y * atlasSize + x], glyphWidth, glyphHeight, atlasSize, scale, scale, codepoint
            );
        }

        // Store glyph info
        Glyph glyph;
        glyph.u0 = static_cast<float>(x) / atlasSize;
        glyph.v0 = static_cast<float>(y) / atlasSize;
        glyph.u1 = static_cast<float>(x + glyphWidth) / atlasSize;
        glyph.v1 = static_cast<float>(y + glyphHeight) / atlasSize;
        glyph.xOffset = static_cast<float>(x0);
        glyph.yOffset = static_cast<float>(y0);
        glyph.width = static_cast<float>(glyphWidth);
        glyph.height = static_cast<float>(glyphHeight);
        glyph.advance = advance * scale;

        font.glyphs[codepoint] = glyph;

        // Update position
        x += glyphWidth + padding;
        if (glyphHeight > rowHeight) rowHeight = glyphHeight;
    }

    // Convert single-channel to RGBA (white text with alpha)
    // Store in atlas data for later texture creation
    AtlasData& atlasData = m_atlasData[m_nextFontID - 1];// Use the ID we just assigned
    atlasData.width = atlasSize;
    atlasData.height = atlasSize;
    atlasData.rgbaData.resize(atlasSize * atlasSize * 4);

    for (int i = 0; i < atlasSize * atlasSize; i++) {
        atlasData.rgbaData[i * 4 + 0] = 255;// R
        atlasData.rgbaData[i * 4 + 1] = 255;// G
        atlasData.rgbaData[i * 4 + 2] = 255;// B
        atlasData.rgbaData[i * 4 + 3] = atlasBitmap[i];// A (from grayscale)
    }

    return true;
}
