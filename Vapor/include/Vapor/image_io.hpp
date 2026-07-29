#pragma once
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace Vapor {

// ============================================================================
// HDR image output — how a path-traced photo leaves the engine.
//
// writeEXR is a self-contained OpenEXR 2.0 writer: single part, scanline,
// 32-bit float, NO compression. ~100 lines against the published format spec
// beats vendoring a 12k-line dependency for the one variant we emit, and the
// round-trip test parses the bytes back with an independent reader, so the
// writer is locked by tests rather than trusted.
//
// The EXR stores linear Rec.709 — no tonemap, no gamma. That is the contract
// of the format and what makes the file gradeable downstream. The PNG twin is
// the opposite: a display-referred preview (exposure -> ACES -> sRGB), for
// looking at, not for grading.
// ============================================================================

// `rgb` is width*height*3 floats, linear, row 0 = top of image.
bool writeEXR(const std::string& path, const float* rgb, Uint32 width, Uint32 height);

// Tonemapped 8-bit preview of the same data via stb_image_write.
bool writeTonemappedPNG(const std::string& path, const float* rgb, Uint32 width, Uint32 height,
                        float exposure = 1.0f);

// ── Tonemap helpers (inline: unit-tested without a GPU) ────────────────────

// ACES filmic fit (Narkowicz 2015). Monotonic, maps [0,inf) into [0,1).
inline float acesTonemap(float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float y = (x * (a * x + b)) / (x * (c * x + d) + e);
    return std::clamp(y, 0.0f, 1.0f);
}

// Linear -> sRGB encode, matching the piecewise curve in 3d_common.metal's
// linearToSRGB (not the pow(1/2.2) approximation).
inline float linearToSRGB(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace Vapor
