#pragma once
#include <SDL3/SDL_stdinc.h>
#include <glm/vec4.hpp>
#include <vector>

// Baked meshlet + cluster-LOD data for a mesh, produced offline by MeshletBuilder
// (meshoptimizer + clusterlod.h) and consumed at runtime by the task/mesh-shader
// path. Shader-agnostic: the same data drives a real mesh-shader pipeline or a
// compute-emulated fallback.
namespace Vapor {

// One meshlet/cluster: a small, self-contained triangle patch. `vertexOffset`
// indexes meshletVertices[] (each entry an index into the mesh's vertex buffer);
// `triangleOffset` indexes meshletTriangles[] (u8 local indices, 3 per triangle,
// each in [0, vertexCount)).
struct Meshlet {
    Uint32 vertexOffset;
    Uint32 triangleOffset;
    Uint32 vertexCount;
    Uint32 triangleCount;
};

// Per-meshlet culling bounds + the two-sphere Nanite LOD-cut metric. A cluster is
// selected for a pixel-error threshold t iff (projected parentError > t) AND
// (refined < 0 OR projected lodError <= t): the coarser parent would be too
// coarse, and this level is already accurate enough. Errors project through their
// own sphere (see clusterlod.h's projection note).
struct MeshletBounds {
    glm::vec4 cullSphere;      // xyz = center, w = radius (frustum/Hi-Z cull)
    glm::vec4 coneApex;        // xyz = apex,   w = unused (backface cone cull)
    glm::vec4 coneAxisCutoff;  // xyz = axis,   w = cutoff
    glm::vec4 lodSphere;       // xyz = center, w = radius (this cluster's LOD error sphere)
    glm::vec4 parentSphere;    // xyz = center, w = radius (coarser-step error sphere)
    float  lodError    = 0.0f;      // 0 for original (finest) geometry
    float  parentError = 3.4e38f;   // FLT_MAX for terminal (coarsest) groups
    Sint32 group   = -1;            // clod group id (DAG bookkeeping)
    Sint32 refined = -1;            // finer group this was simplified from, -1 if original
    Sint32 depth   = 0;             // DAG level (0 = full-detail)
};

struct MeshletData {
    std::vector<Meshlet>       meshlets;
    std::vector<Uint32>        meshletVertices;   // -> mesh vertex buffer
    std::vector<Uint8>         meshletTriangles;  // local, 3 per triangle
    std::vector<MeshletBounds> bounds;            // parallel to meshlets
    Uint32 lodLevelCount = 0;                     // max(depth)+1

    bool isBuilt() const { return !meshlets.empty(); }
    void clear() {
        meshlets.clear(); meshletVertices.clear();
        meshletTriangles.clear(); bounds.clear(); lodLevelCount = 0;
    }
};

} // namespace Vapor
