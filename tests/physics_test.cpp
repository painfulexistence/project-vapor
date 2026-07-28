#include <Vapor/physics_3d.hpp>
#include <Vapor/task_scheduler.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

using namespace Vapor;
using Catch::Approx;

TEST_CASE("Physics3D Combined Tests", "[physics]") {
    TaskScheduler scheduler;
    scheduler.init(1);

    Physics3D physics;
    physics.init(scheduler);

    SECTION("Basic Operations") {
        BodyHandle body = physics.createSphereBody(1.0f, { 0, 10, 0 }, glm::quat(1, 0, 0, 0), BodyMotionType::Dynamic);
        REQUIRE(body.valid());
        physics.addBody(body, true);
        REQUIRE(physics.isActive(body));
        physics.destroyBody(body);
    }

    SECTION("Falling Body") {
        physics.setGravity({ 0, -10.0f, 0 });

        BodyHandle body =
            physics.createSphereBody(1.0f, { 0, 10.0f, 0 }, glm::quat(1, 0, 0, 0), BodyMotionType::Dynamic);
        physics.addBody(body, true);

        for (int i = 0; i < 60; ++i) {
            physics.process(1.0f / 60.0f);
        }

        glm::vec3 pos = physics.getPosition(body);
        REQUIRE(pos.y < 9.5f);
        REQUIRE(pos.y > 4.0f);

        physics.destroyBody(body);
    }

    SECTION("Thin boxes keep their authored size instead of throwing") {
        // Jolt refuses a box whose smallest half-extent is under the shape's
        // convex radius (0.05 by default). A 4 cm handrail collider used to
        // take the whole app down with "Failed to create box shape"; the fix
        // shrinks the convex radius to fit rather than inflating the box.
        const glm::vec3 handrail(0.04f, 0.04f, 1.2f);
        BodyHandle thin;
        REQUIRE_NOTHROW(thin = physics.createBoxBody(handrail, { 0, 1, 0 }, glm::quat(1, 0, 0, 0),
                                                     BodyMotionType::Static));
        REQUIRE(thin.valid());
        physics.addBody(thin, false);

        // Thinner still, and degenerate on one axis — both must survive, since
        // procgen emits colliders straight from authored geometry.
        BodyHandle sliver, flat;
        REQUIRE_NOTHROW(sliver = physics.createBoxBody({ 0.001f, 0.5f, 0.5f }, { 3, 1, 0 },
                                                       glm::quat(1, 0, 0, 0), BodyMotionType::Static));
        REQUIRE_NOTHROW(flat = physics.createBoxBody({ 0.0f, 0.5f, 0.5f }, { 6, 1, 0 },
                                                     glm::quat(1, 0, 0, 0), BodyMotionType::Static));
        REQUIRE(sliver.valid());
        REQUIRE(flat.valid());

        // The thin box is still solid: a body dropped onto it comes to rest on
        // top rather than falling through, which is what inflating the extents
        // would have quietly changed.
        physics.setGravity({ 0, -10.0f, 0 });
        BodyHandle faller = physics.createSphereBody(0.2f, { 0, 1.6f, 0 }, glm::quat(1, 0, 0, 0),
                                                     BodyMotionType::Dynamic);
        physics.addBody(faller, true);
        for (int i = 0; i < 120; ++i) physics.process(1.0f / 60.0f);
        const float restY = physics.getPosition(faller).y;
        // Rail top is 1.04, plus the 0.2 sphere radius.
        REQUIRE(restY > 1.1f);
        REQUIRE(restY < 1.4f);

        physics.destroyBody(faller);
        physics.destroyBody(flat);
        physics.destroyBody(sliver);
        physics.destroyBody(thin);
    }

    physics.deinit();
    scheduler.shutdown();
}
