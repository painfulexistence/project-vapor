#pragma once
#include "graphics.hpp"

// Offline meshlet + cluster-LOD bake for a mesh, using meshoptimizer +
// clusterlod.h. Runs as part of the optimized-scene bake; the result lives in
// Mesh::meshletData and is serialized into the .vscene_optimized cache.
class MeshletBuilder {
public:
    // Cluster size caps. 128 triangles keeps a meshlet within both Vulkan
    // (VK_EXT_mesh_shader) and Metal object/mesh limits; 64 verts is the common
    // hardware-friendly vertex cap.
    static constexpr Uint32 MAX_MESHLET_VERTICES  = 64;
    static constexpr Uint32 MAX_MESHLET_TRIANGLES = 128;

    // Build meshletData for one mesh (no-op if already built or empty). Fills
    // meshlets, meshletVertices, meshletTriangles, bounds, lodLevelCount.
    static void build(Vapor::Mesh& mesh);
};
