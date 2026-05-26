#pragma once
#include "graphics_mesh.hpp"
#include "graphics_sprite.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/vec2.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Vapor {

// ─────────────────────────────────────────────────────────────────────────────
// AtlasBaker
//
// CPU-side sprite atlas packer.  Accepts a list of individual Images and packs
// them into a single atlas Image using the MaxRects / BestShortSideFit
// algorithm.
//
// Typical usage:
//
//   std::vector<AtlasBaker::SpriteInput> inputs;
//   inputs.push_back({ "idle_0", AssetManager::loadImage("idle_0.png") });
//   inputs.push_back({ "idle_1", AssetManager::loadImage("idle_1.png") });
//
//   auto result = AtlasBaker::pack(inputs);
//   if (result.success) {
//       result.atlas.texture = renderer->createTexture(result.atlasImage);
//       AtlasHandle handle   = resourceManager.registerAtlas("player", result.atlas);
//   }
//
// ─────────────────────────────────────────────────────────────────────────────
class AtlasBaker {
public:
    struct SpriteInput {
        std::string            name;
        std::shared_ptr<Image> image;
        glm::vec2              pivot = {0.5f, 0.5f};
    };

    struct Result {
        bool                   success    = false;
        std::shared_ptr<Image> atlasImage;  // RGBA8 pixel data; pass to renderer->createTexture()
        SpriteAtlas            atlas;       // UV metadata; fill atlas.texture before registering
    };

    // Pack sprites into a single atlas.
    //
    //  maxSize  — largest allowed atlas dimension in pixels (power-of-two).
    //             The baker tries 512 → 1024 → 2048 → … up to maxSize until
    //             all sprites fit.
    //  padding  — transparent pixel gap between sprites; prevents UV bleeding.
    //  trim     — strip fully-transparent border pixels before packing; reduces
    //             wasted atlas space for sprites with large transparent margins.
    static Result pack(
        const std::vector<SpriteInput>& sprites,
        Uint32 maxSize = 4096,
        Uint32 padding = 1,
        bool   trim    = true
    );

private:
    // Internal rectangle type used by MaxRects
    struct Rect { Uint32 x = 0, y = 0, w = 0, h = 0; };

    // MaxRects bin: tracks free space and places new rects with BestShortSideFit
    class MaxRectsBin {
    public:
        MaxRectsBin(Uint32 width, Uint32 height);

        // Returns the top-left position where the rect was placed, or
        // {UINT32_MAX, UINT32_MAX} if it did not fit.
        struct PlaceResult { Uint32 x, y; bool ok; };
        PlaceResult insert(Uint32 w, Uint32 h);

    private:
        Uint32            binW, binH;
        std::vector<Rect> freeRects;

        void splitFreeNode(const Rect& freeNode, const Rect& placed);
        void pruneFreeList();
        static bool isContainedIn(const Rect& a, const Rect& b);
    };

    // Find the tight non-transparent bounding box of an image.
    // Returns the full image rect if channelCount < 4 (no alpha).
    static Rect trimmedBounds(const Image& img);

    // Copy a region of src into dst at (dstX, dstY).
    // Handles channel count mismatch by converting to RGBA8.
    static void blit(Image& dst, Uint32 dstX, Uint32 dstY,
                     const Image& src, Uint32 srcX, Uint32 srcY,
                     Uint32 w, Uint32 h);

    // Try packing all sprites into an atlas of the given size.
    // Returns empty optional if any sprite doesn't fit.
    struct PackEntry {
        Uint32    x, y;         // position in atlas
        Rect      srcRect;      // region of source image to copy (after trim)
        glm::vec2 pivot;
        std::string name;
        const Image* image;
    };
    static std::optional<std::vector<PackEntry>> tryPack(
        const std::vector<SpriteInput>& sprites,
        Uint32 atlasW, Uint32 atlasH,
        Uint32 padding, bool trim
    );
};

} // namespace Vapor
