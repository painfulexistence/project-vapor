#pragma once
#include "cbt.hpp"
#include <SDL3/SDL_stdinc.h>
#include <glm/glm.hpp>
#include <vector>

// ============================================================================
// Adaptive GPU tessellation — CPU-side data for the CBT/LEB pipeline.
//
// GPU-layout mirrors of the structs in assets/shaders/3d_tess_lib.metal (the
// static_asserts pin the byte contracts), the fan-root buffer builder, and
// the per-leaf grid the instanced compute path draws. The runtime pipeline
// lives in src/tessellation.cpp (Renderer::tessUpdatePass / tessRenderPass).
// ============================================================================

namespace Vapor {

struct Mesh;

// ---- GPU struct mirrors (see 3d_tess_lib.metal) ----------------------------

// One LEB root: three corners (position + uv packed in .w, normal + uv.y in
// .w) and the root triangle's same-depth adjacency as heap ids.
struct alignas(16) TessRootGpu {
    glm::vec4 posU[3];  // xyz = object-space position of corner vi, w = uv.x
    glm::vec4 nrmV[3];  // xyz = object-space normal at corner vi,  w = uv.y
    Uint32 left;
    Uint32 right;
    Uint32 edge;
    Uint32 node;
};
static_assert(sizeof(TessRootGpu) == 112, "must match MSL TessRoot");

// Inline constants for the tess kernels / render stages (112 bytes).
struct alignas(16) TessParamsGpu {
    glm::mat4 model;
    Uint32 maxDepth;
    Uint32 rootDepth;
    Uint32 rootCount;
    Uint32 maxLeaves;
    float splitPixels;
    float screenHeight;
    float displacementScale;
    Uint32 flags;  // bit0 = freeze
    Uint32 gridIndexCount;
    Uint32 pad0;
    Uint32 pad1;
    Uint32 pad2;
};
static_assert(sizeof(TessParamsGpu) == 112, "must match MSL TessParams");

// GPU-written indirect args, one 64-byte block per tessellated mesh. Byte
// offsets consumed by dispatchIndirect / drawIndexedIndirect /
// drawMeshTasksIndirect — keep in lockstep with MSL TessArgs.
inline constexpr size_t kTessArgsClassifyOffset  = 0;   // uint3 dispatch
inline constexpr size_t kTessArgsLeafPrepOffset  = 12;  // uint3 dispatch
inline constexpr size_t kTessArgsDrawOffset      = 24;  // DrawCommand (5 uints)
inline constexpr size_t kTessArgsMeshTasksOffset = 44;  // uint3 task grid
inline constexpr size_t kTessArgsLeafCountOffset = 56;
inline constexpr size_t kTessArgsSize            = 64;

// Per-leaf corner cache entry (compute path), written by tessLeafPrep.
struct alignas(16) TessLeafDataGpu {
    glm::vec4 posU[3];
    glm::vec4 nrmV[3];
    Uint32 visible;
    Uint32 depth;
    Uint32 node;
    Uint32 pad;
};
static_assert(sizeof(TessLeafDataGpu) == 112, "must match MSL TessLeafData");

// ---- per-leaf grid (must match 3d_tess_lib.metal's TESS_GRID_*) ------------

inline constexpr Uint32 kTessGridSegs = 8;
inline constexpr Uint32 kTessGridVerts = 45;  // (S+1)(S+2)/2
inline constexpr Uint32 kTessGridTris = 64;   // S^2

// ---- creation parameters ---------------------------------------------------

struct TessellationDesc {
    // CBT max depth D: bitfield of 2^D bits (storage = 3 * 2^(D-5) - 1 words;
    // D=20 -> ~0.4 MB, D=24 -> ~6.3 MB). Effective subdivision on top of the
    // control mesh ~= (D - ceil(log2(3 * faceCount))) / 2 quadtree levels,
    // plus 3 more from the per-leaf 8x8 grid. Clamped to [rootDepth+2, 25]
    // (25: dyadic barycentrics stay fp32-exact).
    Uint32 maxDepth = 20;
    // Leaf corner-cache capacity — bounds instanced draws and the split
    // guard. 2^17 leaves * 64 grid tris = 8.4M on-screen triangles.
    Uint32 maxLeaves = 1u << 17;
    float displacementScale = 0.0f;
    glm::mat4 model = glm::mat4(1.0f);
};

// ---- builders (implemented in src/tessellation.cpp) ------------------------

// Fan-triangulate a mesh into LEB roots (3 per triangle, hypotenuses on mesh
// edges, corners = 2 mesh vertices + face centroid) with cross-face
// adjacency. Uses positions/normals/uv0 from the mesh's interleaved vertices.
std::vector<TessRootGpu> buildTessRoots(const Mesh& mesh);

// The canonical per-leaf grid topology, identical to the mesh shader's
// tessGridVertexRC/tessGridTriangle emission: verts are (w1, w2) barycentric
// pairs, triangles wound CCW in object space.
void buildTessGrid(std::vector<glm::vec2>& outVerts, std::vector<Uint32>& outIndices);

} // namespace Vapor
