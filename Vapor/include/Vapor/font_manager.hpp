#pragma once
#include "graphics.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/vec2.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations
namespace MTL {
    class Device;
    class Texture;
}

namespace NS {
    template<typename T>
    class SharedPtr;
}

// Handle type for fonts
struct FontHandle {
    Uint32 rid = UINT32_MAX;
    bool isValid() const { return rid != UINT32_MAX; }
};

// Glyph metrics and UV coordinates
struct Glyph {
    float u0 = 0.0f, v0 = 0.0f;  // Top-left UV
    float u1 = 0.0f, v1 = 0.0f;  // Bottom-right UV
    float xOffset = 0.0f;        // X offset from cursor
    float yOffset = 0.0f;        // Y offset from baseline
    float width = 0.0f;          // Glyph width in pixels
    float height = 0.0f;         // Glyph height in pixels
    float advance = 0.0f;        // Horizontal advance
};

// Font data
struct Font {
    float fontSize = 0.0f;       // Base font size
    float ascent = 0.0f;         // Ascent (above baseline)
    float descent = 0.0f;        // Descent (below baseline)
    float lineHeight = 0.0f;     // Total line height
    Uint32 textureWidth = 0;     // Atlas texture width
    Uint32 textureHeight = 0;    // Atlas texture height
    TextureHandle textureHandle; // Atlas texture handle
    std::unordered_map<int, Glyph> glyphs; // Codepoint -> Glyph mapping
};

// FontManager - handles font loading, atlas generation, and glyph lookup
class FontManager {
public:
    FontManager() = default;
    ~FontManager() = default;

    // Initialize with Metal device (must be called before loading fonts)
    void initialize(MTL::Device* device);

    // Load a font from file, returns handle or invalid handle on failure
    // firstChar/numChars control which characters are baked into the atlas
    FontHandle loadFont(const std::string& path, float baseSize, int firstChar = 32, int numChars = 96);

    // Unload a previously loaded font
    void unloadFont(FontHandle handle);

    // Get font data (returns nullptr if not found)
    Font* getFont(FontHandle handle);

    // Get the texture handle for a font's atlas
    TextureHandle getFontTexture(FontHandle handle);

    // Measure text dimensions at given scale
    glm::vec2 measureText(FontHandle handle, const std::string& text, float scale);

    // Get a specific glyph (returns nullptr if not found)
    const Glyph* getGlyph(FontHandle handle, int codepoint);

    // Register a texture handle (called by Renderer after creating Metal texture)
    void setFontTextureHandle(FontHandle fontHandle, TextureHandle texHandle);

    // Get raw atlas data for texture creation (width, height, RGBA data)
    struct AtlasData {
        Uint32 width = 0;
        Uint32 height = 0;
        std::vector<Uint8> rgbaData;
    };
    const AtlasData* getAtlasData(FontHandle handle) const;

private:
    bool bakeFontAtlas(Font& font, const unsigned char* fontData, float fontSize, int firstChar, int numChars);

    MTL::Device* m_device = nullptr;
    std::unordered_map<Uint32, Font> m_fonts;
    std::unordered_map<Uint32, AtlasData> m_atlasData; // Temporary storage until texture is created
    Uint32 m_nextFontID = 1;
};
