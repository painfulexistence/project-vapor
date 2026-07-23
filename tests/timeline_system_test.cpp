#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entt/entt.hpp>
#include "Vapor/timeline_system.hpp"
#include "Vapor/tween.hpp"

using namespace Vapor;
using Catch::Approx;

// Pure-logic tests: library + registry + TimelineSystem, no GPU, no assets.

namespace {

    // 1-second clip moving X 0 → 10 linearly.
    ActionTimeline makeMoveClip(const std::string& name = "move") {
        ActionTimeline tl;
        tl.name = name;
        ActionTrack track;
        track.property = ActionProperty::Position;
        track.keys.push_back({ 0.0f, glm::vec4(0.0f), EasingType::Linear });
        track.keys.push_back({ 1.0f, glm::vec4(10.0f, 0.0f, 0.0f, 0.0f), EasingType::Linear });
        tl.tracks.push_back(std::move(track));
        tl.recompute();
        return tl;
    }

    entt::entity makeAnimatedEntity(entt::registry& reg, TimelineHandle h, WrapMode wrap) {
        auto e = reg.create();
        reg.emplace<TransformComponent>(e);
        TimelinePlaybackComponent play;
        play.clip = h;
        play.clips.push_back(h);
        play.wrap = wrap;
        play.playing = true;
        reg.emplace<TimelinePlaybackComponent>(e, std::move(play));
        return e;
    }

}// namespace

TEST_CASE("TimelineSystem - track sampling and application", "[timeline]") {
    AnimationClipLibrary lib;
    const TimelineHandle h = lib.addTimeline(makeMoveClip());
    REQUIRE(h.valid());
    REQUIRE(lib.getTimeline(h) != nullptr);
    CHECK(lib.getTimeline(h)->duration == Approx(1.0f));

    entt::registry reg;
    auto e = makeAnimatedEntity(reg, h, WrapMode::Once);

    TimelineSystem::update(reg, lib, 0.5f);
    auto& tc = reg.get<TransformComponent>(e);
    CHECK(tc.position.x == Approx(5.0f));
    CHECK(static_cast<bool>(reg.get<TimelinePlaybackComponent>(e).playing));

    SECTION("Once clamps at the end and stops") {
        TimelineSystem::update(reg, lib, 10.0f);
        CHECK(tc.position.x == Approx(10.0f));
        CHECK(!reg.get<TimelinePlaybackComponent>(e).playing);
    }

    SECTION("speed scales the playhead") {
        auto& play = reg.get<TimelinePlaybackComponent>(e);
        play.time = 0.0f;
        play.speed = 0.5f;
        TimelineSystem::update(reg, lib, 0.5f);// advances 0.25s
        CHECK(play.time == Approx(0.25f));
        CHECK(tc.position.x == Approx(2.5f));
    }
}

TEST_CASE("TimelineSystem - wrap modes", "[timeline]") {
    AnimationClipLibrary lib;
    const TimelineHandle h = lib.addTimeline(makeMoveClip());
    entt::registry reg;

    SECTION("Loop wraps the playhead") {
        auto e = makeAnimatedEntity(reg, h, WrapMode::Loop);
        TimelineSystem::update(reg, lib, 1.25f);
        auto& play = reg.get<TimelinePlaybackComponent>(e);
        CHECK(play.time == Approx(0.25f));
        CHECK(static_cast<bool>(play.playing));
        CHECK(reg.get<TransformComponent>(e).position.x == Approx(2.5f));
    }

    SECTION("PingPong reflects at the end") {
        auto e = makeAnimatedEntity(reg, h, WrapMode::PingPong);
        TimelineSystem::update(reg, lib, 1.25f);// 1.25 → reflected to 0.75, now moving backwards
        auto& play = reg.get<TimelinePlaybackComponent>(e);
        CHECK(play.time == Approx(0.75f));
        TimelineSystem::update(reg, lib, 0.25f);// backwards to 0.5
        CHECK(play.time == Approx(0.5f));
        CHECK(reg.get<TransformComponent>(e).position.x == Approx(5.0f));
    }
}

TEST_CASE("TimelineSystem - easing changes the interpolation", "[timeline]") {
    ActionTimeline tl = makeMoveClip("eased");
    tl.tracks[0].keys[1].easing = EasingType::QuadIn;// u² — slower start
    AnimationClipLibrary lib;
    const TimelineHandle h = lib.addTimeline(std::move(tl));

    entt::registry reg;
    auto e = makeAnimatedEntity(reg, h, WrapMode::Once);
    TimelineSystem::update(reg, lib, 0.5f);
    // QuadIn at u=0.5 → 0.25 → x = 2.5 (linear would be 5).
    CHECK(reg.get<TransformComponent>(e).position.x == Approx(2.5f));
}

TEST_CASE("TimelineSystem - events feed FSMEventQueue", "[timeline]") {
    ActionTimeline tl = makeMoveClip("evented");
    tl.events.push_back({ 0.5f, 1, "Halfway" });
    tl.recompute();
    AnimationClipLibrary lib;
    const TimelineHandle h = lib.addTimeline(std::move(tl));

    entt::registry reg;
    auto e = makeAnimatedEntity(reg, h, WrapMode::Loop);
    auto& queue = reg.emplace<FSMEventQueue>(e);

    TimelineSystem::update(reg, lib, 0.25f);// 0 → 0.25: nothing crossed
    CHECK(queue.empty());

    TimelineSystem::update(reg, lib, 0.5f);// 0.25 → 0.75: crossed 0.5
    REQUIRE(queue.events.size() == 1);
    CHECK(queue.events[0] == "Halfway");

    queue.clear();
    TimelineSystem::update(reg, lib, 0.5f);// 0.75 → 0.25 (wrapped): 0.5 not crossed
    CHECK(queue.empty());

    TimelineSystem::update(reg, lib, 0.5f);// 0.25 → 0.75: crossed again
    CHECK(queue.events.size() == 1);
}

TEST_CASE("TimelineSystem - group time scales", "[timeline]") {
    AnimationClipLibrary lib;
    const TimelineHandle h = lib.addTimeline(makeMoveClip());

    entt::registry reg;
    auto world = makeAnimatedEntity(reg, h, WrapMode::Once);
    auto ui = makeAnimatedEntity(reg, h, WrapMode::Once);
    reg.get<TimelinePlaybackComponent>(ui).groupId = 1;

    TimelineTimeScales scales;
    scales.setGroup(0, 0.0f);// pause the world group; group 1 keeps running

    TimelineSystem::update(reg, lib, 0.5f, &scales);
    CHECK(reg.get<TimelinePlaybackComponent>(world).time == Approx(0.0f));
    CHECK(reg.get<TimelinePlaybackComponent>(ui).time == Approx(0.5f));

    scales.global = 0.5f;
    scales.groups.clear();
    TimelineSystem::update(reg, lib, 0.5f, &scales);
    CHECK(reg.get<TimelinePlaybackComponent>(world).time == Approx(0.25f));
}

TEST_CASE("TimelineSystem - play/stop/seek helpers", "[timeline]") {
    AnimationClipLibrary lib;
    const TimelineHandle walk = lib.addTimeline(makeMoveClip("walk"));
    const TimelineHandle run = lib.addTimeline(makeMoveClip("run"));

    entt::registry reg;
    auto e = reg.create();
    reg.emplace<TransformComponent>(e);

    TimelineSystem::play(reg, e, walk);
    auto& play = reg.get<TimelinePlaybackComponent>(e);
    CHECK(play.clip == walk);
    CHECK(static_cast<bool>(play.playing));

    // Switching by name resolves among THIS entity's clips only.
    play.clips.push_back(run);
    CHECK(TimelineSystem::play(reg, e, lib, "run"));
    CHECK(play.clip == run);
    CHECK(!TimelineSystem::play(reg, e, lib, "not-here"));

    TimelineSystem::seek(reg, e, lib, 0.5f);
    CHECK(reg.get<TransformComponent>(e).position.x == Approx(5.0f));

    TimelineSystem::stop(reg, e);
    CHECK(!play.playing);
    CHECK(play.time == Approx(0.0f));
}

// ── Overlays + Tween (the ActionManager → timeline path) ─────────────────────

TEST_CASE("TimelineSystem - overlays layer and retire", "[timeline][overlay]") {
    AnimationClipLibrary lib;// overlays own their timelines, so no library needed here
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<TransformComponent>(e);

    // A 1s overlay moving X 0 → 10.
    ActionTimeline tl;
    ActionTrack track;
    track.property = ActionProperty::Position;
    track.keys.push_back({ 0.0f, glm::vec4(0.0f), EasingType::Linear });
    track.keys.push_back({ 1.0f, glm::vec4(10.0f, 0.0f, 0.0f, 0.0f), EasingType::Linear });
    tl.tracks.push_back(std::move(track));

    bool finished = false;
    const int id = TimelineSystem::addOverlay(reg, e, tl, [&] { finished = true; });
    CHECK(id == 1);

    TimelineSystem::update(reg, lib, 0.5f);
    CHECK(reg.get<TransformComponent>(e).position.x == Approx(5.0f));
    CHECK(!finished);

    TimelineSystem::update(reg, lib, 0.6f);// past the end → applies final, retires, fires onFinished
    CHECK(reg.get<TransformComponent>(e).position.x == Approx(10.0f));
    CHECK(finished);
    CHECK(reg.get<TimelineOverlayComponent>(e).overlays.empty());
}

TEST_CASE("TimelineSystem - cancelOverlay stops it advancing", "[timeline][overlay]") {
    AnimationClipLibrary lib;
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<TransformComponent>(e);

    ActionTimeline tl;
    ActionTrack track{ ActionProperty::Position, 0, {} };
    track.keys.push_back({ 0.0f, glm::vec4(0.0f), EasingType::Linear });
    track.keys.push_back({ 1.0f, glm::vec4(10.0f, 0.0f, 0.0f, 0.0f), EasingType::Linear });
    tl.tracks.push_back(std::move(track));

    const int id = TimelineSystem::addOverlay(reg, e, tl);
    TimelineSystem::cancelOverlay(reg, e, id);
    CHECK(reg.get<TimelineOverlayComponent>(e).overlays.empty());

    TimelineSystem::update(reg, lib, 0.5f);// no overlay → transform untouched
    CHECK(reg.get<TransformComponent>(e).position.x == Approx(0.0f));
}

TEST_CASE("Tween - builds and plays as an overlay from current state", "[timeline][tween]") {
    AnimationClipLibrary lib;
    entt::registry reg;
    auto e = reg.create();
    auto& tc = reg.emplace<TransformComponent>(e);
    tc.position = glm::vec3(2.0f, 0.0f, 0.0f);// current state — the tween's start key

    SECTION("sequential .then() chains durations") {
        ActionTimeline built = Tween(reg, e)
                                   .moveTo(0.5f, glm::vec3(4.0f, 0.0f, 0.0f))
                                   .then()
                                   .scaleTo(0.5f, glm::vec3(2.0f))
                                   .build();
        CHECK(built.duration == Approx(1.0f));// 0.5 move then 0.5 scale
    }

    SECTION("play() moves the transform from its build-time value") {
        bool done = false;
        Tween(reg, e).moveTo(1.0f, glm::vec3(12.0f, 0.0f, 0.0f)).play([&] { done = true; });

        TimelineSystem::update(reg, lib, 0.5f);
        // start key seeded at x=2, target x=12 → halfway = 7.
        CHECK(reg.get<TransformComponent>(e).position.x == Approx(7.0f));
        CHECK(!done);

        TimelineSystem::update(reg, lib, 0.6f);
        CHECK(reg.get<TransformComponent>(e).position.x == Approx(12.0f));
        CHECK(done);
    }

    SECTION(".with() parallels segments (max duration wins)") {
        ActionTimeline built =
            Tween(reg, e).moveTo(0.5f, glm::vec3(4.0f, 0.0f, 0.0f)).with().fadeTo(0.3f, 0.0f).build();
        CHECK(built.duration == Approx(0.5f));// parallel: the longer segment
        CHECK(built.tracks.size() == 2);      // position + alpha
    }
}

TEST_CASE("Tween - event fires into FSMEventQueue", "[timeline][tween]") {
    AnimationClipLibrary lib;
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<TransformComponent>(e);
    auto& queue = reg.emplace<FSMEventQueue>(e);

    Tween(reg, e).moveTo(1.0f, glm::vec3(1.0f, 0.0f, 0.0f)).event("arrived").play();

    TimelineSystem::update(reg, lib, 0.5f);// event at t=1.0 not crossed yet
    CHECK(queue.empty());

    TimelineSystem::update(reg, lib, 0.6f);// crosses t=1.0
    REQUIRE(queue.events.size() == 1);
    CHECK(queue.events[0] == "arrived");
}
