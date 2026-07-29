// Characterization tests for PathTraceAccumulator — photo mode's progressive
// accumulation state machine.
//
// These lock the invalidation rules, which are the part of path tracing that
// is easy to get subtly wrong and impossible to see in a screenshot: an
// accumulator that fails to reset blends two different images together, and one
// that resets too eagerly never converges at all. Neither failure crashes; both
// silently ruin the render.
//
// Pure logic, no GPU: the accumulator deliberately holds no device state.
#include <Vapor/path_tracer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using Vapor::CameraRenderData;
using Vapor::PathTraceAccumulator;

namespace {

CameraRenderData makeCamera(const glm::vec3& eye = { 0.0f, 0.0f, 5.0f }, float fovY = 1.0f) {
    CameraRenderData cam{};
    cam.view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cam.proj = glm::perspective(fovY, 1.6f, 0.1f, 1000.0f);
    cam.invView = glm::inverse(cam.view);
    cam.invProj = glm::inverse(cam.proj);
    cam.position = eye;
    cam.nearPlane = 0.1f;
    cam.farPlane = 1000.0f;
    return cam;
}

} // namespace

// ============================================================
// 初始狀態
// ============================================================

TEST_CASE("PathTraceAccumulator 初始為空且待重置", "[pathtracer]") {
    PathTraceAccumulator acc;
    REQUIRE(acc.sampleCount() == 0);
    REQUIRE(acc.resetPending());
    REQUIRE_FALSE(acc.hasImage());
}

TEST_CASE("PathTraceAccumulator 首次觀察相機視為變動", "[pathtracer]") {
    PathTraceAccumulator acc;
    // 累積器還沒有相機，第一次一定算變動 —— 否則會把樣本加進未初始化的貼圖
    REQUIRE(acc.observeCamera(makeCamera()));
    REQUIRE(acc.resetPending());
}

// ============================================================
// 樣本累積
// ============================================================

TEST_CASE("PathTraceAccumulator recordTraced 累加樣本並清除重置旗標", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera());

    acc.recordTraced(1);
    REQUIRE(acc.sampleCount() == 1);
    REQUIRE_FALSE(acc.resetPending());
    REQUIRE(acc.hasImage());

    acc.recordTraced(4);
    REQUIRE(acc.sampleCount() == 5);
}

TEST_CASE("PathTraceAccumulator 相機不動時樣本持續累積", "[pathtracer]") {
    PathTraceAccumulator acc;
    const CameraRenderData cam = makeCamera();

    acc.observeCamera(cam);
    acc.recordTraced(8);

    // 同一個相機再觀察一次不應丟棄任何東西 —— 這正是漸進收斂的前提
    REQUIRE_FALSE(acc.observeCamera(cam));
    REQUIRE(acc.sampleCount() == 8);
    REQUIRE_FALSE(acc.resetPending());
}

// ============================================================
// 失效條件
// ============================================================

TEST_CASE("PathTraceAccumulator 相機移動時丟棄累積影像", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera({ 0.0f, 0.0f, 5.0f }));
    acc.recordTraced(64);

    REQUIRE(acc.observeCamera(makeCamera({ 0.0f, 0.0f, 6.0f })));
    REQUIRE(acc.sampleCount() == 0);
    REQUIRE(acc.resetPending());
    REQUIRE_FALSE(acc.hasImage());
}

TEST_CASE("PathTraceAccumulator 投影改變（變焦）時丟棄累積影像", "[pathtracer]") {
    PathTraceAccumulator acc;
    const glm::vec3 eye{ 0.0f, 0.0f, 5.0f };
    acc.observeCamera(makeCamera(eye, 1.0f));
    acc.recordTraced(32);

    // 位置沒變、只有 FOV 變 —— view 矩陣相同，只有 proj 不同
    REQUIRE(acc.observeCamera(makeCamera(eye, 0.7f)));
    REQUIRE(acc.sampleCount() == 0);
}

TEST_CASE("PathTraceAccumulator invalidate 立即丟棄", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera());
    acc.recordTraced(100);

    acc.invalidate();
    REQUIRE(acc.sampleCount() == 0);
    REQUIRE(acc.resetPending());
}

TEST_CASE("PathTraceAccumulator 場景改變時丟棄累積影像", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera());
    acc.observeScene(42);
    acc.recordTraced(50);

    // 同一個場景版本不該丟棄
    REQUIRE_FALSE(acc.observeScene(42));
    REQUIRE(acc.sampleCount() == 50);

    // 換了模型／生成物件 —— 累積的影像已經對應不到現在的場景
    REQUIRE(acc.observeScene(43));
    REQUIRE(acc.sampleCount() == 0);
    REQUIRE(acc.resetPending());
}

TEST_CASE("PathTraceAccumulator 首次觀察場景視為變動", "[pathtracer]") {
    PathTraceAccumulator acc;
    REQUIRE(acc.observeScene(0));
}

// ============================================================
// 取樣預算
// ============================================================

TEST_CASE("PathTraceAccumulator samplesToTrace 未達預算時回傳完整批次", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera());
    acc.recordTraced(10);

    REQUIRE(acc.samplesToTrace(4, 100) == 4);
}

TEST_CASE("PathTraceAccumulator samplesToTrace 不會超出預算", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera());
    acc.recordTraced(98);

    // 預算剩 2，即使一次要求 16 也只給 2 —— 否則實際樣本數會超過使用者設定的上限
    REQUIRE(acc.samplesToTrace(16, 100) == 2);
}

TEST_CASE("PathTraceAccumulator 收斂後 samplesToTrace 回傳 0", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera());
    acc.recordTraced(100);

    REQUIRE(acc.isConverged(100));
    REQUIRE(acc.samplesToTrace(1, 100) == 0);
    // 超過預算（例如下調預算）同樣停止追蹤，而不是回繞成巨大的批次
    REQUIRE(acc.samplesToTrace(1, 50) == 0);
}

TEST_CASE("PathTraceAccumulator 提高預算可讓已收斂的算圖繼續", "[pathtracer]") {
    PathTraceAccumulator acc;
    acc.observeCamera(makeCamera());
    acc.recordTraced(100);
    REQUIRE(acc.samplesToTrace(4, 100) == 0);

    // 已累積的樣本仍然有效，只是預算變大 —— 不應該重來
    REQUIRE(acc.samplesToTrace(4, 200) == 4);
    REQUIRE(acc.sampleCount() == 100);
}
