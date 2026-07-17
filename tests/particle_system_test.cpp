// Particle ECS system tests — exercise the REAL Vapor::ParticleEmitterSystem /
// ParticleRenderSystem / ParticleForceFieldSystem against a recording mock
// IRenderer. Deliberately NOT a mirrored stub: these link the actual headers so
// a regression that deletes or breaks the systems fails the build/tests (the
// particle integration was silently reverted once by an unrelated merge).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "Vapor/components.hpp"
#include "Vapor/irenderer.hpp"
#include "Vapor/render_data.hpp"
#include "Vapor/systems.hpp"

using Catch::Approx;
using namespace Vapor;

// ── Recording mock renderer ────────────────────────────────────────────────
// Implements only the particle slot API (everything else in IRenderer has a
// default no-op). A first-fit free list mirrors the real allocator's contract:
// sequential non-overlapping ranges, reuse after release, ~0u when exhausted.
struct MockRenderer : IRenderer {
    struct Range { uint32_t begin, count; };
    std::vector<Range> freeList;

    int claimCalls = 0, releaseCalls = 0, uploadCalls = 0;
    uint32_t totalUploaded = 0, lastUploadCount = 0, lastUploadBegin = ~0u;
    std::vector<Range> releases;
    ParticleForceField lastForceField;
    std::vector<ParticleDrawPacket> lastDrawList;
    bool drawListSet = false;

    explicit MockRenderer(uint32_t pool = 1'000'000u) { freeList.push_back({0u, pool}); }

    uint32_t claimParticleSlots(uint32_t count) override {
        ++claimCalls;
        for (auto& r : freeList) {
            if (r.count >= count) {
                uint32_t begin = r.begin;
                r.begin += count;
                r.count -= count;
                return begin;
            }
        }
        return ~0u;
    }
    void releaseParticleSlots(uint32_t begin, uint32_t count) override {
        ++releaseCalls;
        releases.push_back({begin, count});
        freeList.push_back({begin, count});  // no coalesce needed for tests
    }
    void uploadParticles(uint32_t begin, const std::vector<GPUParticleData>& p) override {
        ++uploadCalls;
        lastUploadBegin = begin;
        lastUploadCount = static_cast<uint32_t>(p.size());
        totalUploaded += static_cast<uint32_t>(p.size());
    }
    void setParticleForceField(const ParticleForceField& f) override { lastForceField = f; }
    void setParticleDrawList(const std::vector<ParticleDrawPacket>& d) override {
        lastDrawList = d;
        drawListSet = true;
    }
};

// Convenience: an emitter entity (always needs a TransformComponent — the
// emitter view requires it) with the config applied.
template <typename Fn>
static entt::entity makeEmitter(entt::registry& reg, glm::vec3 pos, Fn&& configure) {
    auto e = reg.create();
    reg.emplace<TransformComponent>(e).position = pos;
    auto& em = reg.emplace<ParticleEmitterComponent>(e);
    configure(em);
    return e;
}

// Read helpers cast the Hidden<T> runtime fields to plain types so Catch2's
// expression decomposition/stringification stays happy.
static uint32_t slotBegin(const ParticleEmitterComponent& e) { return static_cast<uint32_t>(e._slotBegin); }
static uint32_t slotCount(const ParticleEmitterComponent& e) { return static_cast<uint32_t>(e._slotCount); }
static bool     cleared(const ParticleEmitterComponent& e)   { return static_cast<bool>(e._cleared); }
static bool     hasFired(const ParticleEmitterComponent& e)  { return static_cast<bool>(e._hasFired); }
static float    reclaim(const ParticleEmitterComponent& e)   { return static_cast<float>(e._reclaimTimer); }

// ============================================================================
// ParticleEmitterSystem — the state machine
// ============================================================================

TEST_CASE("Continuous emitter claims slots and uploads by accumulator", "[particle][emitter]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = false; em.emitRate = 30.0f; em.maxParticles = 100; em.particleLifetime = 2.0f;
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f);  // 30 * 1.0 = 30 spawns

    auto& em = reg.get<ParticleEmitterComponent>(e);
    REQUIRE(mock.claimCalls == 1);
    REQUIRE(slotBegin(em) != ~0u);
    REQUIRE(slotCount(em) == 100);
    REQUIRE(mock.uploadCalls >= 1);
    REQUIRE(mock.lastUploadCount == 30);
    REQUIRE(cleared(em) == false);  // holds live particles now
}

TEST_CASE("One-shot fires the whole batch once, never touches enabled", "[particle][emitter][oneshot]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = true; em.maxParticles = 50; em.particleLifetime = 2.0f;
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);
    auto& em = reg.get<ParticleEmitterComponent>(e);

    REQUIRE(hasFired(em) == true);
    REQUIRE(em.enabled == true);           // gameplay-owned, system must not flip it
    REQUIRE(mock.uploadCalls == 1);
    REQUIRE(mock.lastUploadCount == 50);   // whole batch in one frame
    REQUIRE(reclaim(em) == Approx(2.0f));  // drain armed for finite lifetime

    // A fired one-shot spawns nothing further.
    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);
    REQUIRE(mock.uploadCalls == 1);
}

TEST_CASE("Immortal one-shot never arms reclaim, keeps its slots", "[particle][emitter][oneshot]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = true; em.maxParticles = 50; em.particleLifetime = -1.0f;  // immortal
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);
    auto& em = reg.get<ParticleEmitterComponent>(e);

    REQUIRE(hasFired(em) == true);
    REQUIRE(reclaim(em) < 0.0f);        // not armed — immortal particles never die
    REQUIRE(slotBegin(em) != ~0u);      // slots held indefinitely
}

TEST_CASE("enabled=false clears immediately regardless of lifetime", "[particle][emitter][clear]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = false; em.emitRate = 30.0f; em.maxParticles = 100; em.particleLifetime = -1.0f;
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f);  // claim + emit
    auto& em = reg.get<ParticleEmitterComponent>(e);
    const uint32_t heldBegin = slotBegin(em), heldCount = slotCount(em);
    REQUIRE(heldBegin != ~0u);

    em.enabled = false;
    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);

    REQUIRE(mock.releaseCalls == 1);
    REQUIRE(mock.releases.back().begin == heldBegin);
    REQUIRE(mock.releases.back().count == heldCount);
    REQUIRE(slotBegin(em) == ~0u);    // reset
    REQUIRE(cleared(em) == true);
    REQUIRE(hasFired(em) == false);   // reset so a re-enable starts fresh
}

TEST_CASE("Re-enabling a cleared one-shot re-fires", "[particle][emitter][clear]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = true; em.maxParticles = 40; em.particleLifetime = -1.0f;
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);  // fire #1
    auto& em = reg.get<ParticleEmitterComponent>(e);
    REQUIRE(mock.uploadCalls == 1);

    em.enabled = false;
    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);  // clear
    em.enabled = true;
    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);  // fire #2

    REQUIRE(mock.uploadCalls == 2);
    REQUIRE(hasFired(em) == true);
}

TEST_CASE("emitting=false drains gracefully then reclaims after lifetime", "[particle][emitter][stop]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = false; em.emitRate = 60.0f; em.maxParticles = 100; em.particleLifetime = 0.5f;
    });

    ParticleEmitterSystem::update(reg, &mock, 0.1f);  // claim + emit
    auto& em = reg.get<ParticleEmitterComponent>(e);
    const uint32_t uploadsBefore = mock.uploadCalls;

    em.emitting = false;
    ParticleEmitterSystem::update(reg, &mock, 0.1f);  // graceful stop: no new spawn, drain armed
    REQUIRE(mock.uploadCalls == uploadsBefore);       // stopped spawning
    REQUIRE(reclaim(em) == Approx(0.5f));             // armed to full lifetime this frame
    REQUIRE(slotBegin(em) != ~0u);                    // still holds slots while draining

    ParticleEmitterSystem::update(reg, &mock, 1.0f);  // past lifetime -> reclaim
    REQUIRE(mock.releaseCalls >= 1);
    REQUIRE(slotBegin(em) == ~0u);
    REQUIRE(cleared(em) == true);
}

TEST_CASE("Finite one-shot slots are auto-reclaimed after particles age out", "[particle][emitter][reclaim]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = true; em.maxParticles = 50; em.particleLifetime = 0.5f;
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);  // fire, arm reclaim = 0.5
    auto& em = reg.get<ParticleEmitterComponent>(e);
    REQUIRE(slotBegin(em) != ~0u);

    ParticleEmitterSystem::update(reg, &mock, 0.6f);  // past 0.5 -> reclaim
    REQUIRE(mock.releaseCalls == 1);
    REQUIRE(slotBegin(em) == ~0u);
    REQUIRE(cleared(em) == true);
}

TEST_CASE("Changing maxParticles re-claims at runtime", "[particle][emitter][resize]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = false; em.emitRate = 10.0f; em.maxParticles = 100; em.particleLifetime = -1.0f;
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f);
    auto& em = reg.get<ParticleEmitterComponent>(e);
    REQUIRE(slotCount(em) == 100);
    const int releasesBefore = mock.releaseCalls;

    em.maxParticles = 200;
    ParticleEmitterSystem::update(reg, &mock, 1.0f);

    REQUIRE(mock.releaseCalls == releasesBefore + 1);  // freed old range
    REQUIRE(slotCount(em) == 200);
    REQUIRE(slotBegin(em) != ~0u);
}

TEST_CASE("Global emission stop pauses spawning but keeps slots", "[particle][emitter][global-stop]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = false; em.emitRate = 30.0f; em.maxParticles = 100; em.particleLifetime = -1.0f;
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f, /*emissionEnabled=*/true);
    auto& em = reg.get<ParticleEmitterComponent>(e);
    const uint32_t uploadsBefore = mock.uploadCalls;

    ParticleEmitterSystem::update(reg, &mock, 1.0f, /*emissionEnabled=*/false);
    REQUIRE(mock.uploadCalls == uploadsBefore);  // no new spawns
    REQUIRE(slotBegin(em) != ~0u);               // slots kept (pause, not stop)
    REQUIRE(reclaim(em) < 0.0f);                 // not draining
}

TEST_CASE("_cleared reflects live-particle ownership", "[particle][emitter][cleared]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = false; em.emitRate = 30.0f; em.maxParticles = 100; em.particleLifetime = -1.0f;
    });
    auto& em = reg.get<ParticleEmitterComponent>(e);
    REQUIRE(cleared(em) == true);  // fresh: nothing emitted yet

    ParticleEmitterSystem::update(reg, &mock, 1.0f);
    REQUIRE(cleared(em) == false);  // holds live particles

    em.enabled = false;
    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);
    REQUIRE(cleared(em) == true);   // all gone
}

TEST_CASE("Pool exhaustion is handled without upload", "[particle][emitter][pool]") {
    entt::registry reg;
    MockRenderer mock(/*pool=*/10u);  // tiny pool
    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = true; em.maxParticles = 100; em.particleLifetime = 1.0f;  // > pool
    });

    ParticleEmitterSystem::update(reg, &mock, 1.0f / 60.0f);
    auto& em = reg.get<ParticleEmitterComponent>(e);

    REQUIRE(slotBegin(em) == ~0u);      // claim failed
    REQUIRE(mock.uploadCalls == 0);     // nothing uploaded
}

TEST_CASE("Destroying an emitter entity releases its slots", "[particle][emitter][destroy]") {
    // mock declared before reg so it outlives the registry — the on_destroy hook
    // (which dereferences the mock) fires during reg teardown too.
    MockRenderer mock;
    entt::registry reg;
    ParticleEmitterSystem::attach(reg, &mock);  // wires on_destroy via registry ctx

    auto e = makeEmitter(reg, {0, 0, 0}, [](ParticleEmitterComponent& em) {
        em.oneShot = false; em.emitRate = 10.0f; em.maxParticles = 100; em.particleLifetime = -1.0f;
    });
    ParticleEmitterSystem::update(reg, &mock, 1.0f);  // claim
    const uint32_t heldBegin = slotBegin(reg.get<ParticleEmitterComponent>(e));
    REQUIRE(heldBegin != ~0u);

    reg.destroy(e);
    REQUIRE(mock.releaseCalls == 1);
    REQUIRE(mock.releases.back().begin == heldBegin);
}

// ============================================================================
// ParticleRenderSystem — per-emitter draw packets
// ============================================================================

TEST_CASE("Render system builds a packet per slot-holding emitter", "[particle][render]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = reg.create();
    auto& em = reg.emplace<ParticleEmitterComponent>(e);
    em._slotBegin = 5u;
    em._slotCount = 100u;
    auto& pr = reg.emplace<ParticleRendererComponent>(e);
    pr.blendMode = ParticleBlendMode::AlphaBlend;
    pr.texture = 7u;
    pr.size = 0.3f;

    ParticleRenderSystem::update(reg, &mock);

    REQUIRE(mock.drawListSet);
    REQUIRE(mock.lastDrawList.size() == 1);
    const auto& p = mock.lastDrawList.front();
    REQUIRE(p.slotBegin == 5u);
    REQUIRE(p.slotCount == 100u);
    REQUIRE(static_cast<int>(p.blendMode) == static_cast<int>(ParticleBlendMode::AlphaBlend));
    REQUIRE(p.texture == 7u);
    REQUIRE(p.size == Approx(0.3f));
}

TEST_CASE("Render system uses defaults without a renderer component", "[particle][render]") {
    entt::registry reg;
    MockRenderer mock;
    auto e = reg.create();
    auto& em = reg.emplace<ParticleEmitterComponent>(e);
    em._slotBegin = 0u;
    em._slotCount = 64u;

    ParticleRenderSystem::update(reg, &mock);

    REQUIRE(mock.lastDrawList.size() == 1);
    const auto& p = mock.lastDrawList.front();
    REQUIRE(static_cast<int>(p.blendMode) == static_cast<int>(ParticleBlendMode::Additive));
    REQUIRE(p.texture == INVALID_TEXTURE_ID);
    REQUIRE(p.size == Approx(0.1f));
}

TEST_CASE("Render system skips emitters without claimed slots", "[particle][render]") {
    entt::registry reg;
    MockRenderer mock;
    reg.emplace<ParticleEmitterComponent>(reg.create());  // _slotBegin defaults to ~0u

    ParticleRenderSystem::update(reg, &mock);

    REQUIRE(mock.drawListSet);
    REQUIRE(mock.lastDrawList.empty());
}

TEST_CASE("Render system caps the draw list at MAX_PARTICLE_DRAWS", "[particle][render]") {
    entt::registry reg;
    MockRenderer mock;
    for (uint32_t i = 0; i < MAX_PARTICLE_DRAWS + 10; ++i) {
        auto e = reg.create();
        auto& em = reg.emplace<ParticleEmitterComponent>(e);
        em._slotBegin = i * 100u;
        em._slotCount = 100u;
    }

    ParticleRenderSystem::update(reg, &mock);
    REQUIRE(mock.lastDrawList.size() == MAX_PARTICLE_DRAWS);
}

// ============================================================================
// ParticleForceFieldSystem — attractors + wind
// ============================================================================

TEST_CASE("Force field gathers attractors from components", "[particle][force]") {
    entt::registry reg;
    MockRenderer mock;
    auto a = reg.create();
    reg.emplace<TransformComponent>(a).position = {1, 2, 3};
    reg.emplace<ParticleAttractorComponent>(a).strength = 12.0f;
    auto b = reg.create();
    reg.emplace<TransformComponent>(b).position = {-4, 0, 5};
    reg.emplace<ParticleAttractorComponent>(b).strength = 8.0f;

    ParticleForceFieldSystem::update(reg, &mock);

    REQUIRE(mock.lastForceField.attractors.size() == 2);
    // Order follows entity iteration; check the set of strengths is present.
    float s0 = mock.lastForceField.attractors[0].strength;
    float s1 = mock.lastForceField.attractors[1].strength;
    REQUIRE(((s0 == Approx(12.0f) && s1 == Approx(8.0f)) ||
             (s0 == Approx(8.0f) && s1 == Approx(12.0f))));
}

TEST_CASE("Force field gathers wind and turbulence", "[particle][force]") {
    entt::registry reg;
    MockRenderer mock;
    auto w = reg.create();
    auto& wf = reg.emplace<WindFieldComponent>(w);
    wf.direction = glm::vec3(1, 0, 0);
    wf.strength = 0.6f;
    wf.turbulence = 2.0f;

    ParticleForceFieldSystem::update(reg, &mock);

    REQUIRE(mock.lastForceField.wind.x == Approx(1.0f));
    REQUIRE(mock.lastForceField.wind.w == Approx(0.6f));
    REQUIRE(mock.lastForceField.turbulence == Approx(2.0f));
}

TEST_CASE("Force field caps attractors at MAX_PARTICLE_ATTRACTORS", "[particle][force]") {
    entt::registry reg;
    MockRenderer mock;
    for (uint32_t i = 0; i < MAX_PARTICLE_ATTRACTORS + 5; ++i) {
        auto a = reg.create();
        reg.emplace<TransformComponent>(a);
        reg.emplace<ParticleAttractorComponent>(a).strength = 1.0f;
    }

    ParticleForceFieldSystem::update(reg, &mock);
    REQUIRE(mock.lastForceField.attractors.size() == MAX_PARTICLE_ATTRACTORS);
}
