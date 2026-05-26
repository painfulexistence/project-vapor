#include "Vapor/atlas_baker.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <numeric>

namespace Vapor {

// ─────────────────────────────────────────────────────────────────────────────
// MaxRectsBin
// ─────────────────────────────────────────────────────────────────────────────

AtlasBaker::MaxRectsBin::MaxRectsBin(Uint32 width, Uint32 height)
    : binW(width), binH(height) {
    freeRects.push_back({0, 0, width, height});
}

AtlasBaker::MaxRectsBin::PlaceResult AtlasBaker::MaxRectsBin::insert(Uint32 w, Uint32 h) {
    int   bestIdx       = -1;
    int   bestShortSide = INT_MAX;
    int   bestLongSide  = INT_MAX;

    for (int i = 0; i < static_cast<int>(freeRects.size()); ++i) {
        const Rect& fr = freeRects[i];
        if (fr.w < w || fr.h < h) continue;

        int shortSide = static_cast<int>(std::min(fr.w - w, fr.h - h));
        int longSide  = static_cast<int>(std::max(fr.w - w, fr.h - h));
        if (shortSide < bestShortSide
            || (shortSide == bestShortSide && longSide < bestLongSide)) {
            bestShortSide = shortSide;
            bestLongSide  = longSide;
            bestIdx       = i;
        }
    }

    if (bestIdx < 0) return {0, 0, false};

    Rect placed = {freeRects[bestIdx].x, freeRects[bestIdx].y, w, h};
    splitFreeNode(freeRects[bestIdx], placed);
    freeRects.erase(freeRects.begin() + bestIdx);
    pruneFreeList();

    return {placed.x, placed.y, true};
}

void AtlasBaker::MaxRectsBin::splitFreeNode(const Rect& fn, const Rect& pl) {
    // Right slice
    if (pl.x + pl.w < fn.x + fn.w)
        freeRects.push_back({pl.x + pl.w, fn.y,
                             (fn.x + fn.w) - (pl.x + pl.w), fn.h});
    // Top slice
    if (pl.y + pl.h < fn.y + fn.h)
        freeRects.push_back({fn.x, pl.y + pl.h,
                             fn.w, (fn.y + fn.h) - (pl.y + pl.h)});
    // Left slice
    if (fn.x < pl.x)
        freeRects.push_back({fn.x, fn.y, pl.x - fn.x, fn.h});
    // Bottom slice
    if (fn.y < pl.y)
        freeRects.push_back({fn.x, fn.y, fn.w, pl.y - fn.y});
}

void AtlasBaker::MaxRectsBin::pruneFreeList() {
    for (int i = 0; i < static_cast<int>(freeRects.size()); ++i) {
        for (int j = i + 1; j < static_cast<int>(freeRects.size()); ++j) {
            if (isContainedIn(freeRects[i], freeRects[j])) {
                freeRects.erase(freeRects.begin() + i);
                --i; break;
            }
            if (isContainedIn(freeRects[j], freeRects[i])) {
                freeRects.erase(freeRects.begin() + j);
                --j;
            }
        }
    }
}

bool AtlasBaker::MaxRectsBin::isContainedIn(const Rect& a, const Rect& b) {
    return a.x >= b.x && a.y >= b.y
        && a.x + a.w <= b.x + b.w
        && a.y + a.h <= b.y + b.h;
}

// ─────────────────────────────────────────────────────────────────────────────
// Trim
// ─────────────────────────────────────────────────────────────────────────────

AtlasBaker::Rect AtlasBaker::trimmedBounds(const Image& img) {
    if (img.channelCount < 4 || img.byteArray.empty())
        return {0, 0, img.width, img.height};

    Uint32 minX = img.width, maxX = 0;
    Uint32 minY = img.height, maxY = 0;

    for (Uint32 y = 0; y < img.height; ++y) {
        for (Uint32 x = 0; x < img.width; ++x) {
            Uint8 alpha = img.byteArray[(y * img.width + x) * img.channelCount + 3];
            if (alpha > 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    if (minX > maxX) return {0, 0, 1, 1};  // fully transparent → keep 1×1
    return {minX, minY, maxX - minX + 1, maxY - minY + 1};
}

// ─────────────────────────────────────────────────────────────────────────────
// Blit
// ─────────────────────────────────────────────────────────────────────────────

void AtlasBaker::blit(Image& dst, Uint32 dstX, Uint32 dstY,
                      const Image& src, Uint32 srcX, Uint32 srcY,
                      Uint32 w, Uint32 h) {
    const Uint32 dstCh = dst.channelCount;
    const Uint32 srcCh = src.channelCount;

    for (Uint32 row = 0; row < h; ++row) {
        const Uint8* srcPtr = src.byteArray.data()
            + ((srcY + row) * src.width + srcX) * srcCh;
        Uint8* dstPtr = dst.byteArray.data()
            + ((dstY + row) * dst.width + dstX) * dstCh;

        if (srcCh == dstCh) {
            std::memcpy(dstPtr, srcPtr, w * srcCh);
        } else {
            // Convert: expand/collapse channels, fill missing with 0/255
            for (Uint32 col = 0; col < w; ++col) {
                Uint8 r = srcCh > 0 ? srcPtr[col * srcCh + 0] : 0;
                Uint8 g = srcCh > 1 ? srcPtr[col * srcCh + 1] : r;
                Uint8 b = srcCh > 2 ? srcPtr[col * srcCh + 2] : r;
                Uint8 a = srcCh > 3 ? srcPtr[col * srcCh + 3] : 255;
                if (dstCh > 0) dstPtr[col * dstCh + 0] = r;
                if (dstCh > 1) dstPtr[col * dstCh + 1] = g;
                if (dstCh > 2) dstPtr[col * dstCh + 2] = b;
                if (dstCh > 3) dstPtr[col * dstCh + 3] = a;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// tryPack
// ─────────────────────────────────────────────────────────────────────────────

std::optional<std::vector<AtlasBaker::PackEntry>> AtlasBaker::tryPack(
    const std::vector<SpriteInput>& sprites,
    Uint32 atlasW, Uint32 atlasH,
    Uint32 padding, bool trim
) {
    MaxRectsBin bin(atlasW, atlasH);
    std::vector<PackEntry> entries;
    entries.reserve(sprites.size());

    // Sort by descending area for better packing density
    std::vector<size_t> order(sprites.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& ia = *sprites[a].image;
        const auto& ib = *sprites[b].image;
        return (ia.width * ia.height) > (ib.width * ib.height);
    });

    for (size_t idx : order) {
        const SpriteInput& s = sprites[idx];
        if (!s.image) continue;

        Rect srcRect = trim ? trimmedBounds(*s.image)
                            : Rect{0, 0, s.image->width, s.image->height};

        Uint32 pw = srcRect.w + padding * 2;
        Uint32 ph = srcRect.h + padding * 2;

        auto placed = bin.insert(pw, ph);
        if (!placed.ok) return std::nullopt;

        entries.push_back({
            placed.x + padding,
            placed.y + padding,
            srcRect,
            s.pivot,
            s.name,
            s.image.get()
        });
    }

    return entries;
}

// ─────────────────────────────────────────────────────────────────────────────
// pack  (public)
// ─────────────────────────────────────────────────────────────────────────────

AtlasBaker::Result AtlasBaker::pack(
    const std::vector<SpriteInput>& sprites,
    Uint32 maxSize,
    Uint32 padding,
    bool   trim
) {
    if (sprites.empty()) return {};

    // Try increasing power-of-two atlas sizes until everything fits
    std::optional<std::vector<PackEntry>> entries;
    Uint32 atlasSize = 512;
    while (atlasSize <= maxSize) {
        entries = tryPack(sprites, atlasSize, atlasSize, padding, trim);
        if (entries) break;
        atlasSize *= 2;
    }
    if (!entries) return {};  // sprites don't fit even at maxSize

    // Allocate atlas image (RGBA8, zeroed = fully transparent)
    auto atlasImg        = std::make_shared<Image>();
    atlasImg->width      = atlasSize;
    atlasImg->height     = atlasSize;
    atlasImg->channelCount = 4;
    atlasImg->byteArray.resize(atlasSize * atlasSize * 4, 0);

    // Build SpriteAtlas metadata and blit pixels
    SpriteAtlas atlas;
    atlas.name = "baked_atlas";
    atlas.size = {static_cast<float>(atlasSize), static_cast<float>(atlasSize)};
    atlas.frames.reserve(sprites.size());

    for (const PackEntry& e : *entries) {
        // Blit source pixels into atlas
        blit(*atlasImg, e.x, e.y,
             *e.image, e.srcRect.x, e.srcRect.y,
             e.srcRect.w, e.srcRect.h);

        // Compute normalised UV rect (minU, minV, maxU, maxV)
        float invW = 1.0f / static_cast<float>(atlasSize);
        float invH = 1.0f / static_cast<float>(atlasSize);
        glm::vec4 uvRect = {
            static_cast<float>(e.x)              * invW,
            static_cast<float>(e.y)              * invH,
            static_cast<float>(e.x + e.srcRect.w) * invW,
            static_cast<float>(e.y + e.srcRect.h) * invH,
        };

        Uint16 frameIdx = static_cast<Uint16>(atlas.frames.size());
        atlas.nameToIndex[e.name] = frameIdx;
        atlas.frames.push_back(SpriteFrame{
            e.name,
            uvRect,
            {static_cast<float>(e.image->width), static_cast<float>(e.image->height)},
            {static_cast<float>(e.srcRect.x),    static_cast<float>(e.srcRect.y)},
            e.pivot,
            false   // not rotated
        });
    }

    return Result{true, atlasImg, atlas};
}

} // namespace Vapor
