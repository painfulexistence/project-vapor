#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using Catch::Approx;

// ── Minimal TransformComponent stub (mirrors Vapor::TransformComponent) ────
// Does not link Vapor to avoid GPU / physics dependencies.
struct TransformComponent {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale    = glm::vec3(1.0f);
    glm::mat4 worldTransform = glm::mat4(1.0f);
    entt::entity parent = entt::null;
    bool isDirty = true;
};

// ── TransformSystem under test ─────────────────────────────────────────────
// Mirrors Vaporware/src/systems.hpp TransformSystem exactly.
struct TransformSystem {
    static void update(entt::registry& registry) {
        auto view = registry.view<TransformComponent>();
        for (auto entity : view) {
            auto& t = view.get<TransformComponent>(entity);
            if (!t.isDirty) continue;
            glm::mat4 local = glm::translate(glm::mat4(1.0f), t.position)
                            * glm::mat4_cast(t.rotation)
                            * glm::scale(glm::mat4(1.0f), t.scale);
            if (t.parent != entt::null) {
                if (auto* parentT = registry.try_get<TransformComponent>(t.parent)) {
                    t.worldTransform = parentT->worldTransform * local;
                } else {
                    t.worldTransform = local;
                }
            } else {
                t.worldTransform = local;
            }
            t.isDirty = false;
        }
    }
};

// ── Helpers ────────────────────────────────────────────────────────────────
static void check_mat4_approx(const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f) {
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            REQUIRE(a[col][row] == Approx(b[col][row]).epsilon(eps));
}

// ══════════════════════════════════════════════════════════════════════════
TEST_CASE("TransformSystem - identity entity", "[ecs][transform]") {
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<TransformComponent>(e); // all defaults: pos=0, rot=identity, scale=1

    TransformSystem::update(reg);

    auto& t = reg.get<TransformComponent>(e);
    check_mat4_approx(t.worldTransform, glm::mat4(1.0f));
    REQUIRE_FALSE(t.isDirty);
}

TEST_CASE("TransformSystem - translation only", "[ecs][transform]") {
    entt::registry reg;
    auto e = reg.create();
    auto& t = reg.emplace<TransformComponent>(e);
    t.position = glm::vec3(3.0f, 0.0f, 0.0f);

    TransformSystem::update(reg);

    glm::mat4 expected = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f));
    check_mat4_approx(reg.get<TransformComponent>(e).worldTransform, expected);
}

TEST_CASE("TransformSystem - rotation only", "[ecs][transform]") {
    entt::registry reg;
    auto e = reg.create();
    auto& t = reg.emplace<TransformComponent>(e);
    t.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    TransformSystem::update(reg);

    glm::mat4 expected = glm::mat4_cast(t.rotation);
    check_mat4_approx(reg.get<TransformComponent>(e).worldTransform, expected);
}

TEST_CASE("TransformSystem - non-uniform scale", "[ecs][transform]") {
    entt::registry reg;
    auto e = reg.create();
    auto& t = reg.emplace<TransformComponent>(e);
    t.scale = glm::vec3(2.0f, 3.0f, 4.0f);

    TransformSystem::update(reg);

    glm::mat4 expected = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 4.0f));
    check_mat4_approx(reg.get<TransformComponent>(e).worldTransform, expected);
}

TEST_CASE("TransformSystem - TRS combined order (T * R * S)", "[ecs][transform]") {
    entt::registry reg;
    auto e = reg.create();
    auto& t = reg.emplace<TransformComponent>(e);
    t.position = glm::vec3(1.0f, 2.0f, 3.0f);
    t.rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    t.scale    = glm::vec3(2.0f, 2.0f, 2.0f);

    TransformSystem::update(reg);

    glm::mat4 expected = glm::translate(glm::mat4(1.0f), t.position)
                       * glm::mat4_cast(t.rotation)
                       * glm::scale(glm::mat4(1.0f), t.scale);
    check_mat4_approx(reg.get<TransformComponent>(e).worldTransform, expected);
}

TEST_CASE("TransformSystem - isDirty=false skips recompute", "[ecs][transform]") {
    entt::registry reg;
    auto e = reg.create();
    auto& t = reg.emplace<TransformComponent>(e);
    t.position = glm::vec3(5.0f, 0.0f, 0.0f);
    t.isDirty  = false;
    // worldTransform stays at identity (default) because isDirty=false

    TransformSystem::update(reg);

    check_mat4_approx(reg.get<TransformComponent>(e).worldTransform, glm::mat4(1.0f));
}

TEST_CASE("TransformSystem - parent-child transform propagation", "[ecs][transform]") {
    entt::registry reg;

    auto parent = reg.create();
    auto& pt = reg.emplace<TransformComponent>(parent);
    pt.position = glm::vec3(10.0f, 0.0f, 0.0f);

    auto child = reg.create();
    auto& ct = reg.emplace<TransformComponent>(child);
    ct.position = glm::vec3(0.0f, 5.0f, 0.0f);
    ct.parent = parent;

    // Parent must be updated before child for worldTransform to propagate.
    // In a real scene this requires a topological pass; here we update twice.
    TransformSystem::update(reg); // parent gets worldTransform = T(10,0,0)
    reg.get<TransformComponent>(child).isDirty = true; // force child recalc
    TransformSystem::update(reg); // child = parent.worldTransform * T(0,5,0)

    glm::mat4 expectedParent = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    glm::mat4 expectedChild  = expectedParent * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f));

    check_mat4_approx(reg.get<TransformComponent>(parent).worldTransform, expectedParent);
    check_mat4_approx(reg.get<TransformComponent>(child).worldTransform,  expectedChild);
}

TEST_CASE("TransformSystem - parent destroyed, child falls back to local", "[ecs][transform]") {
    entt::registry reg;

    auto parent = reg.create();
    reg.emplace<TransformComponent>(parent).position = glm::vec3(99.0f, 0.0f, 0.0f);

    auto child = reg.create();
    auto& ct = reg.emplace<TransformComponent>(child);
    ct.position = glm::vec3(1.0f, 0.0f, 0.0f);
    ct.parent   = parent;

    reg.destroy(parent);
    TransformSystem::update(reg);

    // No parent in registry → falls back to local transform
    glm::mat4 expected = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    check_mat4_approx(reg.get<TransformComponent>(child).worldTransform, expected);
}

TEST_CASE("TransformSystem - isDirty cleared after update", "[ecs][transform]") {
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<TransformComponent>(e);

    TransformSystem::update(reg);
    REQUIRE_FALSE(reg.get<TransformComponent>(e).isDirty);
}

TEST_CASE("TransformSystem - multiple entities updated independently", "[ecs][transform]") {
    entt::registry reg;

    auto e1 = reg.create();
    reg.emplace<TransformComponent>(e1).position = glm::vec3(1.0f, 0.0f, 0.0f);

    auto e2 = reg.create();
    reg.emplace<TransformComponent>(e2).position = glm::vec3(0.0f, 2.0f, 0.0f);

    TransformSystem::update(reg);

    check_mat4_approx(reg.get<TransformComponent>(e1).worldTransform,
        glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    check_mat4_approx(reg.get<TransformComponent>(e2).worldTransform,
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f)));
}
