#pragma once
#include "graphics.hpp"  // Image (CPU pixel data; no GPU calls here)
#include <SDL3/SDL_stdinc.h>
#include <functional>
#include <glm/vec2.hpp>
#include <memory>
#include <vector>

namespace Vapor {

// ============================================================================
// TerrainTextureGen — procedural stand-ins for a Gaea/WorldCreator texture
// export (ported verbatim from Atmospheric's terrain_texture_gen). Everything
// here is plain CPU noise synthesis, deterministic per (res, seed) on every
// platform, so it is unit-testable without a GPU:
//
//   * generate* : one seamlessly tiling detail material (albedo + tangent-
//                 space normal) per surface type, from PERIODIC FBm value
//                 noise (lattice wraps at the octave period — that is what
//                 makes them tile). Returned as CPU Images; the renderer
//                 uploads them with REPEAT wrap so they tile in world space.
//   * defaultSplat : per-region RGBA weights {grass, rock, dirt, snow} from
//                 the streamed height source, using WORLD-space (non-periodic)
//                 noise so neighbouring tiles agree across their border.
//
// The terrain shader blends the four layers by these weights; the same look
// is reproduced without a splat texture by recomputing the weights per
// fragment (the automatic slope/height path in the terrain shader).
// ============================================================================

namespace TerrainTextureGen {

    struct DetailLayer {
        std::shared_ptr<Image> albedo;
        std::shared_ptr<Image> normal;
    };

    // res^2 RGBA8 albedo + normal, tiling seamlessly. Call once at init
    // (a 512^2 layer is a few milliseconds).
    DetailLayer generateGrass(int res = 512, Uint32 seed = 11);
    DetailLayer generateRock(int res = 512, Uint32 seed = 22);
    DetailLayer generateDirt(int res = 512, Uint32 seed = 33);
    DetailLayer generateSnow(int res = 512, Uint32 seed = 44);

    struct SplatParams {
        float heightScale = 500.0f;   // metres of displacement for height 1.0
        float rockSlopeStart = 0.55f; // rise/run where rock starts winning
        float rockSlopeFull = 1.05f;  // ... and where it fully owns the texel
        float snowStart = 0.62f;      // normalized height where snow begins
        float snowFull = 0.78f;       // ... and reaches full cover
        float dirtAmount = 0.55f;     // 0..1 strength of the worn dirt/scree
        Uint32 seed = 7u;
    };

    // res*res*4 bytes, RGBA = {grass, rock, dirt, snow} weights over the world
    // rect. height01 must be the streamer's exact height source (thread-safe).
    std::vector<Uint8> defaultSplat(glm::vec2 worldMin, glm::vec2 worldMax, int res,
                                    const std::function<float(float, float)>& height01,
                                    const SplatParams& params);

}// namespace TerrainTextureGen

}// namespace Vapor
