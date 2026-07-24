#include <catch2/catch_test_macros.hpp>
#include "Vapor/cbt.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

// ============================================================================
// CBT/LEB verification — brute-force geometric checks of the exact rules the
// GPU tessellation shaders mirror (3d_tess_lib.metal). Every check here is a
// property the renderer relies on:
//   - neighbor bit-rules == geometric adjacency (split propagation goes to
//     the right nodes, cross-root included)
//   - conforming splits produce watertight triangulations (crack-free
//     rendering without T-junction handling)
//   - decode enumeration is a bijection onto the leaves (indirect draws
//     cover the surface exactly once)
//   - diamond merges never deadlock and reach the fully-merged state
// Coordinates are dyadic rationals throughout, so all comparisons are exact.
// ============================================================================

using namespace Vapor;
using std::uint32_t;

namespace {

struct P2 {
    double x, y;
    bool operator<(const P2& o) const { return x < o.x || (x == o.x && y < o.y); }
    bool operator==(const P2& o) const { return x == o.x && y == o.y; }
};
using Tri = std::array<P2, 3>;

Tri nodeTriangle(const CBT& cbt, uint32_t node, const std::vector<Tri>& rootCorners) {
    uint32_t slot = 0;
    leb::TriangleWeights w = cbt.decodeTriangle(node, &slot);
    Tri out;
    for (int i = 0; i < 3; ++i) {
        out[i].x = double(w.w[i][0]) * rootCorners[slot][0].x +
                   double(w.w[i][1]) * rootCorners[slot][1].x +
                   double(w.w[i][2]) * rootCorners[slot][2].x;
        out[i].y = double(w.w[i][0]) * rootCorners[slot][0].y +
                   double(w.w[i][1]) * rootCorners[slot][1].y +
                   double(w.w[i][2]) * rootCorners[slot][2].y;
    }
    return out;
}

double triArea2(const Tri& t) {
    return (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
}

// Every same-depth neighbor id must point at the unique node geometrically
// sharing the corresponding edge (left = (v1,v2), right = (v0,v1),
// edge = (v2,v0)), across the whole forest at a uniform subdivision depth.
void checkNeighborsUniform(const CBT& cbt, const std::vector<leb::NeighborIDs>& adj,
                           const std::vector<Tri>& rootCorners, uint32_t lebDepth) {
    const uint32_t rootBase = 1u << cbt.rootDepth();
    std::vector<uint32_t> nodes;
    for (uint32_t s = 0; s < cbt.rootCount(); ++s)
        for (uint32_t n = (rootBase + s) << lebDepth; n < ((rootBase + s + 1u) << lebDepth); ++n)
            nodes.push_back(n);

    std::map<std::pair<P2, P2>, std::vector<uint32_t>> edgeMap;
    auto ekey = [](P2 a, P2 b) { return (b < a) ? std::make_pair(b, a) : std::make_pair(a, b); };
    std::map<uint32_t, Tri> tris;
    for (uint32_t n : nodes) {
        Tri t = nodeTriangle(cbt, n, rootCorners);
        tris[n] = t;
        for (int e = 0; e < 3; ++e) edgeMap[ekey(t[e], t[(e + 1) % 3])].push_back(n);
    }
    for (uint32_t n : nodes) {
        const Tri& t = tris[n];
        leb::NeighborIDs ids = cbt.neighbors(n, adj);
        REQUIRE(ids.node == n);
        const std::array<std::pair<P2, P2>, 3> edges = {
            std::make_pair(t[1], t[2]), std::make_pair(t[0], t[1]), std::make_pair(t[2], t[0])
        };
        const uint32_t got[3] = { ids.left, ids.right, ids.edge };
        for (int e = 0; e < 3; ++e) {
            auto& sharers = edgeMap[ekey(edges[e].first, edges[e].second)];
            uint32_t expect = 0;
            for (uint32_t s : sharers)
                if (s != n) expect = s;
            REQUIRE(got[e] == expect);
        }
    }
    // Pairing invariants the split rules assume: hypotenuses pair with
    // hypotenuses, left pairs with right.
    for (uint32_t n : nodes) {
        leb::NeighborIDs ids = cbt.neighbors(n, adj);
        if (ids.edge) REQUIRE(cbt.neighbors(ids.edge, adj).edge == n);
        if (ids.left) REQUIRE(cbt.neighbors(ids.left, adj).right == n);
        if (ids.right) REQUIRE(cbt.neighbors(ids.right, adj).left == n);
    }
}

// Watertightness: no leaf corner may lie strictly inside another leaf's edge
// (dyadic coordinates make the collinearity test exact).
void checkNoTJunctions(const CBT& cbt, const std::vector<Tri>& rootCorners) {
    std::vector<Tri> leaves;
    for (uint32_t i = 0; i < cbt.leafCount(); ++i)
        leaves.push_back(nodeTriangle(cbt, cbt.decodeLeaf(i), rootCorners));
    std::vector<P2> verts;
    for (auto& t : leaves)
        for (auto& p : t) verts.push_back(p);
    for (auto& v : verts) {
        for (auto& t : leaves) {
            for (int e = 0; e < 3; ++e) {
                P2 a = t[e], b = t[(e + 1) % 3];
                if (v == a || v == b) continue;
                const double cross = (b.x - a.x) * (v.y - a.y) - (v.x - a.x) * (b.y - a.y);
                if (cross != 0.0) continue;
                const double dot = (v.x - a.x) * (b.x - a.x) + (v.y - a.y) * (b.y - a.y);
                const double len2 = (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y);
                REQUIRE(!(dot > 0.0 && dot < len2));
            }
        }
    }
}

double totalArea2(const CBT& cbt, const std::vector<Tri>& rootCorners) {
    double a = 0;
    for (uint32_t i = 0; i < cbt.leafCount(); ++i)
        a += std::abs(triArea2(nodeTriangle(cbt, cbt.decodeLeaf(i), rootCorners)));
    return a;
}

void mergeToFixpoint(CBT& cbt, const std::vector<leb::NeighborIDs>& adj, uint32_t floor) {
    for (int guard = 0; guard < 64 && cbt.leafCount() > floor; ++guard) {
        for (uint32_t i = 0; i < cbt.leafCount(); ++i)
            cbt.mergeConforming(cbt.decodeLeaf(i), adj);
        cbt.reduce();
    }
}

} // namespace

TEST_CASE("CBT - single root", "[cbt]") {
    CBT cbt(12, 1);
    const std::vector<leb::NeighborIDs> adj = { { 0, 0, 0, 1 } };
    const std::vector<Tri> rc = { Tri{ P2{ 0, 0 }, P2{ 0, 1 }, P2{ 1, 0 } } };

    REQUIRE(cbt.rootDepth() == 0);
    REQUIRE(cbt.leafCount() == 1);
    REQUIRE(cbt.decodeLeaf(0) == 1);

    SECTION("neighbor bit-rules match geometric adjacency at uniform depths") {
        for (uint32_t d = 1; d <= 6; ++d) checkNeighborsUniform(cbt, adj, rc, d);
    }

    SECTION("triangle weights are exact barycentric partitions") {
        // children of any node tile it exactly; weights are dyadic, rows sum to 1
        for (uint32_t node = 1; node < 64; ++node) {
            leb::TriangleWeights w = cbt.decodeTriangle(node);
            for (int i = 0; i < 3; ++i)
                REQUIRE(w.w[i][0] + w.w[i][1] + w.w[i][2] == 1.0f);
        }
        Tri parent = nodeTriangle(cbt, 5, rc);
        Tri c0 = nodeTriangle(cbt, 10, rc);
        Tri c1 = nodeTriangle(cbt, 11, rc);
        REQUIRE(std::abs(triArea2(c0)) + std::abs(triArea2(c1)) == std::abs(triArea2(parent)));
        // both children share the bisection midpoint of the parent hypotenuse
        const P2 mid{ (parent[2].x + parent[0].x) / 2, (parent[2].y + parent[0].y) / 2 };
        REQUIRE(c0[1] == mid);
        REQUIRE(c1[1] == mid);
    }

    SECTION("random conforming splits stay watertight, then merge back") {
        std::mt19937 rng(7);
        for (int it = 0; it < 300; ++it) {
            cbt.splitConforming(cbt.decodeLeaf(rng() % cbt.leafCount()), adj);
            cbt.reduce();
        }
        REQUIRE(cbt.leafCount() > 200);

        // enumeration is a strictly ordered bijection onto the leaves
        uint32_t prevCeil = 0;
        for (uint32_t i = 0; i < cbt.leafCount(); ++i) {
            const uint32_t n = cbt.decodeLeaf(i);
            REQUIRE(cbt.isLeaf(n));
            const uint32_t ceilPos = n << (cbt.maxDepth() - leb::depthOf(n));
            REQUIRE((i == 0 || ceilPos > prevCeil));
            prevCeil = ceilPos;
        }
        // conformity: a missing same-depth neighbor is covered by a leaf
        // exactly one level coarser
        for (uint32_t i = 0; i < cbt.leafCount(); ++i) {
            const leb::NeighborIDs ids = cbt.neighbors(cbt.decodeLeaf(i), adj);
            for (uint32_t q : { ids.left, ids.right, ids.edge }) {
                if (!q || cbt.heapRead(q) != 0) continue;
                REQUIRE(cbt.heapRead(q >> 1) == 1);
            }
        }
        checkNoTJunctions(cbt, rc);
        REQUIRE(totalArea2(cbt, rc) == 1.0);  // exact area conservation

        mergeToFixpoint(cbt, adj, 1);
        REQUIRE(cbt.leafCount() == 1);  // diamond merges must not deadlock
    }
}

TEST_CASE("CBT - fan-root forest over a two-triangle square", "[cbt]") {
    // A quad split into two mesh triangles; buildFanRoots turns it into six
    // LEB roots meeting at the face centroids, with hypotenuses on the mesh
    // edges (cross-face pairs included).
    const std::vector<uint32_t> idx = { 0, 1, 2, 0, 2, 3 };
    // positions scaled by 3 so the centroids are exact integers
    const std::vector<P2> pos3 = { { 0, 0 }, { 3, 0 }, { 3, 3 }, { 0, 3 } };

    const auto roots = buildFanRoots(idx);
    REQUIRE(roots.size() == 6);

    std::vector<leb::NeighborIDs> adj;
    std::vector<Tri> rc;
    for (auto& r : roots) {
        adj.push_back(r.adjacency);
        P2 o{ 0, 0 };
        for (int k = 0; k < 3; ++k) {
            o.x += pos3[idx[r.face * 3 + k]].x;
            o.y += pos3[idx[r.face * 3 + k]].y;
        }
        o.x /= 3.0;
        o.y /= 3.0;
        Tri t;
        for (int i = 0; i < 3; ++i)
            t[i] = (r.corner[i] == UINT32_MAX) ? o : pos3[r.corner[i]];
        rc.push_back(t);
    }

    CBT cbt(14, 6);
    REQUIRE(cbt.rootDepth() == 3);
    REQUIRE(cbt.leafCount() == 6);

    SECTION("cross-root adjacency matches geometry at uniform depths") {
        for (uint32_t d = 1; d <= 5; ++d) checkNeighborsUniform(cbt, adj, rc, d);
    }

    SECTION("interior mesh edge pairs hypotenuse-to-hypotenuse") {
        int paired = 0;
        for (auto& r : roots) {
            if (r.adjacency.edge == 0) continue;
            ++paired;
        }
        REQUIRE(paired == 2);  // exactly the two roots on the shared diagonal
    }

    SECTION("random conforming splits across roots stay watertight, then merge back") {
        std::mt19937 rng(11);
        for (int it = 0; it < 400; ++it) {
            cbt.splitConforming(cbt.decodeLeaf(rng() % cbt.leafCount()), adj);
            cbt.reduce();
        }
        REQUIRE(cbt.leafCount() > 400);
        checkNoTJunctions(cbt, rc);
        REQUIRE(totalArea2(cbt, rc) == 18.0);  // 2x the 3x3 square's area

        mergeToFixpoint(cbt, adj, 6);
        REQUIRE(cbt.leafCount() == 6);
    }
}

TEST_CASE("CBT - storage layout invariants shared with the GPU", "[cbt]") {
    // The MSL mirrors these formulas verbatim; a change here without a shader
    // change (or vice versa) corrupts the tessellation.
    CBT cbt(12, 3);
    REQUIRE(CBT::storageWordCount(12) == 3u * (1u << 7) - 1u);
    REQUIRE(cbt.bitfieldOffset() == (2u << 7) - 1u);
    REQUIRE(cbt.raw().size() == CBT::storageWordCount(12));
    // level-0 count == root count at init, and heapRead agrees across the
    // sums/bitfield boundary after deep splits
    REQUIRE(cbt.leafCount() == 3);
    const std::vector<leb::NeighborIDs> adj = {
        { 0, 0, 0, 4 }, { 0, 0, 0, 5 }, { 0, 0, 0, 6 }
    };
    for (int i = 0; i < 40; ++i) {
        cbt.splitConforming(cbt.decodeLeaf(0), adj);
        cbt.reduce();
    }
    uint32_t sum = 0;
    for (uint32_t s = 0; s < 3; ++s) sum += cbt.heapRead(4 + s);
    REQUIRE(sum == cbt.leafCount());
}
