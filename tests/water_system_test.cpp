// Water ECS system tests — exercise the REAL Vapor::WaterSystem against a
// recording mock IRenderer. The properties here are the ones with no runtime
// symptom until the frame budget is gone: setWaterGrid reallocates GPU buffers
// and setWaterSimParams dirties the spectrum for a rebake, so a system that
// pushed them every frame would rebuild the surface sixty times a second and
// nothing would say so.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "Vapor/components.hpp"
#include "Vapor/irenderer.hpp"
#include "Vapor/render_scene.hpp"
#include "Vapor/scene_blueprint.hpp"
#include "Vapor/systems.hpp"

using Catch::Approx;
using namespace Vapor;

// ── Recording mock renderer ────────────────────────────────────────────────
// Implements only the water API; everything else in IRenderer is a default
// no-op. Counting calls is the point — the assertions are about how OFTEN each
// entry point is driven, not just what reaches it.
struct MockWaterRenderer : IRenderer {
    int gridCalls = 0, transformCalls = 0, simCalls = 0, settingsCalls = 0;
    bool enabled = false;
    Uint32 tilesX = 0, tilesZ = 0;
    float tileSize = 0.0f, texTileX = 0.0f, texTileZ = 0.0f;
    WaterTransform transform;
    WaterSimParams sim;
    WaterData settings{};

    void setWaterEnabled(bool e) override { enabled = e; }
    bool isWaterEnabled() const override { return enabled; }
    void setWaterGrid(Uint32 tx, Uint32 tz, float ts, float ttx, float ttz) override {
        ++gridCalls;
        tilesX = tx; tilesZ = tz; tileSize = ts; texTileX = ttx; texTileZ = ttz;
    }
    void setWaterTransform(const WaterTransform& t) override { ++transformCalls; transform = t; }
    void setWaterSettings(const WaterData& d) override { ++settingsCalls; settings = d; }
    void setWaterSimParams(const WaterSimParams& p) override { ++simCalls; sim = p; }
};

namespace {

// A water entity at `position`, returning the component by reference so a test
// can edit it between frames the way an inspector would.
WaterComponent& makeWater(entt::registry& reg, glm::vec3 position = glm::vec3(0.0f)) {
    const entt::entity e = reg.create();
    auto& t = reg.emplace<TransformComponent>(e);
    t.position = position;
    return reg.emplace<WaterComponent>(e);
}

}  // namespace

TEST_CASE("water pushes the look every frame but the grid and spectrum only on change",
          "[water][systems]") {
    entt::registry reg;
    MockWaterRenderer renderer;
    WaterComponent& water = makeWater(reg, glm::vec3(0.0f, 1.5f, 0.0f));

    for (int frame = 0; frame < 10; ++frame) WaterSystem::update(reg, &renderer);
    CHECK(renderer.gridCalls == 1);       // reallocates GPU buffers
    CHECK(renderer.transformCalls == 1);
    CHECK(renderer.simCalls == 1);        // each push forces a spectrum rebake
    CHECK(renderer.settingsCalls == 10);  // an assignment; stays live-tunable
    CHECK(renderer.enabled);

    SECTION("a look edit touches nothing expensive") {
        water.look.roughness = 0.2f;
        WaterSystem::update(reg, &renderer);
        CHECK(renderer.gridCalls == 1);
        CHECK(renderer.simCalls == 1);
        CHECK(renderer.settingsCalls == 11);
        CHECK(renderer.settings.roughness == Approx(0.2f));
    }

    SECTION("a spectrum edit rebakes the spectrum and nothing else") {
        water.spectrum.amplitude = 0.00067f;
        WaterSystem::update(reg, &renderer);
        CHECK(renderer.simCalls == 2);
        CHECK(renderer.gridCalls == 1);
        CHECK(renderer.sim.amplitude == Approx(0.00067f));
    }

    SECTION("a grid edit rebuilds the grid and nothing else") {
        water.grid.tilesX = 96;
        WaterSystem::update(reg, &renderer);
        CHECK(renderer.gridCalls == 2);
        CHECK(renderer.tilesX == 96u);
        CHECK(renderer.simCalls == 1);
    }

    SECTION("texTile is a grid argument, not part of the packed change key") {
        water.grid.texTile.y = 3.0f;
        WaterSystem::update(reg, &renderer);
        CHECK(renderer.gridCalls == 2);
        CHECK(renderer.texTileZ == Approx(3.0f));
    }

    SECTION("moving the entity moves the surface and the caustics plane") {
        auto view = reg.view<WaterComponent>();
        reg.get<TransformComponent>(view.front()).position.y = 4.25f;
        WaterSystem::update(reg, &renderer);
        CHECK(renderer.transformCalls == 2);
        CHECK(renderer.transform.position.y == Approx(4.25f));
        CHECK(renderer.settings.causticsParams.w == Approx(4.25f));
    }
}

TEST_CASE("water is switched off when no component drives it", "[water][systems]") {
    entt::registry reg;
    MockWaterRenderer renderer;

    // Like VolumetricFogSystem, an empty scene cleanly disables the pass —
    // opting into the system means delegating the surface to it.
    WaterSystem::update(reg, &renderer);
    CHECK_FALSE(renderer.enabled);
    CHECK(renderer.gridCalls == 0);
    CHECK(renderer.settingsCalls == 0);

    WaterComponent& water = makeWater(reg);
    water.enabled = false;
    WaterSystem::update(reg, &renderer);
    CHECK_FALSE(renderer.enabled);

    water.enabled = true;
    WaterSystem::update(reg, &renderer);
    CHECK(renderer.enabled);
    CHECK(renderer.gridCalls == 1);
}

TEST_CASE("water skips inactive entities and reports a second surface", "[water][systems]") {
    SECTION("InactiveComponent is excluded like every other system") {
        entt::registry reg;
        MockWaterRenderer renderer;
        makeWater(reg);
        reg.emplace<InactiveComponent>(reg.view<WaterComponent>().front());
        WaterSystem::update(reg, &renderer);
        CHECK_FALSE(renderer.enabled);
    }

    SECTION("the renderer has one surface, so the first enabled component wins") {
        entt::registry reg;
        MockWaterRenderer renderer;
        makeWater(reg).grid.tilesX = 12;
        makeWater(reg).grid.tilesX = 34;
        WaterSystem::update(reg, &renderer);
        CHECK(renderer.gridCalls == 1);
        // Whichever the view yields first, exactly one of them drove the grid —
        // the two must not fight over the surface frame by frame, which would
        // reallocate the grid buffers on every single update.
        const Uint32 winner = renderer.tilesX;
        CHECK((winner == 12u || winner == 34u));
        for (int frame = 0; frame < 30; ++frame) WaterSystem::update(reg, &renderer);
        CHECK(renderer.gridCalls == 1);
        CHECK(renderer.tilesX == winner);
    }
}

TEST_CASE("hostile water data is clamped before it sizes an allocation", "[water][systems]") {
    entt::registry reg;
    MockWaterRenderer renderer;
    WaterComponent& water = makeWater(reg);
    // Tile counts size a (tilesX+1)*(tilesZ+1) vertex buffer; 4e6 tiles would
    // ask for hundreds of gigabytes, and 0 builds nothing at all.
    water.grid.tilesX = 0;
    water.grid.tilesZ = 4'000'000u;
    water.grid.tileSize = 0.0f;
    WaterSystem::update(reg, &renderer);
    CHECK(renderer.tilesX >= 1u);
    CHECK(renderer.tilesZ <= 4096u);
    CHECK(renderer.tileSize > 0.0f);

    // The clamp must not thrash: if the change key were built from the
    // unclamped input it would differ from the clamped last-pushed value every
    // frame, rebuilding the grid forever.
    const int after = renderer.gridCalls;
    WaterSystem::update(reg, &renderer);
    WaterSystem::update(reg, &renderer);
    CHECK(renderer.gridCalls == after);
}

TEST_CASE("caustics bounds are authored relative and baked to an ordered world AABB",
          "[water][systems]") {
    entt::registry reg;
    MockWaterRenderer renderer;
    WaterComponent& water = makeWater(reg, glm::vec3(10.0f, 2.0f, -4.0f));
    water.caustics.boundsMin = glm::vec3(-3.0f, -1.0f, -2.0f);
    water.caustics.boundsMax = glm::vec3(3.0f, 0.0f, 2.0f);
    WaterSystem::update(reg, &renderer);

    CHECK(glm::vec3(renderer.settings.causticsBoundsMin) == glm::vec3(7.0f, 1.0f, -6.0f));
    CHECK(glm::vec3(renderer.settings.causticsBoundsMax) == glm::vec3(13.0f, 2.0f, -2.0f));
    CHECK(renderer.settings.causticsBoundsMin.w == Approx(water.caustics.fadeDepth));
    CHECK(renderer.settings.causticsBoundsMax.w == Approx(water.caustics.envReflectionIntensity));

    // A mirrored transform must not invert the box — the shader tests it as an
    // ordered AABB and would reject every sample inside an inverted one.
    reg.get<TransformComponent>(reg.view<WaterComponent>().front()).scale = glm::vec3(-1.0f, 1.0f, -1.0f);
    WaterSystem::update(reg, &renderer);
    const glm::vec3 lo(renderer.settings.causticsBoundsMin);
    const glm::vec3 hi(renderer.settings.causticsBoundsMax);
    CHECK(lo.x <= hi.x);
    CHECK(lo.y <= hi.y);
    CHECK(lo.z <= hi.z);
}

TEST_CASE("the authored look reaches WaterData intact", "[water][systems]") {
    entt::registry reg;
    MockWaterRenderer renderer;
    WaterComponent& water = makeWater(reg);
    water.look.surfaceColor = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    water.look.foamTiling = 7.5f;
    water.look.reflectance = 0.21f;
    water.look.fftParams = glm::vec4(1.0f, 2.0f, 3.0f, 4.0f);
    WaterSystem::update(reg, &renderer);

    CHECK(renderer.settings.surfaceColor == water.look.surfaceColor);
    CHECK(renderer.settings.foamTiling == Approx(7.5f));
    CHECK(renderer.settings.reflectance == Approx(0.21f));
    CHECK(renderer.settings.fftParams == water.look.fftParams);
    // The RHI pass displaces from the FFT simulation, so the legacy waves[]
    // block stays disabled — a non-zero count would divide by its amplitude.
    CHECK(renderer.settings.waveCount == 0u);
}

// ── Scene JSON ─────────────────────────────────────────────────────────────

TEST_CASE("a water surface can be declared entirely in scene JSON", "[water][scene_blueprint]") {
    SceneBlueprint bp = parseSceneBlueprint(R"({
        "entities": [
            { "name": "PoolSurface",
              "position": [0, 1.6, 0],
              "components": {
                  "water": {
                      "grid": { "tilesX": 56, "tilesZ": 24, "tileSize": 0.25 },
                      "look": { "roughness": 0.045, "reflectance": 0.38, "foamTiling": 2.0 },
                      "caustics": { "intensity": 1.15, "boundsMin": [-7, -2, -3], "boundsMax": [7, 0, 3] },
                      "spectrum": { "patchSize": 16.0, "windSpeedMps": 1.7, "amplitude": 0.00019 }
                  }
              } }
        ]
    })");
    REQUIRE(bp.ok);
    REQUIRE(bp.entities.size() == 1);

    entt::registry reg;
    RenderScene scene;
    instantiate(reg, scene, bp);

    auto view = reg.view<WaterComponent>();
    REQUIRE(view.size() == 1);
    const WaterComponent& water = view.get<WaterComponent>(view.front());
    // Nested groups round-trip through the reflection path...
    CHECK(water.grid.tilesX == 56u);
    CHECK(water.grid.tilesZ == 24u);
    CHECK(water.grid.tileSize == Approx(0.25f));
    CHECK(water.spectrum.amplitude == Approx(0.00019f));
    CHECK(water.spectrum.windSpeedMps == Approx(1.7f));
    CHECK(water.caustics.boundsMin == glm::vec3(-7.0f, -2.0f, -3.0f));
    CHECK(water.look.reflectance == Approx(0.38f));
    // ...and an unmentioned field keeps the component's default rather than
    // being zeroed, which is what makes partial authoring usable.
    CHECK(water.look.dampeningFactor == Approx(WaterComponent{}.look.dampeningFactor));
    CHECK(water.spectrum.gravity == Approx(9.81f));
    CHECK(water.enabled);

    // ...and the whole thing drives the renderer straight off the scene load.
    MockWaterRenderer renderer;
    WaterSystem::update(reg, &renderer);
    CHECK(renderer.enabled);
    CHECK(renderer.tilesX == 56u);
    CHECK(renderer.sim.amplitude == Approx(0.00019f));
    CHECK(renderer.settings.causticsParams.w == Approx(1.6f));  // water level = entity Y
}

TEST_CASE("malformed water JSON falls back to defaults instead of throwing",
          "[water][scene_blueprint]") {
    SceneBlueprint bp = parseSceneBlueprint(R"({
        "entities": [
            { "name": "Bad",
              "components": {
                  "water": {
                      "grid": "not an object",
                      "look": { "roughness": "smooth", "reflectance": [1, 2] },
                      "spectrum": { "amplitude": null, "seed": -5 },
                      "enabled": 7
                  }
              } }
        ]
    })");
    REQUIRE(bp.ok);

    entt::registry reg;
    RenderScene scene;
    instantiate(reg, scene, bp);

    auto view = reg.view<WaterComponent>();
    REQUIRE(view.size() == 1);
    const WaterComponent& water = view.get<WaterComponent>(view.front());
    const WaterComponent defaults{};
    CHECK(water.grid.tilesX == defaults.grid.tilesX);
    CHECK(water.look.roughness == Approx(defaults.look.roughness));
    CHECK(water.look.reflectance == Approx(defaults.look.reflectance));
    CHECK(water.spectrum.amplitude == Approx(defaults.spectrum.amplitude));
    CHECK(water.enabled == defaults.enabled);

    // A bad component must still leave a surface the renderer can drive.
    MockWaterRenderer renderer;
    WaterSystem::update(reg, &renderer);
    CHECK(renderer.enabled);
    CHECK(renderer.tilesX >= 1u);
}
