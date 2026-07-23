#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entt/entt.hpp>
#include "Vapor/flipbook_system.hpp"

using namespace Vapor;
using Catch::Approx;

// Pure-logic tests: FlipbookClip sampling + FlipbookSystem playback, no GPU.

TEST_CASE("FlipbookClip - builders and duration", "[flipbook]") {
    SECTION("fromIndices keeps the given order and uniform duration") {
        FlipbookClip c = FlipbookClip::fromIndices("walk", { 4, 5, 6, 7 }, 0.1f);
        REQUIRE(c.frames.size() == 4);
        CHECK(c.frames[0].frameIndex == 4);
        CHECK(c.frames[3].frameIndex == 7);
        CHECK(c.duration == Approx(0.4f));
    }
    SECTION("fromRange enumerates consecutive indices") {
        FlipbookClip c = FlipbookClip::fromRange("run", 8, 4, 0.05f);
        REQUIRE(c.frames.size() == 4);
        CHECK(c.frames[0].frameIndex == 8);
        CHECK(c.frames[3].frameIndex == 11);
        CHECK(c.duration == Approx(0.2f));
    }
}

TEST_CASE("FlipbookClip - sampling maps time to frame", "[flipbook]") {
    FlipbookClip c = FlipbookClip::fromIndices("walk", { 4, 5, 6, 7 }, 0.1f);// slots at [0,.1)(.1,.2)...
    int slot = -99;

    CHECK(c.sample(0.0f, &slot) == 4);
    CHECK(slot == 0);
    CHECK(c.sample(0.15f, &slot) == 5);
    CHECK(slot == 1);
    CHECK(c.sample(0.25f, &slot) == 6);
    CHECK(slot == 2);
    // At/after the end, clamp to the last frame.
    CHECK(c.sample(1.0f, &slot) == 7);
    CHECK(slot == 3);

    SECTION("empty clip reports slot -1") {
        FlipbookClip empty;
        CHECK(empty.sample(0.5f, &slot) == 0);
        CHECK(slot == -1);
    }
}

namespace {
    entt::entity makeSpriteEntity(entt::registry& reg, FlipbookClipHandle h, WrapMode wrap) {
        auto e = reg.create();
        reg.emplace<Sprite2DComponent>(e);
        FlipbookComponent fb;
        fb.clip = h;
        fb.clips.push_back(h);
        fb.wrap = wrap;
        fb.playing = true;
        reg.emplace<FlipbookComponent>(e, std::move(fb));
        return e;
    }
}// namespace

TEST_CASE("FlipbookSystem - drives the sprite frame", "[flipbook]") {
    AnimationClipLibrary lib;
    const FlipbookClipHandle h = lib.addFlipbook(FlipbookClip::fromIndices("walk", { 4, 5, 6, 7 }, 0.1f));

    entt::registry reg;
    auto e = makeSpriteEntity(reg, h, WrapMode::Loop);

    // First update samples frame 0 → atlas index 4.
    FlipbookSystem::update(reg, lib, 0.0f);
    CHECK(reg.get<Sprite2DComponent>(e).frameIndex == 4);

    // Advance into the second frame.
    FlipbookSystem::update(reg, lib, 0.12f);
    CHECK(reg.get<Sprite2DComponent>(e).frameIndex == 5);

    SECTION("Loop wraps back to the first frame") {
        FlipbookSystem::update(reg, lib, 0.4f);// 0.12 → 0.52 → wraps to 0.12 → frame slot 1
        CHECK(reg.get<FlipbookComponent>(e).time == Approx(0.12f));
    }

    SECTION("Once stops on the last frame") {
        auto once = makeSpriteEntity(reg, h, WrapMode::Once);
        FlipbookSystem::update(reg, lib, 10.0f);
        CHECK(reg.get<Sprite2DComponent>(once).frameIndex == 7);
        CHECK(!reg.get<FlipbookComponent>(once).playing);
    }
}

TEST_CASE("FlipbookSystem - group time scale freezes playback", "[flipbook]") {
    AnimationClipLibrary lib;
    const FlipbookClipHandle h = lib.addFlipbook(FlipbookClip::fromRange("idle", 0, 4, 0.1f));

    entt::registry reg;
    auto e = makeSpriteEntity(reg, h, WrapMode::Loop);
    reg.get<FlipbookComponent>(e).groupId = 3;

    TimelineTimeScales scales;
    scales.setGroup(3, 0.0f);// freeze group 3
    FlipbookSystem::update(reg, lib, 0.5f, &scales);
    CHECK(reg.get<FlipbookComponent>(e).time == Approx(0.0f));
}

TEST_CASE("FlipbookSystem - play/stop helpers", "[flipbook]") {
    AnimationClipLibrary lib;
    const FlipbookClipHandle walk = lib.addFlipbook(FlipbookClip::fromRange("walk", 0, 4, 0.1f));
    const FlipbookClipHandle run = lib.addFlipbook(FlipbookClip::fromRange("run", 8, 4, 0.05f));

    entt::registry reg;
    auto e = reg.create();
    reg.emplace<Sprite2DComponent>(e);

    FlipbookSystem::play(reg, e, walk);
    CHECK(reg.get<FlipbookComponent>(e).clip == walk);

    reg.get<FlipbookComponent>(e).clips.push_back(run);
    CHECK(FlipbookSystem::play(reg, e, lib, "run"));
    CHECK(reg.get<FlipbookComponent>(e).clip == run);
    CHECK(!FlipbookSystem::play(reg, e, lib, "missing"));

    FlipbookSystem::stop(reg, e);
    CHECK(!reg.get<FlipbookComponent>(e).playing);
    CHECK(reg.get<FlipbookComponent>(e).time == Approx(0.0f));
}
