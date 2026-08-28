#include <catch2/catch_test_macros.hpp>
#include "Vapor/procgen.hpp"
#include "Vapor/procgen_patterns.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>
#include <utility>

// ============================================================================
// Vapor::procgen verification — every property here is one the engine relies
// on downstream:
//   - validate() is the bake gate: each defect class must be caught, because
//     Mesh::initialize() runs MikkTSpace and throws on degenerate UVs.
//   - Winding/normal agreement everywhere: the renderer culls back faces, so
//     an inverted operator output simply vanishes from the frame.
//   - triangulatePolygon must survive the layouts architecture produces
//     (axis-aligned holes sharing rows/columns) — checked by area
//     conservation, all-CCW output, and a randomized lattice stress.
//   - Closed-volume checks (divergence theorem) prove cap/wall orientation
//     for extrudePolygon and emitBoxes in one number.
// ============================================================================

using namespace Vapor::procgen;

namespace {

double surfaceArea(const MeshData& m) {
    double a = 0.0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const glm::vec3 e1 = m.positions[m.indices[i + 1]] - m.positions[m.indices[i]];
        const glm::vec3 e2 = m.positions[m.indices[i + 2]] - m.positions[m.indices[i]];
        a += 0.5 * glm::length(glm::cross(e1, e2));
    }
    return a;
}

// Signed volume via the divergence theorem — needs a CLOSED surface with
// outward winding; any flipped cap or wall shows up immediately.
double signedVolume(const MeshData& m) {
    double v = 0.0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const glm::vec3 a = m.positions[m.indices[i]];
        const glm::vec3 b = m.positions[m.indices[i + 1]];
        const glm::vec3 c = m.positions[m.indices[i + 2]];
        v += glm::dot(a, glm::cross(b, c)) / 6.0;
    }
    return v;
}

double area2D(const std::vector<glm::vec2>& verts, const std::vector<uint32_t>& tris,
              bool& allCCW) {
    double a = 0.0;
    allCCW = true;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const glm::vec2 p = verts[tris[i]], q = verts[tris[i + 1]], r = verts[tris[i + 2]];
        const double s = 0.5 * ((q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y));
        if (s <= 0.0) allCCW = false;
        a += s;
    }
    return a;
}

MeshData unitQuad() {
    MeshData m;
    m.quad({ 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
           { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 });
    return m;
}

}  // namespace

TEST_CASE("validate 對每一類缺陷各自計數", "[procgen][validate]") {
    const MeshData good = unitQuad();
    REQUIRE(validate(good).ok());
    REQUIRE(validate(good).triangles == 2);

    MeshData oob = good;
    oob.indices[0] = 99;
    CHECK(validate(oob).outOfBoundsIndices == 1);

    MeshData nan = good;
    nan.positions[0].x = std::numeric_limits<float>::quiet_NaN();
    CHECK(validate(nan).nonFinite > 0);

    MeshData degUV = good;
    for (auto& uv : degUV.uvs) uv = glm::vec2(0.25f, 0.75f);
    CHECK(validate(degUV).degenerateUV == 2);

    MeshData degGeo = good;
    for (auto& p : degGeo.positions) p = glm::vec3(0.5f);
    CHECK(validate(degGeo).degenerateGeo == 2);

    MeshData flipped = good;
    std::swap(flipped.indices[1], flipped.indices[2]);
    CHECK(validate(flipped).windingMismatch == 1);
}

TEST_CASE("appendTransformed 剛體放置與鏡像都保持朝向", "[procgen][transform]") {
    const MeshData proto = unitQuad();

    MeshData dst;
    const glm::mat4 rt = glm::translate(glm::mat4(1.0f), glm::vec3(3, 4, 5)) *
                         glm::rotate(glm::mat4(1.0f), glm::radians(37.0f), glm::vec3(0, 1, 0));
    appendTransformed(dst, proto, rt);
    REQUIRE(dst.triangleCount() == 2);
    CHECK(validate(dst).ok());
    const glm::vec3 expected = glm::vec3(rt * glm::vec4(1, 1, 0, 1));
    CHECK(glm::length(dst.positions[2] - expected) < 1e-5f);

    // det < 0 must flip the triangle winding, or the mirror copy is culled.
    MeshData mirrored;
    appendTransformed(mirrored, proto, glm::scale(glm::mat4(1.0f), glm::vec3(-1, 1, 1)));
    CHECK(validate(mirrored).ok());

    // Appending into a non-empty mesh rebases indices.
    MeshData both = proto;
    appendTransformed(both, proto, rt);
    REQUIRE(both.triangleCount() == 4);
    CHECK(validate(both).ok());
}

TEST_CASE("frameTransform 把標準座標軸映射到 (U,V,W) 框架", "[procgen][transform]") {
    const glm::mat4 f = frameTransform(glm::vec3(1, 2, 3), glm::vec3(0, 0, -2), glm::vec3(0, 5, 0));
    CHECK(glm::length(glm::vec3(f * glm::vec4(1, 0, 0, 0)) - glm::vec3(0, 0, -1)) < 1e-6f);
    CHECK(glm::length(glm::vec3(f * glm::vec4(0, 1, 0, 0)) - glm::vec3(0, 1, 0)) < 1e-6f);
    CHECK(glm::length(glm::vec3(f * glm::vec4(0, 0, 0, 1)) - glm::vec3(1, 2, 3)) < 1e-6f);
    CHECK(glm::determinant(glm::mat3(f)) > 0.0f);
}

TEST_CASE("lathe 旋轉成型：球的表面積、體積與極點扇形", "[procgen][lathe]") {
    MeshData sphere;
    std::vector<glm::vec2> prof;
    const int N = 32;
    const float R = 0.7f;
    for (int i = 0; i <= N; ++i) {
        const float t = glm::pi<float>() * (static_cast<float>(i) / N - 0.5f);
        prof.push_back(glm::vec2(R * std::cos(t), R * std::sin(t)));
    }
    lathe(prof, 48, sphere);

    const ValidationReport r = validate(sphere);
    CHECK(r.ok());

    const double exactA = 4.0 * glm::pi<double>() * R * R;
    CHECK(std::abs(surfaceArea(sphere) - exactA) / exactA < 0.01);
    // Positive closed volume == outward winding everywhere (poles included).
    const double exactV = 4.0 / 3.0 * glm::pi<double>() * R * R * R;
    CHECK(std::abs(signedVolume(sphere) - exactV) / exactV < 0.02);

    // Open dome (no pole at the base) — the porthole-lamp use case.
    MeshData dome;
    lathe({ { 0.165f, 0.030f }, { 0.150f, 0.055f }, { 0.110f, 0.070f },
            { 0.055f, 0.077f }, { 0.0f, 0.080f } }, 24, dome);
    CHECK(validate(dome).ok());
    CHECK(dome.triangleCount() > 0);
}

TEST_CASE("triangulatePolygon 面積守恆且輸出全為 CCW", "[procgen][triangulate]") {
    std::vector<glm::vec2> verts;
    bool ccw = false;

    SECTION("正方形（輸入為 CW，需自行正規化）") {
        auto tris = triangulatePolygon({ { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } }, {}, verts);
        CHECK(std::abs(area2D(verts, tris, ccw) - 1.0) < 1e-6);
        CHECK(ccw);
    }
    SECTION("凹多邊形 L 形") {
        auto tris = triangulatePolygon({ { 0, 0 }, { 2, 0 }, { 2, 1 }, { 1, 1 }, { 1, 2 }, { 0, 2 } },
                                       {}, verts);
        CHECK(std::abs(area2D(verts, tris, ccw) - 3.0) < 1e-5);
        CHECK(ccw);
    }
    SECTION("方形開一個洞") {
        auto tris = triangulatePolygon({ { 0, 0 }, { 3, 0 }, { 3, 3 }, { 0, 3 } },
                                       { { { 1, 1 }, { 2, 1 }, { 2, 2 }, { 1, 2 } } }, verts);
        CHECK(std::abs(area2D(verts, tris, ccw) - 8.0) < 1e-5);
        CHECK(ccw);
    }
    SECTION("Poolrooms 天花板：六個共列/共行的天窗洞") {
        std::vector<std::vector<glm::vec2>> wells;
        for (float wx : { 7.0f, 12.0f, 17.0f }) {
            for (float wz : { 3.5f, 6.5f }) {
                wells.push_back({ { wx - 1.2f, wz - 0.8f }, { wx + 1.2f, wz - 0.8f },
                                  { wx + 1.2f, wz + 0.8f }, { wx - 1.2f, wz + 0.8f } });
            }
        }
        auto tris = triangulatePolygon({ { 0, 0 }, { 24, 0 }, { 24, 10 }, { 0, 10 } }, wells, verts);
        const double want = 24.0 * 10.0 - 6.0 * (2.4 * 1.6);
        CHECK(std::abs(area2D(verts, tris, ccw) - want) < 1e-3);
        CHECK(ccw);
    }
    SECTION("旋轉五邊形開圓洞（非軸對齊邊）") {
        std::vector<glm::vec2> penta, circle;
        for (int i = 0; i < 5; ++i) {
            const float a = 2.0f * glm::pi<float>() * i / 5.0f + 0.4f;
            penta.push_back(glm::vec2(5.0f * std::cos(a), 5.0f * std::sin(a)));
        }
        for (int i = 0; i < 24; ++i) {
            const float a = 2.0f * glm::pi<float>() * i / 24.0f;
            circle.push_back(glm::vec2(1.3f * std::cos(a), 1.3f * std::sin(a)));
        }
        auto tris = triangulatePolygon(penta, { circle }, verts);
        const double pentaArea = 0.5 * 5.0 * 25.0 * std::sin(2.0 * glm::pi<double>() / 5.0);
        const double circleArea = 0.5 * 24.0 * 1.3 * 1.3 * std::sin(2.0 * glm::pi<double>() / 24.0);
        CHECK(std::abs(area2D(verts, tris, ccw) - (pentaArea - circleArea)) < 1e-3 * pentaArea);
        CHECK(ccw);
    }
}

TEST_CASE("triangulatePolygon 隨機格網洞壓力測試", "[procgen][triangulate][stress]") {
    // Holes snapped to a coarse lattice share rows/columns constantly — the
    // configuration class that wedges bridge-based ear clippers.
    uint32_t rng = 0xC0FFEEu;
    auto rnd = [&rng](uint32_t n) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng % n;
    };
    for (int iter = 0; iter < 100; ++iter) {
        const int gw = 3 + static_cast<int>(rnd(4));
        const int gh = 2 + static_cast<int>(rnd(3));
        std::vector<std::vector<glm::vec2>> holes;
        double holeArea = 0.0;
        for (int cy = 0; cy < gh; ++cy) {
            for (int cx = 0; cx < gw; ++cx) {
                if (rnd(3) == 0) continue;
                const float x0 = cx * 3.0f + 0.5f + 0.25f * rnd(3);
                const float y0 = cy * 3.0f + 0.5f + 0.25f * rnd(3);
                const float w = 1.0f + 0.25f * rnd(4);
                const float h = 1.0f + 0.25f * rnd(4);
                holes.push_back({ { x0, y0 }, { x0 + w, y0 }, { x0 + w, y0 + h }, { x0, y0 + h } });
                holeArea += double(w) * h;
            }
        }
        const glm::vec2 ext(gw * 3.0f, gh * 3.0f);
        std::vector<glm::vec2> verts;
        bool ccw = false;
        auto tris = triangulatePolygon(
            { { 0, 0 }, { ext.x, 0 }, { ext.x, ext.y }, { 0, ext.y } }, holes, verts);
        const double want = double(ext.x) * ext.y - holeArea;
        REQUIRE(std::abs(area2D(verts, tris, ccw) - want) < 1e-3 * want);
        REQUIRE(ccw);
    }
}

TEST_CASE("polygonCap 在 3D 框架中朝向正確（向下的天花板）", "[procgen][cap]") {
    MeshData cap;
    std::vector<std::vector<glm::vec2>> wells;
    for (float wx : { 7.0f, 12.0f, 17.0f })
        for (float wz : { 3.5f, 6.5f })
            wells.push_back({ { wx - 1.2f, wz - 0.8f }, { wx + 1.2f, wz - 0.8f },
                              { wx + 1.2f, wz + 0.8f }, { wx - 1.2f, wz + 0.8f } });
    polygonCap(glm::vec3(-12, 4.6f, -5), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1),
               { { 0, 0 }, { 24, 0 }, { 24, 10 }, { 0, 10 } }, wells, 0.5f, cap);

    const ValidationReport r = validate(cap);
    CHECK(r.ok());
    REQUIRE(!cap.normals.empty());
    CHECK(cap.normals[0].y < -0.99f);  // U=+X, V=+Z -> W=-Y
    CHECK(std::abs(surfaceArea(cap) - (240.0 - 6.0 * 3.84)) < 1e-2);
}

TEST_CASE("extrudePolygon 封閉稜柱的體積與表面積", "[procgen][extrude]") {
    MeshData prism;
    extrudePolygon(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                   { { 0, 0 }, { 3, 0 }, { 3, 3 }, { 0, 3 } },
                   { { { 1, 1 }, { 2, 1 }, { 2, 2 }, { 1, 2 } } },
                   0.4f, 1.0f, prism);
    CHECK(validate(prism).ok());
    // Volume proves front/back cap + outer/hole wall orientation all at once.
    CHECK(std::abs(signedVolume(prism) - 8.0 * 0.4) < 1e-4);
    const double wantArea = 2.0 * 8.0 + 12.0 * 0.4 + 4.0 * 0.4;
    CHECK(std::abs(surfaceArea(prism) - wantArea) < 1e-3);
}

TEST_CASE("subtractBox 切出的殘餘箱不重疊且體積守恆", "[procgen][boxes]") {
    std::vector<Box3> solid{ { glm::vec3(-12, -0.5f, -5), glm::vec3(12, 0, 5) } };
    subtractBox(solid, { glm::vec3(-7, -1, -3), glm::vec3(7, 1, 3) });
    REQUIRE(solid.size() == 4);

    double vol = 0.0;
    for (const Box3& b : solid) {
        const glm::vec3 d = b.max - b.min;
        vol += double(d.x) * d.y * d.z;
    }
    CHECK(std::abs(vol - (24.0 * 0.5 * 10.0 - 14.0 * 0.5 * 6.0)) < 1e-4);

    for (size_t i = 0; i < solid.size(); ++i) {
        for (size_t j = i + 1; j < solid.size(); ++j) {
            const glm::vec3 lo = glm::max(solid[i].min, solid[j].min);
            const glm::vec3 hi = glm::min(solid[i].max, solid[j].max);
            CHECK(!(lo.x < hi.x - 1e-6f && lo.y < hi.y - 1e-6f && lo.z < hi.z - 1e-6f));
        }
    }

    std::vector<CollisionBox> cols;
    boxesToColliders(solid, cols);
    REQUIRE(cols.size() == 4);
    for (size_t i = 0; i < cols.size(); ++i) {
        CHECK(glm::length((cols[i].center - cols[i].halfExtents) - solid[i].min) < 1e-5f);
        CHECK(glm::length((cols[i].center + cols[i].halfExtents) - solid[i].max) < 1e-5f);
    }
}

TEST_CASE("emitBoxes 精確共面時剔除內部面，聯集表面封閉", "[procgen][boxes]") {
    std::vector<Box3> stack{
        { glm::vec3(0, 0, 0), glm::vec3(2, 1, 2) },
        { glm::vec3(0, 1, 0), glm::vec3(2, 2, 2) },  // sits exactly on top
    };
    MeshData m;
    emitBoxes(stack, 0.5f, m);
    CHECK(m.triangleCount() == 20);  // 12 faces - 2 shared = 10 faces
    CHECK(validate(m).ok());
    // Closed outward union surface -> exact volume of the 2x2x2 stack.
    CHECK(std::abs(signedVolume(m) - 8.0) < 1e-4);

    // Partial coverage keeps the face (by design): 6 + 5 faces survive.
    std::vector<Box3> lshape{ { glm::vec3(0, 0, 0), glm::vec3(4, 1, 4) } };
    subtractBox(lshape, { glm::vec3(2, -1, 2), glm::vec3(5, 2, 5) });
    REQUIRE(lshape.size() == 2);
    MeshData lm;
    emitBoxes(lshape, 0.5f, lm);
    CHECK(validate(lm).ok());
    CHECK(lm.triangleCount() == 22);
}

TEST_CASE("buildTilePanel 磁磚面帶溝縫皆通過烘焙檢核", "[procgen][patterns]") {
    TilePanelDesc d;
    d.width = 2.0f;
    d.height = 1.2f;
    d.uPhase = 0.35f;  // partial cells on the borders
    MeshData tiles, grout;
    buildTilePanel(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), d, tiles, grout);

    REQUIRE(tiles.triangleCount() > 0);
    REQUIRE(grout.triangleCount() > 0);
    CHECK(validate(tiles).ok());
    CHECK(validate(grout).ok());
    // The tile faces sit exactly on the panel plane, grout floors behind it.
    for (const glm::vec3& p : tiles.positions) CHECK(p.z <= 1e-6f);
    for (const glm::vec3& p : grout.positions) CHECK(p.z < -1e-4f);
}

TEST_CASE("sweepProfile + copingProfile 沿封閉路徑掃掠仍朝外", "[procgen][patterns]") {
    const auto rim = roundedRectPathCCW(glm::vec2(-2, -1), glm::vec2(2, 1), 0.12f, 4, 0.0f);
    MeshData coping;
    sweepProfile(rim, true, copingProfile(0.06f, 0.16f, 0.30f, 6), 1.0f / 0.30f, coping);
    REQUIRE(coping.triangleCount() > 0);
    CHECK(validate(coping).ok());
}

TEST_CASE("sweepTube 圓管與端蓋朝外（含平行傳輸框架）", "[procgen][sweep]") {
    std::vector<glm::vec3> path;
    appendLine(path, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0), 4, true);
    appendArc(path, glm::vec3(0.3f, 1, 0), glm::vec3(-0.3f, 0, 0), glm::vec3(0, 0.3f, 0), 6);
    MeshData tube;
    sweepTube(path, 0.05f, 12, 0.25f, tube, true);
    REQUIRE(tube.triangleCount() > 0);
    CHECK(validate(tube).ok());
}
