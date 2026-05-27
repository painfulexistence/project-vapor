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

    physics.deinit();
    scheduler.shutdown();
}
