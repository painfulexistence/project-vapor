/**
 * Interactive Physics Demo - 撿東西、丟東西、重力反轉
 *
 * Controls:
 * - WASD: Move camera
 * - Mouse: Look around (右鍵拖拽)
 * - E: Pick up object (raycast)
 * - Q: Drop held object
 * - Mouse Left Click: Throw held object
 * - G: Toggle gravity direction
 * - R/F: Camera pedestal up/down
 * - ESC: Quit
 */

#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtx/string_cast.hpp>

#include "Vapor/scene.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/camera.hpp"
#include "Vapor/engine_core.hpp"

// 玩家手部抓取系統
class GrabSystem {
public:
    GrabSystem(Physics3D* physics, Camera* camera)
        : physics(physics), camera(camera) {}

    // 嘗試撿起物體 (Raycast from camera)
    bool tryPickup() {
        if (heldBody.valid()) return false;  // Already holding something

        // Raycast from camera forward
        glm::vec3 rayStart = camera->position;
        glm::vec3 rayDir = camera->getForward();
        glm::vec3 rayEnd = rayStart + rayDir * pickupRange;

        RaycastHit hit;
        if (physics->raycast(rayStart, rayEnd, hit)) {
            // Check if it's a dynamic body we can pick up
            if (hit.node && hit.node->body.valid()) {
                auto motionType = physics->getMotionType(hit.node->body);
                if (motionType == BodyMotionType::Dynamic) {
                    heldBody = hit.node->body;
                    heldNode = hit.node;

                    // Store original state
                    originalGravityFactor = physics->getGravityFactor(heldBody);

                    // Make it kinematic so we can control it
                    physics->setMotionType(heldBody, BodyMotionType::Kinematic);
                    physics->setGravityFactor(heldBody, 0.0f);  // No gravity while held

                    // Calculate hold offset from camera
                    holdOffset = glm::length(hit.point - rayStart);

                    fmt::print("✓ Picked up: {} (distance: {:.2f}m)\n",
                        hit.node->name, holdOffset);
                    return true;
                }
            }
        }
        return false;
    }

    // 放下物體
    void drop() {
        if (!heldBody.valid()) return;

        // Restore physics properties
        physics->setMotionType(heldBody, BodyMotionType::Dynamic);
        physics->setGravityFactor(heldBody, originalGravityFactor);
        physics->setLinearVelocity(heldBody, glm::vec3(0.0f));  // Stop movement
        physics->setAngularVelocity(heldBody, glm::vec3(0.0f));

        fmt::print("✓ Dropped: {}\n", heldNode->name);

        heldBody = BodyHandle{};
        heldNode = nullptr;
    }

    // 丟出物體
    void throwObject(float throwForce) {
        if (!heldBody.valid()) return;

        // Restore to dynamic
        physics->setMotionType(heldBody, BodyMotionType::Dynamic);
        physics->setGravityFactor(heldBody, originalGravityFactor);

        // Apply throw impulse
        glm::vec3 throwDirection = camera->getForward();
        glm::vec3 throwImpulse = throwDirection * throwForce;
        physics->applyCentralImpulse(heldBody, throwImpulse);

        fmt::print("✓ Threw: {} with force {:.1f}N\n", heldNode->name, throwForce);

        heldBody = BodyHandle{};
        heldNode = nullptr;
    }

    // 更新抓取位置（每幀調用）
    void update(float deltaTime) {
        if (!heldBody.valid()) return;

        // Calculate target position (in front of camera)
        glm::vec3 targetPos = camera->position + camera->getForward() * holdOffset;

        // Smoothly move held object to target
        glm::vec3 currentPos = physics->getPosition(heldBody);
        glm::vec3 velocity = (targetPos - currentPos) / deltaTime;

        // Clamp velocity to prevent too fast movement
        float maxSpeed = 20.0f;
        if (glm::length(velocity) > maxSpeed) {
            velocity = glm::normalize(velocity) * maxSpeed;
        }

        physics->setLinearVelocity(heldBody, velocity);

        // Update node position (for rendering)
        if (heldNode) {
            heldNode->setPosition(currentPos);
        }
    }

    bool isHoldingObject() const { return heldBody.valid(); }

    void adjustHoldDistance(float delta) {
        holdOffset = glm::clamp(holdOffset + delta, 1.0f, 10.0f);
    }

private:
    Physics3D* physics;
    Camera* camera;

    BodyHandle heldBody;
    Node* heldNode = nullptr;
    float holdOffset = 3.0f;  // Distance from camera
    float pickupRange = 5.0f;
    float originalGravityFactor = 1.0f;
};

int main(int argc, char* args[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print("SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    auto window = SDL_CreateWindow(
        "Interactive Physics - Pick, Throw, Gravity",
        1920, 1080,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN
    );

    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    auto renderer = createRenderer(GraphicsBackend::Vulkan);
    renderer->init(window);

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler());
    physics->setGravity(glm::vec3(0.0f, -9.81f, 0.0f));

    auto scene = std::make_shared<Scene>("Interactive Physics");
    auto material = std::make_shared<Material>();

    // ====== 創建場景 ======

    // 地面
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
    physics->addBody(floor->body);

    // 創建多個可撿拾的物體
    std::vector<std::shared_ptr<Node>> pickableObjects;

    // 箱子們
    for (int i = 0; i < 5; i++) {
        auto box = scene->createNode("Box_" + std::to_string(i));
        scene->addMeshToNode(box, MeshBuilder::buildCube(1.0f, material));

        float x = -10.0f + i * 5.0f;
        box->setPosition(glm::vec3(x, 5.0f, 0.0f));
        box->setLocalScale(glm::vec3(0.5f, 0.5f, 0.5f));

        box->body = physics->createBoxBody(
            glm::vec3(0.5f, 0.5f, 0.5f),
            box->getWorldPosition(),
            glm::identity<glm::quat>(),
            BodyMotionType::Dynamic
        );
        physics->addBody(box->body, true);
        physics->setMass(box->body, 10.0f);
        physics->setBodyUserData(box->body, reinterpret_cast<Uint64>(box.get()));

        pickableObjects.push_back(box);
    }

    // 球體們
    for (int i = 0; i < 3; i++) {
        auto sphere = scene->createNode("Sphere_" + std::to_string(i));
        scene->addMeshToNode(sphere, MeshBuilder::buildCube(1.0f, material));

        sphere->setPosition(glm::vec3(-5.0f + i * 5.0f, 10.0f, -5.0f));
        sphere->setLocalScale(glm::vec3(0.4f, 0.4f, 0.4f));

        sphere->body = physics->createSphereBody(
            0.4f,
            sphere->getWorldPosition(),
            glm::identity<glm::quat>(),
            BodyMotionType::Dynamic
        );
        physics->addBody(sphere->body, true);
        physics->setMass(sphere->body, 5.0f);
        physics->setRestitution(sphere->body, 0.8f);  // Bouncy
        physics->setBodyUserData(sphere->body, reinterpret_cast<Uint64>(sphere.get()));

        pickableObjects.push_back(sphere);
    }

    // 一些輕的物體
    for (int i = 0; i < 3; i++) {
        auto light = scene->createNode("Light_" + std::to_string(i));
        scene->addMeshToNode(light, MeshBuilder::buildCube(1.0f, material));

        light->setPosition(glm::vec3(5.0f + i * 3.0f, 8.0f, 5.0f));
        light->setLocalScale(glm::vec3(0.3f, 0.3f, 0.3f));

        light->body = physics->createBoxBody(
            glm::vec3(0.3f, 0.3f, 0.3f),
            light->getWorldPosition(),
            glm::identity<glm::quat>(),
            BodyMotionType::Dynamic
        );
        physics->addBody(light->body, true);
        physics->setMass(light->body, 1.0f);  // Very light
        physics->setBodyUserData(light->body, reinterpret_cast<Uint64>(light.get()));

        pickableObjects.push_back(light);
    }

    // 創建一個障礙牆
    auto wall = scene->createNode("Wall");
    scene->addMeshToNode(wall, MeshBuilder::buildCube(1.0f, material));
    wall->setPosition(glm::vec3(0.0f, 2.5f, -10.0f));
    wall->setLocalScale(glm::vec3(15.0f, 2.5f, 0.5f));
    wall->body = physics->createBoxBody(
        glm::vec3(15.0f, 2.5f, 0.5f),
        wall->getWorldPosition(),
        glm::identity<glm::quat>(),
        BodyMotionType::Static
    );
    physics->addBody(wall->body);

    renderer->stage(scene);

    // Setup camera
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    Camera camera(
        glm::vec3(0.0f, 5.0f, 20.0f),
        glm::vec3(0.0f, 2.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::radians(60.0f),
        (float)windowWidth / (float)windowHeight,
        0.05f,
        500.0f
    );

    // Setup grab system
    GrabSystem grabSystem(physics.get(), &camera);

    // Lights
    scene->directionalLights.push_back({
        .direction = glm::vec3(0.5, -1.0, 0.3),
        .color = glm::vec3(1.0, 1.0, 1.0),
        .intensity = 5.0,
    });

    bool quit = false;
    float time = SDL_GetTicks() / 1000.0f;
    std::unordered_map<SDL_Scancode, bool> keyState;
    bool mouseRightDown = false;
    bool gravityInverted = false;
    float mouseX = 0, mouseY = 0;

    SDL_SetWindowRelativeMouseMode(window, false);  // Start with cursor visible

    fmt::print("\n=== Interactive Physics Demo ===\n");
    fmt::print("Controls:\n");
    fmt::print("  WASD/RF: Move camera\n");
    fmt::print("  Mouse Right + Drag: Look around\n");
    fmt::print("  E: Pick up object (raycast)\n");
    fmt::print("  Q: Drop object\n");
    fmt::print("  Left Click: Throw object\n");
    fmt::print("  Mouse Wheel: Adjust hold distance\n");
    fmt::print("  G: Toggle gravity direction\n");
    fmt::print("  ESC: Quit\n\n");

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;

            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;

                // Pick up
                if (e.key.scancode == SDL_SCANCODE_E) {
                    if (!grabSystem.tryPickup()) {
                        fmt::print("✗ No object in range or already holding\n");
                    }
                }

                // Drop
                if (e.key.scancode == SDL_SCANCODE_Q) {
                    grabSystem.drop();
                }

                // Toggle gravity
                if (e.key.scancode == SDL_SCANCODE_G) {
                    gravityInverted = !gravityInverted;
                    glm::vec3 newGravity = gravityInverted ?
                        glm::vec3(0.0f, 9.81f, 0.0f) :   // Up
                        glm::vec3(0.0f, -9.81f, 0.0f);   // Down
                    physics->setGravity(newGravity);
                    fmt::print("✓ Gravity: {}\n", gravityInverted ? "INVERTED ↑" : "NORMAL ↓");
                }

                keyState[e.key.scancode] = true;
                break;

            case SDL_EVENT_KEY_UP:
                keyState[e.key.scancode] = false;
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    mouseRightDown = true;
                    SDL_SetWindowRelativeMouseMode(window, true);
                }

                // Throw object
                if (e.button.button == SDL_BUTTON_LEFT && grabSystem.isHoldingObject()) {
                    grabSystem.throwObject(500.0f);  // Throw force
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    mouseRightDown = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (mouseRightDown) {
                    mouseX = e.motion.xrel;
                    mouseY = e.motion.yrel;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                // Adjust hold distance with mouse wheel
                grabSystem.adjustHoldDistance(e.wheel.y * 0.5f);
                break;
            }
        }

        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = glm::min(currTime - time, 0.1f);  // Cap delta time
        time = currTime;

        // Camera movement
        float moveSpeed = 5.0f * deltaTime;
        if (keyState[SDL_SCANCODE_W]) camera.dolly(moveSpeed);
        if (keyState[SDL_SCANCODE_S]) camera.dolly(-moveSpeed);
        if (keyState[SDL_SCANCODE_D]) camera.truck(moveSpeed);
        if (keyState[SDL_SCANCODE_A]) camera.truck(-moveSpeed);
        if (keyState[SDL_SCANCODE_R]) camera.pedestal(moveSpeed);
        if (keyState[SDL_SCANCODE_F]) camera.pedestal(-moveSpeed);

        // Mouse look
        if (mouseRightDown) {
            float sensitivity = 0.002f;
            camera.pan(-mouseX * sensitivity);
            camera.tilt(-mouseY * sensitivity);
            mouseX = 0;
            mouseY = 0;
        }

        // Update grab system
        grabSystem.update(deltaTime);

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
