/**
 * ECS Example - Demonstrates how to use the new EnTT-based ECS architecture
 *
 * This example shows:
 * - Creating entities with components
 * - Setting up transforms and hierarchies
 * - Adding physics bodies
 * - Adding mesh renderers
 * - Using systems to update the scene
 * - Direct registry access for advanced usage
 */

#include "Vapor/scene_ecs.hpp"
#include "Vapor/ecs_components.hpp"
#include "Vapor/ecs_systems.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/graphics.hpp"
#include <fmt/core.h>

using namespace Vapor;

void basicExample(ECSScene& scene) {
    fmt::print("\n=== Basic Example: Creating Entities ===\n");

    // Create a simple entity with transform
    auto player = scene.createEntity("Player");
    scene.addTransform(player, glm::vec3(0, 1, 0));

    // Create an entity with mesh
    auto cube = scene.createEntity("Cube");
    scene.addTransform(cube, glm::vec3(2, 0, 0));

    auto material = std::make_shared<Material>();
    auto mesh = MeshBuilder::buildCube(1.0f, material);
    scene.addMeshRenderer(cube, mesh);

    fmt::print("Created entities: Player, Cube\n");
}

void hierarchyExample(ECSScene& scene) {
    fmt::print("\n=== Hierarchy Example: Parent-Child Relationships ===\n");

    // Create parent
    auto parent = scene.createEntity("Parent");
    scene.addTransform(parent, glm::vec3(0, 0, 0));

    // Create children
    auto child1 = scene.createEntity("Child1");
    scene.addTransform(child1, glm::vec3(1, 0, 0));
    scene.setParent(child1, parent);

    auto child2 = scene.createEntity("Child2");
    scene.addTransform(child2, glm::vec3(-1, 0, 0));
    scene.setParent(child2, parent);

    // When parent moves, children move with it
    auto& parentTransform = scene.getRegistry().get<Transform>(parent);
    parentTransform.translate(glm::vec3(0, 5, 0));

    scene.updateTransforms();

    fmt::print("Created hierarchy: Parent -> Child1, Child2\n");
}

void physicsExample(ECSScene& scene, Physics3D* physics) {
    fmt::print("\n=== Physics Example: Dynamic Bodies ===\n");

    // Create a dynamic cube with physics
    auto dynamicCube = scene.createEntity("DynamicCube");

    // Add transform
    scene.addTransform(dynamicCube,
                      glm::vec3(0, 5, 0),
                      glm::quat(1, 0, 0, 0),
                      glm::vec3(1, 1, 1));

    // Create physics body
    auto body = physics->createBoxBody(
        glm::vec3(0.5f, 0.5f, 0.5f),
        glm::vec3(0, 5, 0),
        glm::quat(1, 0, 0, 0),
        BodyMotionType::Dynamic
    );
    physics->addBody(body, true);

    // Add physics component
    scene.addRigidBody(dynamicCube, body, 1.0f);

    // Add mesh
    auto material = std::make_shared<Material>();
    auto mesh = MeshBuilder::buildCube(1.0f, material);
    scene.addMeshRenderer(dynamicCube, mesh);

    fmt::print("Created dynamic cube with physics\n");
}

void systemsExample(ECSScene& scene, Physics3D* physics, float deltaTime) {
    fmt::print("\n=== Systems Example: Updating the World ===\n");

    // Update transforms (handles hierarchy propagation)
    scene.updateTransforms();

    // Update physics (using the new PhysicsSystem)
    PhysicsSystem physicsSystem(physics);
    physicsSystem.update(scene.getRegistry(), deltaTime);

    fmt::print("Updated all systems\n");
}

void advancedExample(ECSScene& scene) {
    fmt::print("\n=== Advanced Example: Direct Registry Access ===\n");

    // Get direct access to the registry for advanced operations
    auto& registry = scene.getRegistry();

    // Create multiple entities at once
    for (int i = 0; i < 10; i++) {
        auto entity = scene.createEntity(fmt::format("Entity_{}", i));
        scene.addTransform(entity,
                          glm::vec3(i * 2.0f, 0, 0),
                          glm::quat(1, 0, 0, 0),
                          glm::vec3(1, 1, 1));
    }

    // Iterate over all entities with Transform component
    auto view = registry.view<Transform, Name>();
    fmt::print("Entities with Transform:\n");
    for (auto entity : view) {
        auto& name = view.get<Name>(entity);
        auto& transform = view.get<Transform>(entity);
        fmt::print("  - {}: pos=({:.1f}, {:.1f}, {:.1f})\n",
                  name.value,
                  transform.position.x,
                  transform.position.y,
                  transform.position.z);
    }

    // Filter entities by multiple components
    auto renderView = registry.view<Transform, MeshRenderer, Visible>();
    fmt::print("Visible entities with mesh: {}\n", renderView.size_hint());
}

void gameplayExample(ECSScene& scene) {
    fmt::print("\n=== Gameplay Example: Easy to Write Game Logic ===\n");

    // Create a player entity with custom components
    auto player = scene.createEntity("Player");
    auto& playerTransform = scene.addTransform(player, glm::vec3(0, 1, 0));

    // Easy gameplay: Move player based on input
    glm::vec3 inputDirection(1, 0, 0); // Simulate input
    float moveSpeed = 5.0f;
    float deltaTime = 0.016f;

    playerTransform.translate(inputDirection * moveSpeed * deltaTime);

    // Create enemies in a circle around player
    int numEnemies = 8;
    for (int i = 0; i < numEnemies; i++) {
        float angle = (float)i / numEnemies * 2.0f * 3.14159f;
        float radius = 5.0f;

        auto enemy = scene.createEntity(fmt::format("Enemy_{}", i));
        scene.addTransform(enemy,
                          glm::vec3(cos(angle) * radius, 0, sin(angle) * radius));

        // Enemies could have additional custom components here
        // registry.emplace<EnemyAI>(enemy);
        // registry.emplace<Health>(enemy, 100.0f);
    }

    fmt::print("Created player and {} enemies\n", numEnemies);

    // Easy iteration: Update all enemies
    auto& registry = scene.getRegistry();
    auto enemies = registry.view<Transform, Name>();

    for (auto entity : enemies) {
        auto& name = enemies.get<Name>(entity);
        if (name.value.find("Enemy") != std::string::npos) {
            auto& enemyTransform = enemies.get<Transform>(entity);

            // Simple AI: move towards player
            glm::vec3 toPlayer = playerTransform.position - enemyTransform.position;
            float distance = glm::length(toPlayer);

            if (distance > 0.1f) {
                glm::vec3 direction = glm::normalize(toPlayer);
                enemyTransform.translate(direction * 2.0f * deltaTime);
            }
        }
    }

    fmt::print("Updated enemy AI\n");
}

int main() {
    fmt::print("=== EnTT ECS Integration Examples ===\n");

    // Create ECS scene
    ECSScene scene("Example Scene");

    // Initialize physics (simplified for example)
    auto physics = std::make_unique<Physics3D>();
    // physics->init(taskScheduler); // Would need task scheduler in real app

    // Run examples
    basicExample(scene);
    hierarchyExample(scene);
    // physicsExample(scene, physics.get()); // Uncomment when physics is initialized
    advancedExample(scene);
    gameplayExample(scene);

    // Print scene info
    fmt::print("\n");
    scene.print();

    fmt::print("\n=== Key Benefits of ECS Architecture ===\n");
    fmt::print("1. Easy to write gameplay code - just add components and iterate\n");
    fmt::print("2. Data-oriented design - better cache performance\n");
    fmt::print("3. Flexible composition - mix and match components\n");
    fmt::print("4. Clear separation of concerns - components are data, systems are logic\n");
    fmt::print("5. Scalable - handles thousands of entities efficiently\n");

    return 0;
}
