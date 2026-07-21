// clusterlod's C-API implementation (CLUSTERLOD_IMPLEMENTATION) lives in the
// GLOBAL namespace with a `using namespace clod`, so inside it `Cluster` means
// clod::Cluster. It MUST be compiled before any Vapor header: several Vapor
// headers do a global `using namespace Vapor`, and main's Vapor::Cluster would
// then be equally visible, making `Cluster` ambiguous inside the clod impl
// (a cascade of parse errors in clusterlod.h). Keep this block first.
#include <meshoptimizer.h>
#define CLUSTERLOD_IMPLEMENTATION   // exactly one TU emits the clusterlod impl
#include "meshopt/clusterlod.h"

#include "meshlet_builder.hpp"

#include <algorithm>
#include <vector>

using namespace Vapor;

void MeshletBuilder::build(Mesh& mesh) {
    mesh.meshletData.clear();
    if (mesh.indices.empty() || mesh.vertices.empty()) return;
    // clusterlod needs triangle lists.
    if (mesh.primitiveMode != PrimitiveMode::TRIANGLES) return;

    clodMesh cm = {};
    cm.indices = mesh.indices.data();
    cm.index_count = mesh.indices.size();
    cm.vertex_count = mesh.vertices.size();
    // position is the first field of VertexData (glm::vec3), so &position.x is the
    // interleaved position stream.
    cm.vertex_positions = &mesh.vertices[0].position.x;
    cm.vertex_positions_stride = sizeof(VertexData);
    // No attribute-aware simplification for now (positions only); attribute
    // weights + protect mask are a later refinement for UV/normal seams.

    clodConfig config = clodDefaultConfig(MAX_MESHLET_TRIANGLES);
    config.max_vertices = MAX_MESHLET_VERTICES;

    MeshletData& md = mesh.meshletData;
    std::vector<clodBounds> groupSimplified;  // indexed by clod group id

    auto onGroup = [&](clodGroup group, const clodCluster* clusters, size_t count) -> int {
        const int groupId = static_cast<int>(groupSimplified.size());
        groupSimplified.push_back(group.simplified);
        md.lodLevelCount = std::max<Uint32>(md.lodLevelCount, static_cast<Uint32>(group.depth) + 1);

        for (size_t i = 0; i < count; ++i) {
            const clodCluster& c = clusters[i];

            // Meshlet-local indexing: meshletVertices[triangles[k]] == indices[k].
            std::vector<unsigned int> lv(c.index_count);
            std::vector<unsigned char> lt(c.index_count);
            const size_t uniq = clodLocalIndices(lv.data(), lt.data(), c.indices, c.index_count);

            Meshlet ml;
            ml.vertexOffset   = static_cast<Uint32>(md.meshletVertices.size());
            ml.triangleOffset = static_cast<Uint32>(md.meshletTriangles.size());
            ml.vertexCount    = static_cast<Uint32>(uniq);
            ml.triangleCount  = static_cast<Uint32>(c.index_count / 3);
            md.meshletVertices.insert(md.meshletVertices.end(), lv.begin(), lv.begin() + uniq);
            md.meshletTriangles.insert(md.meshletTriangles.end(), lt.begin(), lt.end());

            MeshletBounds b;
            b.cullSphere = glm::vec4(c.bounds.center[0], c.bounds.center[1], c.bounds.center[2], c.bounds.radius);

            // Backface cone from the cluster geometry.
            meshopt_Bounds mb = meshopt_computeClusterBounds(
                c.indices, c.index_count, cm.vertex_positions, cm.vertex_count, cm.vertex_positions_stride);
            b.coneApex       = glm::vec4(mb.cone_apex[0], mb.cone_apex[1], mb.cone_apex[2], 0.0f);
            b.coneAxisCutoff = glm::vec4(mb.cone_axis[0], mb.cone_axis[1], mb.cone_axis[2], mb.cone_cutoff);

            // Two-sphere LOD cut. parent = this group's simplified step (coarser).
            b.parentSphere = glm::vec4(group.simplified.center[0], group.simplified.center[1],
                                       group.simplified.center[2], group.simplified.radius);
            b.parentError  = group.simplified.error;
            b.group   = groupId;
            b.refined = c.refined;
            b.depth   = group.depth;
            // lod sphere/error come from the finer group (filled in a second pass);
            // original geometry (refined < 0) is exact.
            if (c.refined < 0) {
                b.lodSphere = b.cullSphere;
                b.lodError  = 0.0f;
            }

            md.bounds.push_back(b);
            md.meshlets.push_back(ml);
        }
        return groupId;
    };

    clodBuild(config, cm, onGroup);

    // Second pass: the finer group referenced by `refined` is known now.
    for (MeshletBounds& b : md.bounds) {
        if (b.refined >= 0 && static_cast<size_t>(b.refined) < groupSimplified.size()) {
            const clodBounds& s = groupSimplified[b.refined];
            b.lodSphere = glm::vec4(s.center[0], s.center[1], s.center[2], s.radius);
            b.lodError  = s.error;
        }
    }
}
