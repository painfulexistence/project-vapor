#include <catch2/catch_test_macros.hpp>
#include "Vapor/meshlet_builder.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/asset_serializer.hpp"
#include <cmath>
#include <sstream>
#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>

using namespace Vapor;

// A subdivided, gently bumpy grid: manifold and large enough that clusterlod
// produces several LOD levels.
static Mesh makeGrid(int N) {
    Mesh mesh;
    mesh.hasPosition = true;
    mesh.primitiveMode = PrimitiveMode::TRIANGLES;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            float fx = float(x) / N, fy = float(y) / N;
            float h = 0.06f * std::sin(fx * 12.0f) * std::cos(fy * 12.0f);
            VertexData v{};
            v.position = glm::vec3(fx - 0.5f, h, fy - 0.5f);
            v.normal = glm::vec3(0, 1, 0);
            mesh.vertices.push_back(v);
        }
    auto vid = [&](int x, int y) { return Uint32(y * (N + 1) + x); };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            mesh.indices.insert(mesh.indices.end(),
                { vid(x, y), vid(x + 1, y), vid(x + 1, y + 1),
                  vid(x, y), vid(x + 1, y + 1), vid(x, y + 1) });
        }
    return mesh;
}

TEST_CASE("MeshletBuilder - grid produces a valid cluster-LOD DAG", "[meshlet]") {
    Mesh mesh = makeGrid(160);              // 2*160*160 = 51200 triangles
    const long inputTris = long(mesh.indices.size() / 3);

    MeshletBuilder::build(mesh);
    const MeshletData& md = mesh.meshletData;

    REQUIRE(md.isBuilt());
    REQUIRE(md.meshlets.size() == md.bounds.size());
    REQUIRE(md.lodLevelCount >= 2);          // grid should simplify to multiple levels

    SECTION("meshlet-local indexing is valid and in range") {
        for (size_t i = 0; i < md.meshlets.size(); ++i) {
            const Meshlet& m = md.meshlets[i];
            REQUIRE(m.vertexCount <= MeshletBuilder::MAX_MESHLET_VERTICES);
            REQUIRE(m.triangleCount <= MeshletBuilder::MAX_MESHLET_TRIANGLES);
            REQUIRE(m.vertexOffset + m.vertexCount <= md.meshletVertices.size());
            REQUIRE(m.triangleOffset + m.triangleCount * 3 <= md.meshletTriangles.size());
            for (Uint32 t = 0; t < m.triangleCount * 3; ++t)
                REQUIRE(md.meshletTriangles[m.triangleOffset + t] < m.vertexCount);
            for (Uint32 v = 0; v < m.vertexCount; ++v)
                REQUIRE(md.meshletVertices[m.vertexOffset + v] < mesh.vertices.size());
        }
    }

    SECTION("level 0 reconstructs the full mesh (watertight base)") {
        long lvl0 = 0;
        for (size_t i = 0; i < md.meshlets.size(); ++i)
            if (md.bounds[i].depth == 0) lvl0 += md.meshlets[i].triangleCount;
        REQUIRE(lvl0 == inputTris);
    }

    SECTION("Nanite two-sphere cut is threshold-monotone") {
        auto selectTris = [&](float t) {
            long tris = 0;
            for (size_t i = 0; i < md.meshlets.size(); ++i) {
                const MeshletBounds& b = md.bounds[i];
                bool drawParent = b.parentError > t;                 // coarser too coarse
                bool drawThis   = (b.refined < 0) || (b.lodError <= t); // this level accurate enough
                if (drawParent && drawThis) tris += md.meshlets[i].triangleCount;
            }
            return tris;
        };
        REQUIRE(selectTris(0.0f) == inputTris);          // finest == full mesh
        REQUIRE(selectTris(1e9f) < inputTris);           // coarsest is a real reduction
        long prev = -1;
        for (float t : { 1e9f, 1.0f, 0.1f, 0.01f, 0.0f }) {
            long tris = selectTris(t);
            if (prev >= 0) REQUIRE(tris >= prev);        // tighter threshold -> not fewer tris
            prev = tris;
        }
    }
}

TEST_CASE("MeshletBuilder - degenerate inputs don't crash", "[meshlet]") {
    SECTION("empty mesh") {
        Mesh mesh;
        mesh.primitiveMode = PrimitiveMode::TRIANGLES;
        MeshletBuilder::build(mesh);
        REQUIRE_FALSE(mesh.meshletData.isBuilt());
    }
    SECTION("non-triangle topology is skipped") {
        Mesh mesh = makeGrid(4);
        mesh.primitiveMode = PrimitiveMode::LINES;
        MeshletBuilder::build(mesh);
        REQUIRE_FALSE(mesh.meshletData.isBuilt());
    }
}

TEST_CASE("MeshletData survives a cereal round-trip", "[meshlet][serializer]") {
    Mesh mesh = makeGrid(48);
    MeshletBuilder::build(mesh);
    REQUIRE(mesh.meshletData.isBuilt());

    std::stringstream ss;
    {
        cereal::BinaryOutputArchive out(ss);
        out(mesh.meshletData.meshlets, mesh.meshletData.meshletVertices,
            mesh.meshletData.meshletTriangles, mesh.meshletData.bounds,
            mesh.meshletData.lodLevelCount);
    }
    MeshletData rt;
    {
        cereal::BinaryInputArchive in(ss);
        in(rt.meshlets, rt.meshletVertices, rt.meshletTriangles, rt.bounds, rt.lodLevelCount);
    }
    REQUIRE(rt.meshlets.size() == mesh.meshletData.meshlets.size());
    REQUIRE(rt.meshletVertices == mesh.meshletData.meshletVertices);
    REQUIRE(rt.meshletTriangles == mesh.meshletData.meshletTriangles);
    REQUIRE(rt.bounds.size() == mesh.meshletData.bounds.size());
    REQUIRE(rt.lodLevelCount == mesh.meshletData.lodLevelCount);
    // spot-check a bound field survived
    REQUIRE(rt.bounds[0].cullSphere.w == mesh.meshletData.bounds[0].cullSphere.w);
}
