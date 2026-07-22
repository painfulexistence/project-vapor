#pragma once
#include <cassert>
#include <cstdint>
#include <bit>
#include <unordered_map>
#include <vector>

// ============================================================================
// CBT/LEB — Concurrent Binary Tree over Longest-Edge Bisection
//
// The data structure behind adaptive GPU tessellation (see the TessUpdate pass
// in the renderer and the mirrored MSL in assets/shaders/3d_tess_lib.metal):
// a binary heap over triangle bisections, stored as one uint32 buffer of
// per-node leaf counts on top of a bitfield. Splits/merges are single-bit
// writes (concurrent-safe via atomicOr/And on the GPU); a sum reduction
// rebuilds the counts, which give O(1) leaf totals and O(depth) leaf-index ->
// node decoding in heap (space-filling) order. Bisection triangulations are
// conforming by construction — adjacent leaves share complete edges and
// differ by at most one depth — so uniform per-leaf grids are crack-free with
// no T-junction machinery.
//
// This header is the CPU reference implementation: it is what the unit tests
// verify, what initializes the GPU buffer, and what the MSL must match
// bit-for-bit (same storage layout, same split/merge/neighbor rules).
//
// References: J. Dupuy, "Concurrent Binary Trees (with application to longest
// edge bisection)", HPG 2020 — the successor to the ping-pong linear-quadtree
// scheme of Khoury 2018.
// ============================================================================

namespace Vapor {
namespace leb {

// ----------------------------------------------------------------------------
// Node convention
//
// A node is a heap index: root = 1, children of k = {2k, 2k+1}, depth =
// floor(log2(k)). Its triangle is (v0, v1, v2) with the bisected edge (the
// "hypotenuse") = (v2, v0) and apex v1. Bisection at m = (v0 + v2)/2:
//   child taking bit 0 = (v0, m, v1)     (heap id 2k)
//   child taking bit 1 = (v1, m, v2)     (heap id 2k + 1)
// so the bit path below a node id's MSB is the split sequence from its root.
// ----------------------------------------------------------------------------

inline std::uint32_t depthOf(std::uint32_t node) {
    assert(node != 0);
    return std::uint32_t(std::bit_width(node)) - 1u;
}

// Same-depth neighbors of a node. Convention (edge roles, not screen sides):
//   left  = across edge (v1, v2)
//   right = across edge (v0, v1)
//   edge  = across the bisected edge (v2, v0)
// id 0 = no neighbor (domain boundary). `node` is the node's own id.
//
// Invariant the rules rely on (and the fan root builder establishes): with
// consistent winding, my `left` neighbor sees the shared edge as its `right`
// edge and vice versa, and bisected edges always pair with bisected edges
// (the diamond property of conforming bisection).
struct NeighborIDs {
    std::uint32_t left = 0, right = 0, edge = 0, node = 0;
};

// Neighbor evolution under one split step (Dupuy 2020, §neighborhoods).
// Null neighbors (0) propagate to 0 because 0 << 1 | (0 != 0) == 0.
inline NeighborIDs splitNeighborIDs(const NeighborIDs& n, std::uint32_t bit) {
    const std::uint32_t b2 = (n.right != 0) ? 1u : 0u;
    const std::uint32_t b3 = (n.edge != 0) ? 1u : 0u;
    if (bit == 0) {
        // child (v0, m, v1): left = sibling, right = edge-neighbor's bit-1
        // child, new bisected edge (v1, v0) faces the old right neighbor's
        // bit-1 child.
        return { n.node << 1 | 1, n.edge << 1 | b3, n.right << 1 | b2, n.node << 1 };
    } else {
        // child (v1, m, v2): left = edge-neighbor's bit-0 child, right =
        // sibling, new bisected edge (v2, v1) faces the old left neighbor's
        // bit-0 child; nulls propagate as 0 << 1 == 0.
        return { n.edge << 1, n.node << 1, n.left << 1, n.node << 1 | 1 };
    }
}

// Barycentric corner weights of a node's triangle w.r.t. its root triangle:
// row i = weights of corner vi over (root v0, root v1, root v2). All weights
// are dyadic rationals with denominator 2^lebDepth — exact in fp32 up to
// depth 24, which is why corner positions match bit-for-bit across leaves
// that share an edge (crack-free without epsilon snapping).
struct TriangleWeights {
    float w[3][3];
};

inline TriangleWeights rootTriangleWeights() {
    return { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
}

inline TriangleWeights splitTriangleWeights(const TriangleWeights& t, std::uint32_t bit) {
    TriangleWeights r;
    for (int c = 0; c < 3; ++c) {
        const float mid = (t.w[0][c] + t.w[2][c]) * 0.5f;
        if (bit == 0) {
            r.w[0][c] = t.w[0][c];
            r.w[1][c] = mid;
            r.w[2][c] = t.w[1][c];
        } else {
            r.w[0][c] = t.w[1][c];
            r.w[1][c] = mid;
            r.w[2][c] = t.w[2][c];
        }
    }
    return r;
}

} // namespace leb

// ----------------------------------------------------------------------------
// CBT storage
//
// One uint32 array shared verbatim with the GPU:
//   [ counts level 0 | counts level 1 | ... | counts level S | bitfield ]
// where S = maxDepth - 5 ("sum levels"). Level d holds 2^d uint32 counts at
// offset 2^d - 1 (index = node - 2^d). The bitfield holds 2^maxDepth bits =
// 2^S words; a node at depth d > S reads its count by masking + popcount of
// its ceil-bit range, which always fits one aligned 32-bit word.
//
// Tree encoding: each leaf marks exactly one ceil bit — the one at its
// leftmost depth-maxDepth descendant (node << (maxDepth - depth)). Counts are
// then subtree leaf totals; a node is a leaf iff its count is 1 and its
// parent's is greater (the decode descent finds leaves without needing the
// second check). Split of n = set the mark of its right child; merge of a
// sibling pair = clear the right sibling's mark. Both are single-bit ops.
//
// Multiple LEB roots ("bisection forest", used for fan-triangulated meshes)
// live at depth rootDepth = ceil(log2(rootCount)): root slot i is heap node
// (1 << rootDepth) + i; trailing slots without a root stay unmarked and are
// never visited (count 0).
// ----------------------------------------------------------------------------

class CBT {
public:
    static constexpr std::uint32_t kCollapse = 5;  // bottom levels served by the bitfield

    CBT(std::uint32_t maxDepth, std::uint32_t rootCount)
      : maxDepth_(maxDepth),
        rootDepth_(rootCount <= 1 ? 0 : std::uint32_t(std::bit_width(rootCount - 1))),
        rootCount_(rootCount) {
        assert(maxDepth >= kCollapse + 1 && maxDepth <= 25);  // 25: dyadic weights stay fp32-exact
        assert(rootCount >= 1 && rootDepth_ < maxDepth_);
        storage_.assign(storageWordCount(maxDepth), 0u);
        for (std::uint32_t i = 0; i < rootCount_; ++i) {
            setCeilBit(ceilBitOf((1u << rootDepth_) + i));
        }
        reduce();
    }

    static std::uint32_t storageWordCount(std::uint32_t maxDepth) {
        const std::uint32_t s = maxDepth - kCollapse;
        return 3u * (1u << s) - 1u;  // (2^(s+1) - 1) count words + 2^s bitfield words
    }

    std::uint32_t maxDepth() const { return maxDepth_; }
    std::uint32_t rootDepth() const { return rootDepth_; }
    std::uint32_t rootCount() const { return rootCount_; }
    const std::vector<std::uint32_t>& raw() const { return storage_; }

    std::uint32_t bitfieldOffset() const { return (2u << (maxDepth_ - kCollapse)) - 1u; }

    // Subtree leaf count of a node. Counts above the bitfield are only as
    // fresh as the last reduce().
    std::uint32_t heapRead(std::uint32_t node) const {
        if (node == 0) return 0;
        const std::uint32_t d = leb::depthOf(node);
        assert(d <= maxDepth_);
        const std::uint32_t s = maxDepth_ - kCollapse;
        if (d <= s) {
            return storage_[(1u << d) - 1u + (node - (1u << d))];
        }
        const std::uint32_t first = ceilBitOf(node);
        const std::uint32_t len = 1u << (maxDepth_ - d);  // <= 16, word-aligned range
        const std::uint32_t word = storage_[bitfieldOffset() + (first >> 5)];
        const std::uint32_t mask = ((len == 32u) ? ~0u : ((1u << len) - 1u)) << (first & 31u);
        return std::uint32_t(std::popcount(word & mask));
    }

    std::uint32_t leafCount() const { return storage_[0]; }

    // Rebuild all count levels from the bitfield (the GPU runs this as a
    // chain of tiny dispatches after the classify pass).
    void reduce() {
        const std::uint32_t s = maxDepth_ - kCollapse;
        const std::uint32_t bfOff = bitfieldOffset();
        const std::uint32_t lvlSOff = (1u << s) - 1u;
        for (std::uint32_t i = 0; i < (1u << s); ++i) {
            storage_[lvlSOff + i] = std::uint32_t(std::popcount(storage_[bfOff + i]));
        }
        for (std::uint32_t d = s; d-- > 0;) {
            const std::uint32_t off = (1u << d) - 1u;
            const std::uint32_t childOff = (2u << d) - 1u;
            for (std::uint32_t i = 0; i < (1u << d); ++i) {
                storage_[off + i] = storage_[childOff + 2 * i] + storage_[childOff + 2 * i + 1];
            }
        }
    }

    // leafIndex in [0, leafCount()) -> leaf node, in heap (space-filling)
    // order. The depth guard keeps the descent from stopping at an ancestor
    // of a lone root in a sparse forest.
    std::uint32_t decodeLeaf(std::uint32_t leafIndex) const {
        assert(leafIndex < leafCount());
        std::uint32_t node = 1;
        while (leb::depthOf(node) < rootDepth_ || heapRead(node) > 1) {
            const std::uint32_t leftCount = heapRead(node << 1);
            if (leafIndex < leftCount) {
                node = node << 1;
            } else {
                leafIndex -= leftCount;
                node = node << 1 | 1;
            }
        }
        return node;
    }

    bool isLeaf(std::uint32_t node) const {
        if (heapRead(node) != 1) return false;
        const std::uint32_t d = leb::depthOf(node);
        if (d < rootDepth_) return false;
        return d == rootDepth_ || heapRead(node >> 1) > 1;
    }

    // Same-depth neighborhood of a node, from its root's adjacency through
    // the split rules along the node's bit path. rootAdj is indexed by root
    // slot; ids inside are heap ids (of OTHER roots for cross-root edges).
    leb::NeighborIDs neighbors(std::uint32_t node, const std::vector<leb::NeighborIDs>& rootAdj) const {
        const std::uint32_t d = leb::depthOf(node);
        assert(d >= rootDepth_);
        const std::uint32_t lebDepth = d - rootDepth_;
        const std::uint32_t slot = (node >> lebDepth) - (1u << rootDepth_);
        leb::NeighborIDs ids = rootAdj[slot];
        for (std::uint32_t k = lebDepth; k-- > 0;) {
            ids = leb::splitNeighborIDs(ids, (node >> k) & 1u);
        }
        return ids;
    }

    // Barycentric corner weights of a node w.r.t. its root triangle, plus the
    // root slot to look the corners up with.
    leb::TriangleWeights decodeTriangle(std::uint32_t node, std::uint32_t* outRootSlot = nullptr) const {
        const std::uint32_t d = leb::depthOf(node);
        assert(d >= rootDepth_);
        const std::uint32_t lebDepth = d - rootDepth_;
        if (outRootSlot) *outRootSlot = (node >> lebDepth) - (1u << rootDepth_);
        leb::TriangleWeights t = leb::rootTriangleWeights();
        for (std::uint32_t k = lebDepth; k-- > 0;) {
            t = leb::splitTriangleWeights(t, (node >> k) & 1u);
        }
        return t;
    }

    // Conforming split: bisecting a node's hypotenuse forces the triangle
    // across it to bisect the same edge, which may require creating it first
    // by splitting its ancestors — the classic compatibility chain. All
    // writes are idempotent single-bit sets, so re-walking already-split
    // regions is harmless (and the GPU version is concurrent-safe).
    void splitConforming(std::uint32_t node, const std::vector<leb::NeighborIDs>& rootAdj) {
        if (leb::depthOf(node) >= maxDepth_) return;
        splitNode(node);
        std::uint32_t t = neighbors(node, rootAdj).edge;
        while (t != 0) {
            splitNode(t);
            // A root's hypotenuse partner is the node the chain just came
            // from (fan roots pair hypotenuse-to-hypotenuse) — already split.
            if (leb::depthOf(t) <= rootDepth_) break;
            t >>= 1;
            splitNode(t);
            t = neighbors(t, rootAdj).edge;
        }
    }

    // Merge this leaf's sibling pair if the containing diamond is minimal:
    // the parent holds exactly its two leaves and the parent's hypotenuse
    // partner holds AT MOST two (== 1 means the partner's pair merged
    // earlier — no dangling midpoint on its side; > 2 means it is still
    // subdivided on the shared edge and merging would leave a T-vertex).
    // The partner's own pair merges when its leaves classify — the LoD
    // metric lives on the shared hypotenuse, so both halves reach the same
    // decision. Returns whether the pair merged.
    bool mergeConforming(std::uint32_t leaf, const std::vector<leb::NeighborIDs>& rootAdj) {
        const std::uint32_t d = leb::depthOf(leaf);
        if (d <= rootDepth_) return false;
        const std::uint32_t parent = leaf >> 1;
        if (heapRead(parent) != 2) return false;
        const std::uint32_t top = neighbors(parent, rootAdj).edge;
        if (top != 0 && heapRead(top) > 2) return false;
        clearCeilBit(ceilBitOf(leaf | 1u));
        return true;
    }

    // Raw bit ops (the GPU uses atomicOr/And on the same words).
    void splitNode(std::uint32_t node) {
        const std::uint32_t d = leb::depthOf(node);
        if (d >= maxDepth_) return;
        setCeilBit(ceilBitOf(node << 1 | 1u));
    }

private:
    // Bitfield position of a node's leftmost depth-maxDepth descendant: shift
    // the id down to the ceil level, then drop the leading 1 (depth-maxDepth
    // ids live in [2^D, 2^(D+1)); the bitfield indexes their low D bits).
    std::uint32_t ceilBitOf(std::uint32_t node) const {
        const std::uint32_t d = leb::depthOf(node);
        return (node << (maxDepth_ - d)) & ((1u << maxDepth_) - 1u);
    }

    void setCeilBit(std::uint32_t pos) {
        storage_[bitfieldOffset() + (pos >> 5)] |= 1u << (pos & 31u);
    }
    void clearCeilBit(std::uint32_t pos) {
        storage_[bitfieldOffset() + (pos >> 5)] &= ~(1u << (pos & 31u));
    }

    std::uint32_t maxDepth_;
    std::uint32_t rootDepth_;
    std::uint32_t rootCount_;
    std::vector<std::uint32_t> storage_;
};

// ----------------------------------------------------------------------------
// Fan-root builder for triangle meshes
//
// Every mesh triangle (P0, P1, P2) becomes three LEB roots, one per mesh
// edge, meeting at the face centroid O:
//   root e = (v0 = P_{e+1}, v1 = O, v2 = P_e)
// so each root's hypotenuse (v2, v0) lies exactly on a mesh edge (traversed
// forward) and the triangle keeps the face's winding. Consequences:
//   - cross-face neighbors pair hypotenuse-to-hypotenuse (the LoD metric on
//     the shared edge is evaluated on identical endpoints from both sides),
//   - in-face fan neighbors pair edge1-to-edge0,
// which is exactly the invariant the neighbor split rules require.
// ----------------------------------------------------------------------------

struct TessRootTopology {
    // corner[i] = mesh vertex index of root corner vi; UINT32_MAX marks v1 =
    // the face centroid (not a mesh vertex).
    std::uint32_t corner[3];
    std::uint32_t face;  // source mesh triangle
    leb::NeighborIDs adjacency;
};

// indices: mesh triangle list (3 per face, consistent winding). Returns one
// entry per root, in slot order (face*3 + edge); adjacency ids are heap ids
// at rootDepth = ceil(log2(3 * faceCount)).
inline std::vector<TessRootTopology> buildFanRoots(const std::vector<std::uint32_t>& indices) {
    const std::uint32_t faceCount = std::uint32_t(indices.size() / 3);
    const std::uint32_t rootCount = faceCount * 3;
    const std::uint32_t rootDepth =
        rootCount <= 1 ? 0 : std::uint32_t(std::bit_width(rootCount - 1));
    const std::uint32_t rootBase = 1u << rootDepth;

    // Directed mesh edge (a -> b) -> root slot whose hypotenuse runs a -> b.
    // The cross-face partner traverses the shared edge in the opposite
    // direction (consistent winding), so it is found under the reversed key.
    std::unordered_map<std::uint64_t, std::uint32_t> edgeToSlot;
    edgeToSlot.reserve(rootCount);
    const auto edgeKey = [](std::uint32_t a, std::uint32_t b) {
        return (std::uint64_t(a) << 32) | b;
    };

    std::vector<TessRootTopology> roots(rootCount);
    for (std::uint32_t f = 0; f < faceCount; ++f) {
        for (std::uint32_t e = 0; e < 3; ++e) {
            const std::uint32_t a = indices[f * 3 + e];
            const std::uint32_t b = indices[f * 3 + (e + 1) % 3];
            const std::uint32_t slot = f * 3 + e;
            roots[slot].corner[0] = b;
            roots[slot].corner[1] = UINT32_MAX;
            roots[slot].corner[2] = a;
            roots[slot].face = f;
            roots[slot].adjacency.node = rootBase + slot;
            // In-face fan neighbors: edge1 (O, v2=P_e) pairs with the
            // previous fan root's edge0; edge0 (v0=P_{e+1}, O) with the next.
            roots[slot].adjacency.left = rootBase + f * 3 + (e + 2) % 3;
            roots[slot].adjacency.right = rootBase + f * 3 + (e + 1) % 3;
            edgeToSlot.emplace(edgeKey(a, b), slot);
        }
    }
    for (std::uint32_t slot = 0; slot < rootCount; ++slot) {
        const std::uint32_t a = roots[slot].corner[2];
        const std::uint32_t b = roots[slot].corner[0];
        const auto it = edgeToSlot.find(edgeKey(b, a));
        roots[slot].adjacency.edge = (it != edgeToSlot.end()) ? rootBase + it->second : 0u;
    }
    return roots;
}

} // namespace Vapor
