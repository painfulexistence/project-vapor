#pragma once
// ============================================================================
// Poolrooms — procedural mesh toolkit (engine-agnostic: glm + std only).
//
// Everything the scene is built from: beveled-tile panels with recessed grout
// channels, swept profiles (bullnose pool coping, concave cove trim), tube
// sweeps (ladder rails) and axis-aligned boxes/quads (plaster, corridor).
//
// Conventions:
//  * Right-handed, Y up. Front faces are CCW (the engine culls back faces).
//  * A "panel frame" is (origin, U, V): U runs along the panel width, V along
//    its height, and the outward normal is W = normalize(cross(U, V)).
//  * Tile-face UVs span [0,1] per tile cell, so a single-tile texture (the
//    swap-in slot for user tile textures) maps one tile per cell. Partial
//    edge cells get proportionally clipped UVs.
//  * Every emitted quad has non-zero UV area — the engine runs MikkTSpace on
//    import and throws on degenerate UV triangles.
// ============================================================================

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace poolgen {

struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;

    size_t vertexCount() const { return positions.size(); }
    size_t triangleCount() const { return indices.size() / 3; }
    bool empty() const { return indices.empty(); }

    void append(const MeshData& other) {
        const uint32_t base = static_cast<uint32_t>(positions.size());
        positions.insert(positions.end(), other.positions.begin(), other.positions.end());
        normals.insert(normals.end(), other.normals.begin(), other.normals.end());
        uvs.insert(uvs.end(), other.uvs.begin(), other.uvs.end());
        indices.reserve(indices.size() + other.indices.size());
        for (uint32_t i : other.indices) indices.push_back(base + i);
    }

    // a -> b -> c -> d CCW as seen from the normal side.
    void quad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
              const glm::vec3& n,
              const glm::vec2& uvA, const glm::vec2& uvB, const glm::vec2& uvC, const glm::vec2& uvD) {
        const uint32_t base = static_cast<uint32_t>(positions.size());
        positions.push_back(a); positions.push_back(b); positions.push_back(c); positions.push_back(d);
        normals.push_back(n); normals.push_back(n); normals.push_back(n); normals.push_back(n);
        uvs.push_back(uvA); uvs.push_back(uvB); uvs.push_back(uvC); uvs.push_back(uvD);
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    }
};

// Axis-aligned collision box, consumed by the physics setup in main.cpp.
struct CollisionBox {
    glm::vec3 center;
    glm::vec3 halfExtents;
};

// ── Tiled panel ─────────────────────────────────────────────────────────────

struct TilePanelDesc {
    float width = 1.0f;    // extent along U (meters)
    float height = 1.0f;   // extent along V
    float tileW = 0.20f;
    float tileH = 0.20f;
    float groutWidth = 0.010f;  // full gap between tile faces
    float groutDepth = 0.005f;  // grout floor recess below the tile surface
    float bevel = 0.007f;       // slope margin from tile face edge down to the grout floor
    // Offset the tile lattice (fraction of a tile, 0..1) so adjacent panels
    // can stagger like running bond if wanted.
    float uPhase = 0.0f;
    float vPhase = 0.0f;
};

// Emits a tiled panel into `tiles` (beveled tile faces) and `grout` (recessed
// channel floors). The panel spans origin .. origin + width*U + height*V, with
// the tile surface exactly on that plane and the grout sunk behind it.
inline void buildTilePanel(const glm::vec3& origin, glm::vec3 U, glm::vec3 V,
                           const TilePanelDesc& d, MeshData& tiles, MeshData& grout) {
    U = glm::normalize(U);
    V = glm::normalize(V);
    const glm::vec3 W = glm::normalize(glm::cross(U, V));
    auto P = [&](float u, float v, float w) { return origin + u * U + v * V + w * W; };

    const float g = d.groutWidth * 0.5f;   // half-gap on each side of a cell edge
    const float b = glm::max(d.bevel, 1e-4f);
    const float depth = glm::max(d.groutDepth, 1e-4f);

    const int nu = glm::max(1, static_cast<int>(std::ceil(d.width / d.tileW - 1e-4f)));
    const int nv = glm::max(1, static_cast<int>(std::ceil(d.height / d.tileH - 1e-4f)));

    const float u0Lattice = -d.uPhase * d.tileW;
    const float v0Lattice = -d.vPhase * d.tileH;

    for (int j = 0; j < nv + 1; ++j) {
        for (int i = 0; i < nu + 1; ++i) {
            // Cell bounds in panel space, clipped to the panel rectangle.
            const float cu0 = u0Lattice + i * d.tileW;
            const float cv0 = v0Lattice + j * d.tileH;
            const float cu1 = cu0 + d.tileW;
            const float cv1 = cv0 + d.tileH;
            const float u0 = glm::max(cu0, 0.0f), u1 = glm::min(cu1, d.width);
            const float v0 = glm::max(cv0, 0.0f), v1 = glm::min(cv1, d.height);
            if (u1 - u0 < 2.0f * (g + b) + 1e-4f) continue;
            if (v1 - v0 < 2.0f * (g + b) + 1e-4f) continue;

            // Tile footprint (grout gap removed) and face (bevel removed).
            const float fu0 = u0 + g, fu1 = u1 - g;
            const float fv0 = v0 + g, fv1 = v1 - g;
            const float tu0 = fu0 + b, tu1 = fu1 - b;
            const float tv0 = fv0 + b, tv1 = fv1 - b;

            // Cell-relative UV (0..1 across the unclipped cell, so partial
            // edge tiles read a clipped portion of the texture).
            auto cellUV = [&](float u, float v) {
                return glm::vec2((u - cu0) / d.tileW, (v - cv0) / d.tileH);
            };

            // Top face.
            tiles.quad(P(tu0, tv0, 0), P(tu1, tv0, 0), P(tu1, tv1, 0), P(tu0, tv1, 0), W,
                       cellUV(tu0, tv0), cellUV(tu1, tv0), cellUV(tu1, tv1), cellUV(tu0, tv1));

            // Bevel ring: slopes from the face edge (w=0) down to the
            // footprint edge on the grout floor (w=-depth). fa/fb: footprint
            // (lower, outer) edge; ta/tb: face (upper, inner) edge — wound so
            // the face is CCW seen from the panel's normal side.
            auto bevelQuad = [&](glm::vec3 fa, glm::vec3 fb, glm::vec3 ta, glm::vec3 tb,
                                 glm::vec2 uvFa, glm::vec2 uvFb, glm::vec2 uvTa, glm::vec2 uvTb) {
                const glm::vec3 n = glm::normalize(glm::cross(fb - fa, ta - fa));
                tiles.quad(fa, fb, tb, ta, n, uvFa, uvFb, uvTb, uvTa);
            };
            // south (v = fv0)
            bevelQuad(P(fu0, fv0, -depth), P(fu1, fv0, -depth), P(tu0, tv0, 0), P(tu1, tv0, 0),
                      cellUV(fu0, fv0), cellUV(fu1, fv0), cellUV(tu0, tv0), cellUV(tu1, tv0));
            // north (v = fv1)
            bevelQuad(P(fu1, fv1, -depth), P(fu0, fv1, -depth), P(tu1, tv1, 0), P(tu0, tv1, 0),
                      cellUV(fu1, fv1), cellUV(fu0, fv1), cellUV(tu1, tv1), cellUV(tu0, tv1));
            // west (u = fu0)
            bevelQuad(P(fu0, fv1, -depth), P(fu0, fv0, -depth), P(tu0, tv1, 0), P(tu0, tv0, 0),
                      cellUV(fu0, fv1), cellUV(fu0, fv0), cellUV(tu0, tv1), cellUV(tu0, tv0));
            // east (u = fu1)
            bevelQuad(P(fu1, fv0, -depth), P(fu1, fv1, -depth), P(tu1, tv0, 0), P(tu1, tv1, 0),
                      cellUV(fu1, fv0), cellUV(fu1, fv1), cellUV(tu1, tv0), cellUV(tu1, tv1));
        }
    }

    // Grout channel floors at w=-depth: vertical strips run the full panel
    // height; horizontal strips fill the gaps between them (no overlaps).
    // UVs in tile units so the grout texel density matches the tiles.
    auto groutQuad = [&](float u0, float v0, float u1, float v1) {
        if (u1 - u0 < 1e-5f || v1 - v0 < 1e-5f) return;
        grout.quad(P(u0, v0, -depth), P(u1, v0, -depth), P(u1, v1, -depth), P(u0, v1, -depth), W,
                   glm::vec2(u0 / d.tileW, v0 / d.tileH), glm::vec2(u1 / d.tileW, v0 / d.tileH),
                   glm::vec2(u1 / d.tileW, v1 / d.tileH), glm::vec2(u0 / d.tileW, v1 / d.tileH));
    };

    std::vector<float> uCuts;  // vertical grout strip spans [cut-g, cut+g]
    for (int i = 0; i <= nu + 1; ++i) {
        const float cu = u0Lattice + i * d.tileW;
        if (cu > -g && cu < d.width + g) uCuts.push_back(cu);
    }
    // Panel borders always get a half-strip so tiles never touch the edge raw.
    for (float cu : { 0.0f, d.width }) uCuts.push_back(cu);
    std::sort(uCuts.begin(), uCuts.end());
    uCuts.erase(std::unique(uCuts.begin(), uCuts.end(),
                            [](float a, float bb) { return std::abs(a - bb) < 1e-4f; }),
                uCuts.end());

    for (float cu : uCuts) {
        groutQuad(glm::max(cu - g, 0.0f), 0.0f, glm::min(cu + g, d.width), d.height);
    }
    std::vector<float> vCuts;
    for (int j = 0; j <= nv + 1; ++j) {
        const float cv = v0Lattice + j * d.tileH;
        if (cv > -g && cv < d.height + g) vCuts.push_back(cv);
    }
    for (float cv : { 0.0f, d.height }) vCuts.push_back(cv);
    std::sort(vCuts.begin(), vCuts.end());
    vCuts.erase(std::unique(vCuts.begin(), vCuts.end(),
                            [](float a, float bb) { return std::abs(a - bb) < 1e-4f; }),
                vCuts.end());

    for (float cv : vCuts) {
        const float sv0 = glm::max(cv - g, 0.0f), sv1 = glm::min(cv + g, d.height);
        // Fill horizontally between the vertical strips.
        float cursor = 0.0f;
        for (float cu : uCuts) {
            const float su0 = glm::max(cu - g, 0.0f), su1 = glm::min(cu + g, d.width);
            groutQuad(cursor, sv0, su0, sv1);
            cursor = su1;
        }
        groutQuad(cursor, sv0, d.width, sv1);
    }
}

// ── Path helpers ────────────────────────────────────────────────────────────

// Densify a straight segment (helps the tube's parallel transport stay stable
// through arcs that follow) — emits points from `from` to `to` inclusive.
inline void appendLine(std::vector<glm::vec3>& path, const glm::vec3& from, const glm::vec3& to,
                       int steps, bool includeStart) {
    for (int s = includeStart ? 0 : 1; s <= steps; ++s) {
        path.push_back(glm::mix(from, to, static_cast<float>(s) / steps));
    }
}

// 90° arc from center+startOffset to center+endOffset (both radius vectors),
// appending points EXCLUDING the start point.
inline void appendArc(std::vector<glm::vec3>& path, const glm::vec3& center,
                      const glm::vec3& startOffset, const glm::vec3& endOffset,
                      int steps) {
    for (int s = 1; s <= steps; ++s) {
        const float t = (glm::pi<float>() * 0.5f) * (static_cast<float>(s) / steps);
        path.push_back(center + startOffset * std::cos(t) + endOffset * std::sin(t));
    }
}

// ── Swept profiles (coping bullnose, cove trim) ─────────────────────────────

// A 2D profile point in the sweep frame: `side` runs along the profile's
// horizontal axis, `up` along vertical; `normal` is the outward surface
// normal in the same 2D frame. `vCoord` is the texture V along the profile.
struct ProfilePoint {
    glm::vec2 pos;     // (side, up)
    glm::vec2 normal;  // (side, up), normalized
    float vCoord;
};

// Sweep a profile along a polyline path. At each path vertex the frame is
// (sideDir, up, tangent): sideDir = normalize(cross(up, tangent)) — the
// profile's +side axis points to the LEFT of the travel direction.
// `uScale` converts path arc length into texture U (e.g. 1/tileSize).
// The path may be closed (last point joins the first).
inline void sweepProfile(const std::vector<glm::vec3>& path, bool closed,
                         const std::vector<ProfilePoint>& profile,
                         float uScale, MeshData& out) {
    const size_t n = path.size();
    if (n < 2 || profile.size() < 2) return;

    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const size_t rings = closed ? n + 1 : n;

    std::vector<float> arc(rings, 0.0f);
    auto pathAt = [&](size_t i) { return path[i % n]; };
    for (size_t i = 1; i < rings; ++i) {
        arc[i] = arc[i - 1] + glm::length(pathAt(i) - pathAt(i - 1));
    }

    const uint32_t base = static_cast<uint32_t>(out.positions.size());
    const uint32_t stride = static_cast<uint32_t>(profile.size());

    for (size_t i = 0; i < rings; ++i) {
        glm::vec3 tangent;
        if (closed) {
            tangent = glm::normalize(pathAt(i + 1) - pathAt(i + n - 1));
        } else if (i == 0) {
            tangent = glm::normalize(path[1] - path[0]);
        } else if (i == n - 1) {
            tangent = glm::normalize(path[n - 1] - path[n - 2]);
        } else {
            tangent = glm::normalize(path[i + 1] - path[i - 1]);
        }
        const glm::vec3 side = glm::normalize(glm::cross(up, tangent));

        for (const ProfilePoint& pp : profile) {
            out.positions.push_back(pathAt(i) + side * pp.pos.x + up * pp.pos.y);
            out.normals.push_back(glm::normalize(side * pp.normal.x + up * pp.normal.y));
            out.uvs.push_back(glm::vec2(arc[i] * uScale, pp.vCoord));
        }
    }

    for (size_t i = 0; i + 1 < rings; ++i) {
        for (uint32_t k = 0; k + 1 < stride; ++k) {
            const uint32_t a = base + static_cast<uint32_t>(i) * stride + k;
            const uint32_t b = a + stride;
            // Profile +side points LEFT of travel; with the profile winding
            // top->bottom this ordering faces outward (CCW from outside).
            out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(b + 1);
            out.indices.push_back(a); out.indices.push_back(b + 1); out.indices.push_back(a + 1);
        }
    }
}

// Bullnose coping profile: starts flush with the deck surface, rolls over a
// quarter-round of radius r and continues a short skirt down the pool's inner
// wall. Profile +side must face the POOL (sweep the rim counter-clockwise
// seen from above with the pool on the left).
inline std::vector<ProfilePoint> copingProfile(float r, float lip, float skirt, int segments = 6) {
    std::vector<ProfilePoint> pts;
    // Deck-side lip, flat on top.
    pts.push_back({ glm::vec2(-lip, 0.0f), glm::vec2(0.0f, 1.0f), 0.0f });
    pts.push_back({ glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 1.0f), 0.25f });
    // Quarter round from horizontal (top) to vertical (pool face).
    for (int s = 1; s <= segments; ++s) {
        const float a = (glm::pi<float>() * 0.5f) * (static_cast<float>(s) / segments);
        const glm::vec2 n(std::sin(a), std::cos(a));
        pts.push_back({ glm::vec2(std::sin(a) * r, -(1.0f - std::cos(a)) * r), n,
                        0.25f + 0.5f * (static_cast<float>(s) / segments) });
    }
    // Straight skirt down the inner wall.
    pts.push_back({ glm::vec2(r, -r - skirt), glm::vec2(1.0f, 0.0f), 1.0f });
    return pts;
}

// Concave cove trim where a wall meets the floor: quarter-round of radius r
// bridging from the wall face (top) to the floor (bottom). +side faces AWAY
// from the wall (sweep with the room on the left).
inline std::vector<ProfilePoint> coveProfile(float r, int segments = 5) {
    std::vector<ProfilePoint> pts;
    for (int s = 0; s <= segments; ++s) {
        const float a = (glm::pi<float>() * 0.5f) * (static_cast<float>(s) / segments);
        // From (0, r) at the wall down to (r, 0) at the floor, curving inward.
        pts.push_back({ glm::vec2(r - r * std::cos(a), r - r * std::sin(a)),
                        glm::vec2(std::cos(a), std::sin(a)),
                        static_cast<float>(s) / segments });
    }
    return pts;
}

// Rounded-rectangle rim path in the XZ plane at height y, ordered so the
// rectangle interior stays on the LEFT of travel (side = cross(up, tangent)
// points inward) — what the coping sweep expects. Each edge contributes its
// two endpoints and each corner a circular arc built from the two edge
// endpoints directly (P(t) = C + cos·(A−C) + sin·(B−C)), so no angle
// bookkeeping can go wrong.
inline std::vector<glm::vec3> roundedRectPathCCW(const glm::vec2& minXZ, const glm::vec2& maxXZ,
                                                 float cornerR, int cornerSegs, float y) {
    std::vector<glm::vec3> path;
    const float r = glm::max(cornerR, 1e-4f);
    auto P = [&](float x, float z) { return glm::vec3(x, y, z); };
    auto arc = [&](const glm::vec3& C, const glm::vec3& A, const glm::vec3& B) {
        // From A (exclusive) to B (inclusive) around C, 90°.
        appendArc(path, C, A - C, B - C, cornerSegs);
    };

    // Interior-on-the-left order: minX edge traveling +Z, maxZ edge +X,
    // maxX edge -Z, minZ edge -X (cross(Y, T) points into the rectangle).
    path.push_back(P(minXZ.x, minXZ.y + r));                                     // E1 start
    path.push_back(P(minXZ.x, maxXZ.y - r));                                     // E1 end
    arc(P(minXZ.x + r, maxXZ.y - r), path.back(), P(minXZ.x + r, maxXZ.y));      // corner -> E2
    path.push_back(P(maxXZ.x - r, maxXZ.y));                                     // E2 end
    arc(P(maxXZ.x - r, maxXZ.y - r), path.back(), P(maxXZ.x, maxXZ.y - r));      // corner -> E3
    path.push_back(P(maxXZ.x, minXZ.y + r));                                     // E3 end
    arc(P(maxXZ.x - r, minXZ.y + r), path.back(), P(maxXZ.x - r, minXZ.y));      // corner -> E4
    path.push_back(P(minXZ.x + r, minXZ.y));                                     // E4 end
    arc(P(minXZ.x + r, minXZ.y + r), path.back(), P(minXZ.x, minXZ.y + r));      // corner -> E1
    // The final arc point coincides with the first path point; drop it (the
    // sweep closes the loop itself).
    if (glm::length(path.back() - path.front()) < 1e-5f) path.pop_back();
    return path;
}

// ── Tubes and cylinders (ladder) ────────────────────────────────────────────

// Circular tube swept along a polyline with parallel-transported frames.
// UV: u wraps the circumference once, v = arc length / vTile.
inline void sweepTube(const std::vector<glm::vec3>& path, float radius, int sides,
                      float vTile, MeshData& out, bool capEnds = true) {
    const size_t n = path.size();
    if (n < 2 || sides < 3) return;

    // Parallel-transport frames along the path.
    std::vector<glm::vec3> tangents(n), sideAxes(n), upAxes(n);
    for (size_t i = 0; i < n; ++i) {
        if (i == 0) tangents[i] = glm::normalize(path[1] - path[0]);
        else if (i == n - 1) tangents[i] = glm::normalize(path[n - 1] - path[n - 2]);
        else tangents[i] = glm::normalize(path[i + 1] - path[i - 1]);
    }
    glm::vec3 ref = std::abs(tangents[0].y) < 0.95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    sideAxes[0] = glm::normalize(glm::cross(tangents[0], ref));
    upAxes[0] = glm::cross(sideAxes[0], tangents[0]);
    for (size_t i = 1; i < n; ++i) {
        const glm::vec3 prev = sideAxes[i - 1];
        glm::vec3 s = prev - tangents[i] * glm::dot(prev, tangents[i]);
        if (glm::dot(s, s) < 1e-10f) s = glm::cross(tangents[i], upAxes[i - 1]);
        sideAxes[i] = glm::normalize(s);
        upAxes[i] = glm::cross(sideAxes[i], tangents[i]);
    }

    std::vector<float> arc(n, 0.0f);
    for (size_t i = 1; i < n; ++i) arc[i] = arc[i - 1] + glm::length(path[i] - path[i - 1]);

    const uint32_t base = static_cast<uint32_t>(out.positions.size());
    const uint32_t stride = static_cast<uint32_t>(sides + 1);
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k <= sides; ++k) {
            const float a = 2.0f * glm::pi<float>() * (static_cast<float>(k) / sides);
            const glm::vec3 radial = sideAxes[i] * std::cos(a) + upAxes[i] * std::sin(a);
            out.positions.push_back(path[i] + radial * radius);
            out.normals.push_back(radial);
            out.uvs.push_back(glm::vec2(static_cast<float>(k) / sides, arc[i] / vTile));
        }
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        for (int k = 0; k < sides; ++k) {
            const uint32_t a = base + static_cast<uint32_t>(i) * stride + k;
            const uint32_t b = a + stride;
            // k walks side->up, which is clockwise seen from outside the
            // radial direction — wind against it so faces point outward.
            out.indices.push_back(a); out.indices.push_back(b + 1); out.indices.push_back(a + 1);
            out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(b + 1);
        }
    }

    if (capEnds) {
        auto cap = [&](size_t ring, const glm::vec3& n) {
            const uint32_t center = static_cast<uint32_t>(out.positions.size());
            out.positions.push_back(path[ring]);
            out.normals.push_back(n);
            out.uvs.push_back(glm::vec2(0.5f, 0.5f));
            for (int k = 0; k <= sides; ++k) {
                const float a = 2.0f * glm::pi<float>() * (static_cast<float>(k) / sides);
                const glm::vec3 radial = sideAxes[ring] * std::cos(a) + upAxes[ring] * std::sin(a);
                out.positions.push_back(path[ring] + radial * radius);
                out.normals.push_back(n);
                out.uvs.push_back(glm::vec2(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)));
            }
            // The ring parameter runs clockwise seen from +tangent, so the
            // end cap (n aligned with the tangent) fans in reverse.
            const bool reversed = glm::dot(n, tangents[ring]) > 0.0f;
            for (int k = 0; k < sides; ++k) {
                out.indices.push_back(center);
                if (reversed) {
                    out.indices.push_back(center + 2 + k);
                    out.indices.push_back(center + 1 + k);
                } else {
                    out.indices.push_back(center + 1 + k);
                    out.indices.push_back(center + 2 + k);
                }
            }
        };
        cap(0, -tangents[0]);
        cap(n - 1, tangents[n - 1]);
    }
}

// Straight cylinder between two points (ladder rungs).
inline void cylinderBetween(const glm::vec3& a, const glm::vec3& b, float radius, int sides,
                            MeshData& out) {
    sweepTube({ a, b }, radius, sides, glm::max(glm::length(b - a), 0.05f), out, true);
}

// Flip the winding and normals of every triangle appended after
// `firstIndex`/`firstVertex` — turns an outward-facing sweep into an
// inward-facing lining (slide interiors).
inline void flipAppended(MeshData& m, size_t firstVertex, size_t firstIndex) {
    for (size_t v = firstVertex; v < m.normals.size(); ++v) m.normals[v] = -m.normals[v];
    for (size_t i = firstIndex; i + 2 < m.indices.size(); i += 3) {
        std::swap(m.indices[i + 1], m.indices[i + 2]);
    }
}

// Flat annulus (washer) facing `normal`: connects an outer and inner circle in
// one plane — slide mouth rims. CCW seen from the normal side.
inline void annulus(const glm::vec3& center, glm::vec3 normal, float outerR, float innerR,
                    int segments, MeshData& out) {
    normal = glm::normalize(normal);
    const glm::vec3 ref = std::abs(normal.y) < 0.95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(ref, normal));
    const glm::vec3 v = glm::cross(normal, u);
    const uint32_t base = static_cast<uint32_t>(out.positions.size());
    for (int s = 0; s <= segments; ++s) {
        const float a = 2.0f * glm::pi<float>() * (static_cast<float>(s) / segments);
        const glm::vec3 radial = u * std::cos(a) + v * std::sin(a);
        out.positions.push_back(center + radial * outerR);
        out.positions.push_back(center + radial * innerR);
        for (int k = 0; k < 2; ++k) {
            out.normals.push_back(normal);
            out.uvs.push_back(glm::vec2(static_cast<float>(s) / segments, k ? 1.0f : 0.0f));
        }
    }
    for (int s = 0; s < segments; ++s) {
        const uint32_t o0 = base + s * 2, i0 = o0 + 1, o1 = o0 + 2, i1 = o0 + 3;
        // CCW seen from the +normal side (angle increases counter-clockwise
        // in the right-handed u,v basis about n).
        out.indices.push_back(o0); out.indices.push_back(i1); out.indices.push_back(i0);
        out.indices.push_back(o0); out.indices.push_back(o1); out.indices.push_back(i1);
    }
}

// Flat disc facing `normal` (triangle fan, concentric UVs) — porthole lamp
// faces and similar round fixtures.
inline void disc(const glm::vec3& center, glm::vec3 normal, float radius, int segments,
                 MeshData& out) {
    normal = glm::normalize(normal);
    const glm::vec3 ref = std::abs(normal.y) < 0.95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(ref, normal));
    const glm::vec3 v = glm::cross(normal, u);

    const uint32_t base = static_cast<uint32_t>(out.positions.size());
    out.positions.push_back(center);
    out.normals.push_back(normal);
    out.uvs.push_back(glm::vec2(0.5f, 0.5f));
    for (int s = 0; s <= segments; ++s) {
        const float a = 2.0f * glm::pi<float>() * (static_cast<float>(s) / segments);
        const glm::vec3 radial = u * std::cos(a) + v * std::sin(a);
        out.positions.push_back(center + radial * radius);
        out.normals.push_back(normal);
        out.uvs.push_back(glm::vec2(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)));
    }
    for (int s = 0; s < segments; ++s) {
        // CCW seen from the normal side: u x v basis is right-handed about it.
        out.indices.push_back(base);
        out.indices.push_back(base + 1 + s);
        out.indices.push_back(base + 2 + s);
    }
}

// Circular ring path (for torus-like fixture rims via sweepTube).
inline std::vector<glm::vec3> circlePath(const glm::vec3& center, const glm::vec3& normal,
                                         float radius, int segments) {
    const glm::vec3 n = glm::normalize(normal);
    const glm::vec3 ref = std::abs(n.y) < 0.95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(ref, n));
    const glm::vec3 v = glm::cross(n, u);
    std::vector<glm::vec3> path;
    for (int s = 0; s <= segments; ++s) {
        const float a = 2.0f * glm::pi<float>() * (static_cast<float>(s) / segments);
        path.push_back(center + (u * std::cos(a) + v * std::sin(a)) * radius);
    }
    return path;
}

// ── Plain surfaces (plaster ceiling, corridor, skylight wells) ──────────────

// Single rectangle in a panel frame with a flat-tiled UV (uvScale = texels per
// meter equivalent; here: how many UV repeats per meter).
inline void plainQuad(const glm::vec3& origin, glm::vec3 U, glm::vec3 V,
                      float width, float height, float uvScale, MeshData& out) {
    U = glm::normalize(U);
    V = glm::normalize(V);
    const glm::vec3 W = glm::normalize(glm::cross(U, V));
    out.quad(origin,
             origin + U * width,
             origin + U * width + V * height,
             origin + V * height,
             W,
             glm::vec2(0.0f, 0.0f), glm::vec2(width * uvScale, 0.0f),
             glm::vec2(width * uvScale, height * uvScale), glm::vec2(0.0f, height * uvScale));
}

}  // namespace poolgen
