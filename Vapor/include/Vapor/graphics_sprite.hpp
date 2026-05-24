#pragma once
#include "graphics_handles.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// Single frame within a sprite atlas
struct SpriteFrame {
    std::string name;
    glm::vec4 uvRect = {0, 0, 1, 1};  // minU, minV, maxU, maxV
    glm::vec2 sourceSize = {1, 1};    // Original size in pixels
    glm::vec2 offset = {0, 0};        // Trim offset
    glm::vec2 pivot = {0.5f, 0.5f};   // Anchor point (0-1)
    bool rotated = false;              // 90° rotation for packing
};

// Sprite atlas containing multiple named frames packed into a single texture
struct SpriteAtlas {
    std::string name;
    TextureHandle texture;
    glm::vec2 size = {0, 0};  // Atlas dimensions in pixels
    std::vector<SpriteFrame> frames;
    std::unordered_map<std::string, uint16_t> nameToIndex;

    const SpriteFrame* getFrame(uint16_t index) const {
        return index < frames.size() ? &frames[index] : nullptr;
    }

    const SpriteFrame* getFrame(const std::string& frameName) const {
        auto it = nameToIndex.find(frameName);
        return it != nameToIndex.end() ? &frames[it->second] : nullptr;
    }

    uint16_t getFrameIndex(const std::string& frameName) const {
        auto it = nameToIndex.find(frameName);
        return it != nameToIndex.end() ? it->second : UINT16_MAX;
    }
};
