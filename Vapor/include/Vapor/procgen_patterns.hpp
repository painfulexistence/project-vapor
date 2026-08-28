#pragma once
// ============================================================================
// Vapor::procgen — architectural pattern generators built on procgen.hpp.
//
// Higher-level generators with opinions about real-world construction:
// tiled panels with recessed grout channels and beveled tile edges (all real
// geometry, so silhouettes and grazing light stay honest), and swept trim
// profiles (bullnose pool coping, concave cove base trim) for sweepProfile.
// The primitive vocabulary they build on lives in procgen.hpp.
// ============================================================================

#include "procgen.hpp"

namespace Vapor {
namespace procgen {

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

// ── Swept trim profiles (pool coping, cove base) ────────────────────────────

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

}  // namespace procgen
}  // namespace Vapor
