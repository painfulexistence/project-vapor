#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Vapor/physics_3d.hpp>
#include <Vapor/task_scheduler.hpp>
#include <Vapor/scene.hpp>
#include <glm/glm.hpp>

using namespace Vapor;
using Catch::Approx;

TEST_CASE("Physics3D Combined Tests", "[physics]") {
    TaskScheduler scheduler;
    scheduler.init(1);

    Physics3D physics;
    physics.init(scheduler);

    SECTION("Basic Operations") {
        BodyHandle body = physics.createSphereBody(1.0f, {0, 10, 0}, glm::quat(1, 0, 0, 0), BodyMotionType::Dynamic);
        REQUIRE(body.valid());
        physics.addBody(body, true);
        REQUIRE(physics.isActive(body));
        physics.destroyBody(body);
    }

    SECTION("Falling Body") {
        physics.setGravity({0, -10.0f, 0});

        auto scene = std::make_shared<Scene>();
        auto node = std::make_shared<Node>();
        node->name = "FallingSphere";
        scene->nodes.push_back(node);

        BodyHandle body = physics.createSphereBody(1.0f, {0, 10.0f, 0}, glm::quat(1, 0, 0, 0), BodyMotionType::Dynamic);
        node->body = body;
        physics.addBody(body, true);
        physics.setBodyUserData(body, reinterpret_cast<Uint64>(node.get()));

        for(int i = 0; i < 60; ++i) {
            physics.process(scene, 1.0f/60.0f);
        }

        glm::vec3 pos = physics.getPosition(body);
        REQUIRE(pos.y < 9.5f);
        REQUIRE(pos.y > 4.0f);
        
        physics.destroyBody(body);
    }

    physics.deinit();
    scheduler.shutdown();
}
