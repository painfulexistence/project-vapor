/**
 * Physics Demo - Demonstrates CharacterController, VehicleController, and FluidVolume
 *
 * Controls:
 * - WASD: Move character
 * - Space: Jump
 * - Arrow Keys: Control vehicle
 * - C: Switch between character and vehicle control
 * - Camera: IJKL (pan/tilt), RF (pedestal), UO (roll)
 */

#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>

#include "Vapor/scene.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/character_controller.hpp"
#include "Vapor/vehicle_controller.hpp"
#include "Vapor/fluid_volume.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/camera.hpp"
#include "Vapor/engine_core.hpp"

int main(int argc, char* args[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print("SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    const char* winTitle = "Jolt Physics Demo - Character, Vehicle & Fluid";
    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
    GraphicsBackend gfxBackend = GraphicsBackend::Vulkan;

    auto window = SDL_CreateWindow(winTitle, 1920, 1080, winFlags);

    // Initialize engine core
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();
    fmt::print("Engine core initialized\n");

    auto renderer = createRenderer(gfxBackend);
    renderer->init(window);

    // Initialize physics
    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler());
    physics->setGravity(glm::vec3(0.0f, -9.81f, 0.0f));

    // Create scene
    auto scene = std::make_shared<Scene>("Physics Demo");

    // Create simple material
    auto material = std::make_shared<Material>();

    // ====== 創建地面 ======
    auto floor = scene->createNode("Floor");
    scene->addMeshToNode(floor, MeshBuilder::buildCube(1.0f, material));
    floor->setPosition(glm::vec3(0.0f, -0.5f, 0.0f));
    floor->setLocalScale(glm::vec3(50.0f, 0.5f, 50.0f));
    floor->body = physics->createBoxBody(
        glm::vec3(50.0f, 0.5f, 50.0f),
        glm::vec3(0.0f, -0.5f, 0.0f),
        glm::identity<glm::quat>(),
        BodyMotionType::Static
    );
    physics->addBody(floor->body, false);

    // ====== 創建斜坡 ======
    auto ramp = scene->createNode("Ramp");
    scene->addMeshToNode(ramp, MeshBuilder::buildCube(1.0f, material));
    ramp->setPosition(glm::vec3(10.0f, 1.0f, 0.0f));
    ramp->setLocalScale(glm::vec3(5.0f, 0.2f, 3.0f));
    ramp->setLocalRotation(glm::angleAxis(glm::radians(15.0f), glm::vec3(0, 0, 1)));
    ramp->body = physics->createBoxBody(
        glm::vec3(5.0f, 0.2f, 3.0f),
        ramp->getWorldPosition(),
        ramp->getWorldRotation(),
        BodyMotionType::Static
    );
    physics->addBody(ramp->body, false);

    // ====== 創建一些障礙物 ======
    for (int i = 0; i < 5; i++) {
        auto obstacle = scene->createNode("Obstacle" + std::to_string(i));
        scene->addMeshToNode(obstacle, MeshBuilder::buildCube(1.0f, material));
        obstacle->setPosition(glm::vec3(-5.0f + i * 2.5f, 0.5f, 5.0f));
        obstacle->body = physics->createBoxBody(
            glm::vec3(0.5f, 0.5f, 0.5f),
            obstacle->getWorldPosition(),
            glm::identity<glm::quat>(),
            BodyMotionType::Dynamic
        );
        physics->addBody(obstacle->body, true);
        physics->setMass(obstacle->body, 50.0f);
    }

    // ====== 創建Character Controller ======
    auto character = scene->createNode("Character");
    scene->addMeshToNode(character, MeshBuilder::buildCube(1.0f, material));
    character->setPosition(glm::vec3(0.0f, 2.0f, 0.0f));
    character->setLocalScale(glm::vec3(0.3f, 0.9f, 0.3f));  // Capsule-like visualization

    CharacterControllerSettings charSettings;
    charSettings.height = 1.8f;
    charSettings.radius = 0.3f;
    charSettings.mass = 70.0f;
    character->attachCharacterController(physics.get(), charSettings);

    // ====== 創建Vehicle ======
    auto vehicle = scene->createNode("Vehicle");
    scene->addMeshToNode(vehicle, MeshBuilder::buildCube(1.0f, material));
    vehicle->setPosition(glm::vec3(-10.0f, 3.0f, 0.0f));
    vehicle->setLocalScale(glm::vec3(0.9f, 0.7f, 2.2f));  // Sedan-like size

    VehicleSettings vehicleSettings = VehicleSettings::createSedan();
    vehicle->attachVehicleController(physics.get(), vehicleSettings);

    // ====== 創建流體區域 (水池) ======
    auto waterVolume = scene->createFluidVolume(
        physics.get(),
        FluidVolumeSettings::createWaterVolume(
            glm::vec3(0.0f, 1.0f, -10.0f),  // Position
            glm::vec3(10.0f, 2.0f, 10.0f)   // Dimensions (half-extents)
        )
    );

    // Create visual representation of water
    auto waterVisual = scene->createNode("Water");
    scene->addMeshToNode(waterVisual, MeshBuilder::buildCube(1.0f, material));
    waterVisual->setPosition(glm::vec3(0.0f, 1.0f, -10.0f));
    waterVisual->setLocalScale(glm::vec3(10.0f, 2.0f, 10.0f));

    // ====== 創建一些可漂浮的物體 ======
    for (int i = 0; i < 3; i++) {
        auto floater = scene->createNode("Floater" + std::to_string(i));
        scene->addMeshToNode(floater, MeshBuilder::buildCube(1.0f, material));
        floater->setPosition(glm::vec3(-5.0f + i * 5.0f, 5.0f, -10.0f));
        floater->body = physics->createBoxBody(
            glm::vec3(0.5f, 0.5f, 0.5f),
            floater->getWorldPosition(),
            glm::identity<glm::quat>(),
            BodyMotionType::Dynamic
        );
        physics->addBody(floater->body, true);
        physics->setMass(floater->body, 100.0f);  // Mass affects buoyancy
    }

    // ====== 創建Trigger區域示例 ======
    auto triggerZone = scene->createNode("TriggerZone");
    triggerZone->setPosition(glm::vec3(15.0f, 1.0f, 0.0f));
    triggerZone->trigger = physics->createBoxTrigger(
        glm::vec3(2.0f, 2.0f, 2.0f),
        triggerZone->getWorldPosition()
    );

    renderer->stage(scene);

    // Setup camera
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    Camera camera(
        glm::vec3(0.0f, 5.0f, 15.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::radians(60.0f),
        (float)windowWidth / (float)windowHeight,
        0.05f,
        500.0f
    );

    // Setup lights
    scene->directionalLights.push_back({
        .direction = glm::vec3(0.5, -1.0, 0.3),
        .color = glm::vec3(1.0, 1.0, 1.0),
        .intensity = 5.0,
    });

    bool quit = false;
    float time = SDL_GetTicks() / 1000.0f;
    std::unordered_map<SDL_Scancode, bool> keyboardState;
    bool controlCharacter = true;  // Toggle between character and vehicle control

    fmt::print("\n=== Jolt Physics Demo ===\n");
    fmt::print("Controls:\n");
    fmt::print("  WASD: Move character\n");
    fmt::print("  Space: Jump\n");
    fmt::print("  Arrow Keys: Control vehicle\n");
    fmt::print("  C: Switch between character/vehicle control\n");
    fmt::print("  Camera: IJKL (pan/tilt), RF (pedestal), UO (roll)\n");
    fmt::print("  ESC: Quit\n\n");

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit = true;
                }
                if (e.key.scancode == SDL_SCANCODE_C) {
                    controlCharacter = !controlCharacter;
                    fmt::print("Switched to {} control\n", controlCharacter ? "character" : "vehicle");
                }
                keyboardState[e.key.scancode] = true;
                break;
            case SDL_EVENT_KEY_UP:
                keyboardState[e.key.scancode] = false;
                break;
            }
        }

        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = currTime - time;
        time = currTime;

        // Camera controls
        if (keyboardState[SDL_SCANCODE_I]) camera.tilt(1.0f * deltaTime);
        if (keyboardState[SDL_SCANCODE_K]) camera.tilt(-1.0f * deltaTime);
        if (keyboardState[SDL_SCANCODE_L]) camera.pan(-1.0f * deltaTime);
        if (keyboardState[SDL_SCANCODE_J]) camera.pan(1.0f * deltaTime);
        if (keyboardState[SDL_SCANCODE_R]) camera.pedestal(1.0f * deltaTime);
        if (keyboardState[SDL_SCANCODE_F]) camera.pedestal(-1.0f * deltaTime);
        if (keyboardState[SDL_SCANCODE_U]) camera.roll(-1.0f * deltaTime);
        if (keyboardState[SDL_SCANCODE_O]) camera.roll(1.0f * deltaTime);

        if (controlCharacter) {
            // Character movement
            auto* charController = character->getCharacterController();
            if (charController) {
                glm::vec3 moveDir(0.0f);
                if (keyboardState[SDL_SCANCODE_W]) moveDir.z -= 1.0f;
                if (keyboardState[SDL_SCANCODE_S]) moveDir.z += 1.0f;
                if (keyboardState[SDL_SCANCODE_A]) moveDir.x -= 1.0f;
                if (keyboardState[SDL_SCANCODE_D]) moveDir.x += 1.0f;

                if (glm::length(moveDir) > 0.0f) {
                    moveDir = glm::normalize(moveDir) * 5.0f;  // Movement speed
                    charController->move(moveDir, deltaTime);
                }

                if (keyboardState[SDL_SCANCODE_SPACE]) {
                    charController->jump(5.0f);  // Jump speed
                }

                // Camera follows character
                camera.setLookAt(character->getWorldPosition());
            }
        } else {
            // Vehicle controls
            auto* vehicleController = vehicle->getVehicleController();
            if (vehicleController) {
                float throttle = 0.0f;
                float steering = 0.0f;

                if (keyboardState[SDL_SCANCODE_UP]) throttle = 1.0f;
                if (keyboardState[SDL_SCANCODE_DOWN]) throttle = -0.5f;
                if (keyboardState[SDL_SCANCODE_LEFT]) steering = 1.0f;
                if (keyboardState[SDL_SCANCODE_RIGHT]) steering = -1.0f;

                vehicleController->setThrottle(throttle);
                vehicleController->setSteering(steering);
                vehicleController->setBrake(keyboardState[SDL_SCANCODE_SPACE] ? 1.0f : 0.0f);

                // Camera follows vehicle
                camera.setLookAt(vehicle->getWorldPosition());

                // Print vehicle stats every 2 seconds
                static float statTimer = 0.0f;
                statTimer += deltaTime;
                if (statTimer >= 2.0f) {
                    fmt::print("Vehicle Speed: {:.1f} km/h, Wheels on ground: {}/{}\n",
                        vehicleController->getSpeedKmh(),
                        (int)vehicleController->isWheelInContact(0) +
                        (int)vehicleController->isWheelInContact(1) +
                        (int)vehicleController->isWheelInContact(2) +
                        (int)vehicleController->isWheelInContact(3),
                        vehicleController->getWheelCount()
                    );
                    statTimer = 0.0f;
                }
            }
        }

        // Update systems
        engineCore->update(deltaTime);
        scene->update(deltaTime);
        physics->process(scene, deltaTime);

        renderer->draw(scene, camera);
    }

    // Cleanup
    renderer->deinit();
    physics->deinit();
    engineCore->shutdown();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
