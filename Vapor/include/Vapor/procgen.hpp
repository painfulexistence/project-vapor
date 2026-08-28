#pragma once
// ============================================================================
// Vapor::procgen — procedural mesh toolkit (engine-agnostic: glm + std only).
//
// A small bake-time modeling kit: immediate-mode triangle emission into
// MeshData streams, frame-based generators, path builders, sweep/lathe/
// extrude operators, transform-append instancing, axis-aligned box CSG for
// blockouts, and a validation pass. Grown out of Examples/Poolrooms and
// promoted engine-side so scenes and tools can share one vocabulary.
//
// Conventions:
//  * Right-handed, Y up. Front faces are CCW (the engine culls back faces).
//  * A "panel frame" is (origin, U, V): U runs along the panel width, V along
//    its height, and the outward normal is W = normalize(cross(U, V)).
//  * Every emitted triangle should have non-zero UV area — Mesh::initialize()
//    runs MikkTSpace on import and throws on degenerate UV triangles.
//    validate() checks this, plus winding/normal agreement.
//  * Feed the result to Vapor::Mesh::initialize(vertices, indices) (see
//    Examples/Poolrooms/main.cpp for the 6-line conversion).
// ============================================================================

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Vapor {
namespace procgen {

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

// Axis-aligned collision box for the app's physics setup. Note that convex
// box shapes usually carry a convex radius (Jolt's default is 0.05) and the
// backend will reject a box whose smallest half-extent falls below it — clamp
// thin colliders before handing them over.
struct CollisionBox {
    glm::vec3 center;
    glm::vec3 halfExtents;
};

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
        auto cap = [&](size_t ring, const glm::vec3& capN) {
            const uint32_t center = static_cast<uint32_t>(out.positions.size());
            out.positions.push_back(path[ring]);
            out.normals.push_back(capN);
            out.uvs.push_back(glm::vec2(0.5f, 0.5f));
            for (int k = 0; k <= sides; ++k) {
                const float a = 2.0f * glm::pi<float>() * (static_cast<float>(k) / sides);
                const glm::vec3 radial = sideAxes[ring] * std::cos(a) + upAxes[ring] * std::sin(a);
                out.positions.push_back(path[ring] + radial * radius);
                out.normals.push_back(capN);
                out.uvs.push_back(glm::vec2(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)));
            }
            // The ring parameter runs clockwise seen from +tangent, so the
            // end cap (normal aligned with the tangent) fans in reverse.
            const bool reversed = glm::dot(capN, tangents[ring]) > 0.0f;
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


// ── Transform-append instancing ─────────────────────────────────────────────

// Append `src` into `dst` transformed by `xf` — the bake-time repetition
// system: build a fixture once, place it many times (optionally mirrored).
// Normals go through the inverse-transpose; a mirroring transform
// (det < 0) flips the triangle winding so faces keep pointing outward.
inline void appendTransformed(MeshData& dst, const MeshData& src, const glm::mat4& xf) {
    const uint32_t base = static_cast<uint32_t>(dst.positions.size());
    const glm::mat3 nrmXf = glm::inverseTranspose(glm::mat3(xf));
    const bool mirrored = glm::determinant(glm::mat3(xf)) < 0.0f;

    dst.positions.reserve(dst.positions.size() + src.positions.size());
    dst.normals.reserve(dst.normals.size() + src.normals.size());
    dst.uvs.insert(dst.uvs.end(), src.uvs.begin(), src.uvs.end());
    for (const glm::vec3& p : src.positions) {
        dst.positions.push_back(glm::vec3(xf * glm::vec4(p, 1.0f)));
    }
    for (const glm::vec3& n : src.normals) {
        dst.normals.push_back(glm::normalize(nrmXf * n));
    }
    dst.indices.reserve(dst.indices.size() + src.indices.size());
    for (size_t i = 0; i + 2 < src.indices.size(); i += 3) {
        if (mirrored) {
            dst.indices.push_back(base + src.indices[i + 0]);
            dst.indices.push_back(base + src.indices[i + 2]);
            dst.indices.push_back(base + src.indices[i + 1]);
        } else {
            dst.indices.push_back(base + src.indices[i + 0]);
            dst.indices.push_back(base + src.indices[i + 1]);
            dst.indices.push_back(base + src.indices[i + 2]);
        }
    }
}

// Frame placement helper: (origin, U, V) with W = cross(U, V) — the same
// convention every generator uses, as a matrix for appendTransformed.
inline glm::mat4 frameTransform(const glm::vec3& origin, glm::vec3 U, glm::vec3 V) {
    U = glm::normalize(U);
    V = glm::normalize(V);
    const glm::vec3 W = glm::normalize(glm::cross(U, V));
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(U, 0.0f);
    m[1] = glm::vec4(V, 0.0f);
    m[2] = glm::vec4(W, 0.0f);
    m[3] = glm::vec4(origin, 1.0f);
    return m;
}

// ── Lathe (surface of revolution) ───────────────────────────────────────────

// Revolve a 2D profile about the +Y axis. Profile points are (radius, height),
// ordered bottom-to-top for an outward-facing surface; per-point normals come
// from the polyline tangents. Rings at radius ~0 collapse into pole fans.
// UV: u wraps the revolution, v runs along the profile arc length.
inline void lathe(const std::vector<glm::vec2>& profile, int segments, MeshData& out) {
    const size_t np = profile.size();
    if (np < 2 || segments < 3) return;
    const float poleEps = 1e-5f;

    // 2D profile normals (outward: for a wall going up, n = (+1, 0)).
    std::vector<glm::vec2> n2(np);
    for (size_t i = 0; i < np; ++i) {
        glm::vec2 t;
        if (i == 0) t = profile[1] - profile[0];
        else if (i == np - 1) t = profile[np - 1] - profile[np - 2];
        else t = profile[i + 1] - profile[i - 1];
        t = glm::normalize(t);
        n2[i] = glm::vec2(t.y, -t.x);
    }
    std::vector<float> arc(np, 0.0f);
    for (size_t i = 1; i < np; ++i) arc[i] = arc[i - 1] + glm::length(profile[i] - profile[i - 1]);
    const float totalArc = glm::max(arc.back(), 1e-6f);

    // Ring vertices (poles emit a single center vertex per segment fan later).
    std::vector<uint32_t> ringStart(np);
    for (size_t i = 0; i < np; ++i) {
        ringStart[i] = static_cast<uint32_t>(out.positions.size());
        if (profile[i].x < poleEps) {
            out.positions.push_back(glm::vec3(0.0f, profile[i].y, 0.0f));
            out.normals.push_back(glm::normalize(glm::vec3(0.0f, n2[i].y >= 0.0f ? 1.0f : -1.0f, 0.0f)));
            out.uvs.push_back(glm::vec2(0.5f, arc[i] / totalArc));
            continue;
        }
        for (int s = 0; s <= segments; ++s) {
            const float a = 2.0f * glm::pi<float>() * (static_cast<float>(s) / segments);
            const float ca = std::cos(a), sa = std::sin(a);
            out.positions.push_back(glm::vec3(profile[i].x * ca, profile[i].y, profile[i].x * sa));
            out.normals.push_back(glm::normalize(glm::vec3(n2[i].x * ca, n2[i].y, n2[i].x * sa)));
            out.uvs.push_back(glm::vec2(static_cast<float>(s) / segments, arc[i] / totalArc));
        }
    }

    // Bands. Increasing angle runs x -> z, which is CLOCKWISE seen from
    // outside the surface — wind against it, exactly like sweepTube.
    for (size_t i = 0; i + 1 < np; ++i) {
        const bool pole0 = profile[i].x < poleEps;
        const bool pole1 = profile[i + 1].x < poleEps;
        if (pole0 && pole1) continue;
        if (pole0) {  // fan from the bottom pole up to ring i+1
            for (int s = 0; s < segments; ++s) {
                out.indices.push_back(ringStart[i]);
                out.indices.push_back(ringStart[i + 1] + s);
                out.indices.push_back(ringStart[i + 1] + s + 1);
            }
        } else if (pole1) {  // fan from ring i up to the top pole
            for (int s = 0; s < segments; ++s) {
                out.indices.push_back(ringStart[i + 1]);
                out.indices.push_back(ringStart[i] + s + 1);
                out.indices.push_back(ringStart[i] + s);
            }
        } else {
            for (int s = 0; s < segments; ++s) {
                const uint32_t a = ringStart[i] + s;
                const uint32_t b = ringStart[i + 1] + s;
                out.indices.push_back(a); out.indices.push_back(b + 1); out.indices.push_back(a + 1);
                out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(b + 1);
            }
        }
    }
}

// ── Polygon triangulation (ear clipping, holes via bridging) ────────────────

namespace detail {

inline float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

inline float signedArea(const std::vector<glm::vec2>& ring) {
    float a = 0.0f;
    for (size_t i = 0; i < ring.size(); ++i) {
        const glm::vec2& p = ring[i];
        const glm::vec2& q = ring[(i + 1) % ring.size()];
        a += cross2(p, q);
    }
    return 0.5f * a;
}

}  // namespace detail

// Triangulate a simple polygon (outer ring, any winding) with optional holes
// (any winding; holes must not cross the outer ring or each other, but MAY
// touch it or share coordinate rows/columns — the layouts architecture
// actually produces). Scanline trapezoid decomposition: the plane is cut at
// every distinct vertex Y, each slab's even-odd material spans become
// trapezoids, and ring vertices lying on a cut line are welded into BOTH
// adjacent slabs, so the result is crack-free (no T-junctions) with every
// triangle CCW. Emits more triangles than an optimal ear clip would — the
// price of shrugging off inputs that wedge ear clippers; a bake-time tool,
// not a runtime one. Returns triangle indices into `outVerts` (freshly
// built; exactly-shared vertices are welded to one index).
inline std::vector<uint32_t> triangulatePolygon(std::vector<glm::vec2> outer,
                                                const std::vector<std::vector<glm::vec2>>& holes,
                                                std::vector<glm::vec2>& outVerts) {
    std::vector<uint32_t> tris;
    outVerts.clear();
    if (outer.size() < 3) return tris;

    std::vector<std::vector<glm::vec2>> rings;
    rings.push_back(std::move(outer));
    for (const auto& h : holes) {
        if (h.size() >= 3) rings.push_back(h);
    }

    // Non-horizontal edges (horizontals lie exactly on a cut line and never
    // cross a slab interior), plus the cut lines and per-line vertex lists.
    struct Edge { glm::vec2 lo, hi; };  // lo.y < hi.y
    std::vector<Edge> edges;
    std::vector<float> ys;
    for (const auto& r : rings) {
        for (size_t i = 0; i < r.size(); ++i) {
            const glm::vec2 p = r[i], q = r[(i + 1) % r.size()];
            ys.push_back(p.y);
            if (p.y == q.y) continue;
            edges.push_back(p.y < q.y ? Edge{ p, q } : Edge{ q, p });
        }
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    if (ys.size() < 2) return tris;

    // Original vertices grouped by cut line, x-sorted: candidates for welding
    // into trapezoid edges that pass through them.
    std::vector<std::vector<float>> lineXs(ys.size());
    for (const auto& r : rings) {
        for (const glm::vec2& p : r) {
            const size_t li = static_cast<size_t>(
                std::lower_bound(ys.begin(), ys.end(), p.y) - ys.begin());
            if (li < ys.size() && ys[li] == p.y) lineXs[li].push_back(p.x);
        }
    }
    for (auto& xs : lineXs) std::sort(xs.begin(), xs.end());

    // Weld exactly-equal coordinates to one output vertex. Every x on a cut
    // line is either an original vertex coordinate or comes from the same
    // interpolation with the same inputs, so bitwise equality is exact.
    std::unordered_map<uint64_t, uint32_t> weld;
    auto vertexIndex = [&](float x, float y) -> uint32_t {
        const uint64_t key = (uint64_t(std::bit_cast<uint32_t>(x + 0.0f)) << 32) |
                             std::bit_cast<uint32_t>(y + 0.0f);
        auto it = weld.find(key);
        if (it != weld.end()) return it->second;
        const uint32_t idx = static_cast<uint32_t>(outVerts.size());
        outVerts.push_back(glm::vec2(x, y));
        weld.emplace(key, idx);
        return idx;
    };
    auto emitTri = [&](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
        if (detail::cross2(b - a, c - a) <= 1e-12f) return;  // degenerate sliver
        tris.push_back(vertexIndex(a.x, a.y));
        tris.push_back(vertexIndex(b.x, b.y));
        tris.push_back(vertexIndex(c.x, c.y));
    };

    // Because cut lines sit at every vertex, an edge crossing a slab's middle
    // spans the whole slab; clamping returns exact endpoint coordinates.
    auto xAt = [](const Edge& e, float y) {
        if (y <= e.lo.y) return e.lo.x;
        if (y >= e.hi.y) return e.hi.x;
        return e.lo.x + (y - e.lo.y) / (e.hi.y - e.lo.y) * (e.hi.x - e.lo.x);
    };

    struct Cross { float xm, x0, x1; };
    std::vector<Cross> xs;
    std::vector<glm::vec2> B, T;  // bottom/top chains of one trapezoid
    for (size_t s = 0; s + 1 < ys.size(); ++s) {
        const float y0 = ys[s], y1 = ys[s + 1];
        const float ym = 0.5f * (y0 + y1);
        xs.clear();
        for (const Edge& e : edges) {
            if (e.lo.y > ym || e.hi.y < ym) continue;
            xs.push_back({ xAt(e, ym), xAt(e, y0), xAt(e, y1) });
        }
        std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) {
            if (a.xm != b.xm) return a.xm < b.xm;
            if (a.x0 != b.x0) return a.x0 < b.x0;
            return a.x1 < b.x1;
        });
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {  // even-odd pairing
            const Cross& L = xs[k];
            const Cross& R = xs[k + 1];
            if (R.x0 - L.x0 <= 1e-7f && R.x1 - L.x1 <= 1e-7f) continue;  // zero-width channel

            // Chains: trapezoid corners plus any ring vertex sitting on the
            // open span of the bottom/top edge (that vertex is a trapezoid
            // corner in the neighboring slab — welding it here keeps the two
            // slabs crack-free).
            B.clear();
            T.clear();
            B.push_back(glm::vec2(L.x0, y0));
            for (float x : lineXs[s]) {
                if (x > L.x0 && x < R.x0) B.push_back(glm::vec2(x, y0));
            }
            B.push_back(glm::vec2(R.x0, y0));
            T.push_back(glm::vec2(L.x1, y1));
            for (float x : lineXs[s + 1]) {
                if (x > L.x1 && x < R.x1) T.push_back(glm::vec2(x, y1));
            }
            T.push_back(glm::vec2(R.x1, y1));

            // Strip between the chains, CCW (y1 > y0).
            size_t i = 0, j = 0;
            while (i + 1 < B.size() || j + 1 < T.size()) {
                bool advanceB;
                if (j + 1 >= T.size()) advanceB = true;
                else if (i + 1 >= B.size()) advanceB = false;
                else advanceB = B[i + 1].x <= T[j + 1].x;
                if (advanceB) {
                    emitTri(B[i], B[i + 1], T[j]);
                    ++i;
                } else {
                    emitTri(B[i], T[j + 1], T[j]);
                    ++j;
                }
            }
        }
    }
    return tris;
}

// Planar polygon face (with holes) in a panel frame: 2D points map to
// origin + p.x*U + p.y*V, normal = cross(U, V), uv = p * uvScale.
inline void polygonCap(const glm::vec3& origin, glm::vec3 U, glm::vec3 V,
                       const std::vector<glm::vec2>& outer,
                       const std::vector<std::vector<glm::vec2>>& holes,
                       float uvScale, MeshData& out) {
    U = glm::normalize(U);
    V = glm::normalize(V);
    const glm::vec3 W = glm::normalize(glm::cross(U, V));
    std::vector<glm::vec2> verts2;
    std::vector<uint32_t> tris = triangulatePolygon(outer, holes, verts2);
    const uint32_t base = static_cast<uint32_t>(out.positions.size());
    for (const glm::vec2& p : verts2) {
        out.positions.push_back(origin + U * p.x + V * p.y);
        out.normals.push_back(W);
        out.uvs.push_back(p * uvScale);
    }
    for (uint32_t i : tris) out.indices.push_back(base + i);
}

// Prism: polygon cap extruded along +W by `depth` — front cap, back cap and
// side walls (side UV: u = ring arc length * uvScale, v = depth * uvScale).
inline void extrudePolygon(const glm::vec3& origin, glm::vec3 U, glm::vec3 V,
                           const std::vector<glm::vec2>& outer,
                           const std::vector<std::vector<glm::vec2>>& holes,
                           float depth, float uvScale, MeshData& out) {
    U = glm::normalize(U);
    V = glm::normalize(V);
    const glm::vec3 W = glm::normalize(glm::cross(U, V));
    // Caps: triangulate once, emit twice — the front face at +W*depth keeps
    // the CCW winding (normal +W); the back face at the origin reuses the
    // same triangles with reversed winding (normal -W).
    std::vector<glm::vec2> verts2;
    const std::vector<uint32_t> tris = triangulatePolygon(outer, holes, verts2);
    const uint32_t frontBase = static_cast<uint32_t>(out.positions.size());
    for (const glm::vec2& p : verts2) {
        out.positions.push_back(origin + W * depth + U * p.x + V * p.y);
        out.normals.push_back(W);
        out.uvs.push_back(p * uvScale);
    }
    for (uint32_t i : tris) out.indices.push_back(frontBase + i);
    const uint32_t backBase = static_cast<uint32_t>(out.positions.size());
    for (const glm::vec2& p : verts2) {
        out.positions.push_back(origin + U * p.x + V * p.y);
        out.normals.push_back(-W);
        out.uvs.push_back(p * uvScale);
    }
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        out.indices.push_back(backBase + tris[i + 0]);
        out.indices.push_back(backBase + tris[i + 2]);
        out.indices.push_back(backBase + tris[i + 1]);
    }
    auto walls = [&](const std::vector<glm::vec2>& ring, bool holeRing) {
        if (ring.size() < 3) return;
        std::vector<glm::vec2> r = ring;
        const bool ccw = detail::signedArea(r) > 0.0f;
        if (holeRing == ccw) std::reverse(r.begin(), r.end());  // outer CCW, holes CW
        float arcLen = 0.0f;
        for (size_t i = 0; i < r.size(); ++i) {
            const glm::vec2 p2 = r[i];
            const glm::vec2 q2 = r[(i + 1) % r.size()];
            const glm::vec3 p = origin + U * p2.x + V * p2.y;
            const glm::vec3 q = origin + U * q2.x + V * q2.y;
            const glm::vec3 e = q - p;
            const glm::vec3 n = glm::normalize(glm::cross(e, W));
            const float len = glm::length(e);
            out.quad(p, q, q + W * depth, p + W * depth, n,
                     glm::vec2(arcLen * uvScale, 0.0f),
                     glm::vec2((arcLen + len) * uvScale, 0.0f),
                     glm::vec2((arcLen + len) * uvScale, depth * uvScale),
                     glm::vec2(arcLen * uvScale, depth * uvScale));
            arcLen += len;
        }
    };
    walls(outer, false);
    for (const auto& h : holes) walls(h, true);
}

// ── Axis-aligned box CSG (blockout tool) ────────────────────────────────────

// Solid = a list of non-overlapping boxes. Union appends; subtract splits.
// The Atmospheric-style blockout workflow: describe rooms/corridors as boxes
// minus doorways, emit render quads AND colliders from the same list.
struct Box3 {
    glm::vec3 min;
    glm::vec3 max;
    bool valid() const {
        return min.x < max.x - 1e-6f && min.y < max.y - 1e-6f && min.z < max.z - 1e-6f;
    }
};

// Subtract `cutter` from every box in `solid` (splitting into up to 6
// remainder boxes each).
inline void subtractBox(std::vector<Box3>& solid, const Box3& cutter) {
    std::vector<Box3> result;
    result.reserve(solid.size() + 4);
    for (const Box3& b : solid) {
        const glm::vec3 imin = glm::max(b.min, cutter.min);
        const glm::vec3 imax = glm::min(b.max, cutter.max);
        if (!(imin.x < imax.x && imin.y < imax.y && imin.z < imax.z)) {
            result.push_back(b);  // no overlap
            continue;
        }
        // Slabs on each side of the intersection, X then Y then Z.
        Box3 r;
        r = { b.min, glm::vec3(imin.x, b.max.y, b.max.z) };                 if (r.valid()) result.push_back(r);
        r = { glm::vec3(imax.x, b.min.y, b.min.z), b.max };                 if (r.valid()) result.push_back(r);
        r = { glm::vec3(imin.x, b.min.y, b.min.z),
              glm::vec3(imax.x, imin.y, b.max.z) };                          if (r.valid()) result.push_back(r);
        r = { glm::vec3(imin.x, imax.y, b.min.z),
              glm::vec3(imax.x, b.max.y, b.max.z) };                         if (r.valid()) result.push_back(r);
        r = { glm::vec3(imin.x, imin.y, b.min.z),
              glm::vec3(imax.x, imax.y, imin.z) };                           if (r.valid()) result.push_back(r);
        r = { glm::vec3(imin.x, imin.y, imax.z),
              glm::vec3(imax.x, imax.y, b.max.z) };                          if (r.valid()) result.push_back(r);
    }
    solid = std::move(result);
}

// Emit the boxes as quads (UV in world units * uvScale). Faces exactly
// covered by a sibling box are skipped, so decompositions from subtractBox
// don't draw their internal seams.
inline void emitBoxes(const std::vector<Box3>& solid, float uvScale, MeshData& out) {
    auto covered = [&](const Box3& self, int axis, bool positive) {
        const float plane = positive ? self.max[axis] : self.min[axis];
        const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        for (const Box3& o : solid) {
            if (&o == &self) continue;
            const float oppo = positive ? o.min[axis] : o.max[axis];
            if (std::abs(oppo - plane) > 1e-5f) continue;
            if (o.min[a1] <= self.min[a1] + 1e-5f && o.max[a1] >= self.max[a1] - 1e-5f &&
                o.min[a2] <= self.min[a2] + 1e-5f && o.max[a2] >= self.max[a2] - 1e-5f) {
                return true;
            }
        }
        return false;
    };
    for (const Box3& b : solid) {
        const glm::vec3 c0 = b.min, c1 = b.max;
        const glm::vec3 d = c1 - c0;
        struct Face { glm::vec3 o, u, v; int axis; bool pos; };
        // Each face's frame satisfies cross(u, v) = outward axis.
        const Face faces[6] = {
            { { c1.x, c0.y, c0.z }, { 0, d.y, 0 }, { 0, 0, d.z }, 0, true },   // +X
            { { c0.x, c0.y, c0.z }, { 0, 0, d.z }, { 0, d.y, 0 }, 0, false },  // -X
            { { c0.x, c1.y, c0.z }, { 0, 0, d.z }, { d.x, 0, 0 }, 1, true },   // +Y
            { { c0.x, c0.y, c0.z }, { d.x, 0, 0 }, { 0, 0, d.z }, 1, false },  // -Y
            { { c0.x, c0.y, c1.z }, { d.x, 0, 0 }, { 0, d.y, 0 }, 2, true },   // +Z
            { { c0.x, c0.y, c0.z }, { 0, d.y, 0 }, { d.x, 0, 0 }, 2, false },  // -Z
        };
        for (const Face& f : faces) {
            if (covered(b, f.axis, f.pos)) continue;
            const glm::vec3 n = glm::normalize(glm::cross(f.u, f.v));
            const float lu = glm::length(f.u), lv = glm::length(f.v);
            out.quad(f.o, f.o + f.u, f.o + f.u + f.v, f.o + f.v, n,
                     glm::vec2(0, 0), glm::vec2(lu * uvScale, 0),
                     glm::vec2(lu * uvScale, lv * uvScale), glm::vec2(0, lv * uvScale));
        }
    }
}

inline void boxesToColliders(const std::vector<Box3>& solid, std::vector<CollisionBox>& out) {
    for (const Box3& b : solid) {
        out.push_back({ (b.min + b.max) * 0.5f, (b.max - b.min) * 0.5f });
    }
}

// ── Validation ──────────────────────────────────────────────────────────────

// Bake-time integrity report: OOB indices, non-finite values, degenerate
// geometry/UV triangles (MikkTSpace throws on the latter), and triangles
// whose winding disagrees with their vertex normals. ok() gates a build.
struct ValidationReport {
    size_t triangles = 0;
    size_t outOfBoundsIndices = 0;
    size_t nonFinite = 0;
    size_t degenerateGeo = 0;
    size_t degenerateUV = 0;
    size_t windingMismatch = 0;
    bool ok() const {
        return outOfBoundsIndices == 0 && nonFinite == 0 && degenerateGeo == 0 &&
               degenerateUV == 0 && windingMismatch == 0;
    }
};

inline ValidationReport validate(const MeshData& m, bool checkWinding = true) {
    ValidationReport r;
    if (m.positions.size() != m.normals.size() || m.positions.size() != m.uvs.size()) {
        r.nonFinite = size_t(-1);  // stream mismatch: flag hard
        return r;
    }
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        ++r.triangles;
        const uint32_t ia = m.indices[i], ib = m.indices[i + 1], ic = m.indices[i + 2];
        if (ia >= m.positions.size() || ib >= m.positions.size() || ic >= m.positions.size()) {
            ++r.outOfBoundsIndices;
            continue;
        }
        const glm::vec3 a = m.positions[ia], b = m.positions[ib], c = m.positions[ic];
        for (const glm::vec3& p : { a, b, c }) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) ++r.nonFinite;
        }
        const glm::vec3 cr = glm::cross(b - a, c - a);
        const float area2 = glm::length(cr);
        if (area2 < 1e-10f) { ++r.degenerateGeo; continue; }
        const glm::vec2 uvA = m.uvs[ia], uvB = m.uvs[ib], uvC = m.uvs[ic];
        const float uvArea = std::abs((uvB.x - uvA.x) * (uvC.y - uvA.y) -
                                      (uvC.x - uvA.x) * (uvB.y - uvA.y));
        if (uvArea < 1e-10f) ++r.degenerateUV;
        if (checkWinding) {
            const glm::vec3 geoN = cr / area2;
            const glm::vec3 nAvg = glm::normalize(m.normals[ia] + m.normals[ib] + m.normals[ic]);
            if (glm::dot(geoN, nAvg) < 0.0f) ++r.windingMismatch;
        }
    }
    return r;
}

}  // namespace procgen
}  // namespace Vapor
