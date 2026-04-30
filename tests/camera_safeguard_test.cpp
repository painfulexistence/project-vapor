// Characterization tests for Camera: view/proj matrix, frustum culling.
// These lock CURRENT behaviour — particularly the frustum plane extraction
// formula which is non-obvious and easy to silently break.
#define GLM_ENABLE_EXPERIMENTAL
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Vapor/camera.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using Catch::Approx;

// ============================================================
// Construction & defaults
// ============================================================

TEST_CASE("Camera 預設建構：非正交模式", "[camera]") {
    Camera cam;
    REQUIRE_FALSE(cam.isOrthographic());
}

TEST_CASE("Camera getProjMatrix 回傳非零矩陣", "[camera]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0}, glm::radians(60.f), 1.6f, 0.1f, 500.f);
    auto proj = cam.getProjMatrix();
    // 透視矩陣 [2][2] 不為 0
    REQUIRE(proj[2][2] != Approx(0.0f));
}

TEST_CASE("Camera getViewMatrix eye=0 center=-Y 正確", "[camera]") {
    // 預設 center=(0,-1,0)；eye=(0,0,0)；forward 朝 -Y
    Camera cam;
    auto view = cam.getViewMatrix();
    // View matrix 不應是 identity
    REQUIRE(view != glm::identity<glm::mat4>());
}

// ============================================================
// Projection mode switching
// ============================================================

TEST_CASE("setOrthographic 切換為正交模式", "[camera][ortho]") {
    Camera cam;
    cam.setOrthographic(-10.f, 10.f, -10.f, 10.f, 0.1f, 100.f);
    REQUIRE(cam.isOrthographic());
}

TEST_CASE("setPerspective 切換回透視模式", "[camera][perspective]") {
    Camera cam;
    cam.setOrthographic(-1.f, 1.f, -1.f, 1.f, 0.1f, 100.f);
    cam.setPerspective(glm::radians(45.f), 1.f, 0.1f, 500.f);
    REQUIRE_FALSE(cam.isOrthographic());
}

TEST_CASE("正交 getProjMatrix [1][1] 等於 2/(top-bottom)", "[camera][ortho]") {
    Camera cam;
    cam.setOrthographic(-5.f, 5.f, -5.f, 5.f, 0.1f, 100.f);
    auto proj = cam.getProjMatrix();
    // GLM is column-major: proj[col][row]. proj[1][1] = column 1, row 1 = Y scale.
    // 2 / (top - bottom) = 2 / 10 = 0.2
    REQUIRE(proj[1][1] == Approx(0.2f));
}

// ============================================================
// Dirty flag caching
// ============================================================

TEST_CASE("getProjMatrix 連續呼叫回傳相同矩陣（有 cache）", "[camera][cache]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0}, glm::radians(45.f), 1.f, 0.1f, 100.f);
    auto p1 = cam.getProjMatrix();
    auto p2 = cam.getProjMatrix();
    REQUIRE(p1 == p2);
}

TEST_CASE("getViewMatrix 連續呼叫回傳相同矩陣（有 cache）", "[camera][cache]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0}, glm::radians(45.f), 1.f, 0.1f, 100.f);
    auto v1 = cam.getViewMatrix();
    auto v2 = cam.getViewMatrix();
    REQUIRE(v1 == v2);
}

TEST_CASE("setEye 後 getViewMatrix 更新", "[camera][cache]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0}, glm::radians(45.f), 1.f, 0.1f, 100.f);
    auto v1 = cam.getViewMatrix();
    cam.setEye({0.f, 0.f, 10.f});
    auto v2 = cam.getViewMatrix();
    REQUIRE(v1 != v2);
}

TEST_CASE("setCenter 後 getViewMatrix 更新", "[camera][cache]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0}, glm::radians(45.f), 1.f, 0.1f, 100.f);
    auto v1 = cam.getViewMatrix();
    cam.setCenter({1.f, 0.f, 0.f});
    auto v2 = cam.getViewMatrix();
    REQUIRE(v1 != v2);
}

// ============================================================
// getForward
// ============================================================

TEST_CASE("getForward 是 normalize(center - eye)", "[camera]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0});
    auto fwd = cam.getForward();
    // eye=(0,0,5) center=(0,0,0) → forward = (0,0,-1)
    REQUIRE(fwd.x == Approx(0.f));
    REQUIRE(fwd.y == Approx(0.f));
    REQUIRE(fwd.z == Approx(-1.f));
}

// ============================================================
// getFrustumPlanes — 6 個平面
// ============================================================

TEST_CASE("getFrustumPlanes 回傳 6 個平面", "[camera][frustum]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    auto planes = cam.getFrustumPlanes();
    REQUIRE(planes.size() == 6);
}

TEST_CASE("getFrustumPlanes 各平面法線已被 normalize（長度 ≈ 1）", "[camera][frustum]") {
    Camera cam({0,0,5}, {0,0,0}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    auto planes = cam.getFrustumPlanes();
    for (const auto& p : planes) {
        float len = glm::length(glm::vec3(p.x, p.y, p.z));
        REQUIRE(len == Approx(1.0f).epsilon(0.001f));
    }
}

// ============================================================
// isVisible(bsphere) — 球形 frustum culling
// ============================================================

TEST_CASE("isVisible bsphere：相機正前方的物體可見", "[camera][frustum][visible]") {
    // 相機在 z=10，朝向 origin
    Camera cam({0,0,10}, {0,0,0}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    // 在 z=0 放一個半徑 1 的球
    glm::vec4 bsphere{0.f, 0.f, 0.f, 1.f};
    REQUIRE(cam.isVisible(bsphere) == true);
}

TEST_CASE("isVisible bsphere：遠超 far plane 的物體不可見", "[camera][frustum][visible]") {
    Camera cam({0,0,0}, {0,0,-1}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    // far=100，球心在 z=-200，半徑 1
    glm::vec4 bsphere{0.f, 0.f, -200.f, 1.f};
    REQUIRE(cam.isVisible(bsphere) == false);
}

TEST_CASE("isVisible bsphere：相機後方物體不可見", "[camera][frustum][visible]") {
    // 相機在原點看向 -Z（center = (0,0,-1)）
    Camera cam({0,0,0}, {0,0,-1}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    // 球心在 +Z 方向（相機背後），半徑 0.1
    glm::vec4 bsphere{0.f, 0.f, 50.f, 0.1f};
    REQUIRE(cam.isVisible(bsphere) == false);
}

TEST_CASE("isVisible bsphere：大半徑球跨越 frustum 邊界仍可見", "[camera][frustum][visible]") {
    Camera cam({0,0,10}, {0,0,0}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    // 球心在 frustum 外側但半徑很大，還是應該可見
    glm::vec4 bsphere{100.f, 0.f, 0.f, 200.f};
    REQUIRE(cam.isVisible(bsphere) == true);
}

// ============================================================
// isVisible(AABB) — 包圍盒 frustum culling
// ============================================================

TEST_CASE("isVisible AABB：相機前方的包圍盒可見", "[camera][frustum][aabb]") {
    Camera cam({0,0,10}, {0,0,0}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    glm::vec3 minB{-1.f, -1.f, -1.f};
    glm::vec3 maxB{ 1.f,  1.f,  1.f};
    REQUIRE(cam.isVisible(minB, maxB) == true);
}

TEST_CASE("isVisible AABB：遠超 far plane 的包圍盒不可見", "[camera][frustum][aabb]") {
    Camera cam({0,0,0}, {0,0,-1}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 100.f);
    glm::vec3 minB{-1.f, -1.f, -300.f};
    glm::vec3 maxB{ 1.f,  1.f, -200.f};
    REQUIRE(cam.isVisible(minB, maxB) == false);
}

TEST_CASE("isVisible AABB：完全在側面以外的包圍盒不可見", "[camera][frustum][aabb]") {
    // FOV 90°，aspect 1：水平視角約 ±45°（z=-1 時 x 範圍 ±1）
    // 把包圍盒放在 x=1000（遠超視角範圍）
    Camera cam({0,0,0}, {0,0,-1}, {0,1,0}, glm::radians(90.f), 1.f, 0.1f, 500.f);
    glm::vec3 minB{900.f, -1.f, -5.f};
    glm::vec3 maxB{1000.f, 1.f,  5.f};
    REQUIRE(cam.isVisible(minB, maxB) == false);
}

// ============================================================
// near / far accessors
// ============================================================

TEST_CASE("Camera::near() 和 far() 回傳設定值", "[camera]") {
    Camera cam({}, {}, {}, glm::radians(45.f), 1.f, 0.5f, 200.f);
    REQUIRE(cam.near() == Approx(0.5f));
    REQUIRE(cam.far() == Approx(200.f));
}
