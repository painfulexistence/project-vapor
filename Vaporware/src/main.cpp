#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <args.hxx>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include "SDL3/SDL_stdinc.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include <iostream>

#include "Vapor/scene.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/asset_manager.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/camera.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/fsm.hpp"


int main(int argc, char* args[]) {
    args::ArgumentParser parser { "This is Project Vapor." };
    args::Group windowGroup(parser, "Window:");
    args::ValueFlag<Uint32> width(windowGroup, "number", "Window width", {'w', "width"}, 1280);
    args::ValueFlag<Uint32> height(windowGroup, "number", "Window height", {'h', "height"}, 720);
    args::Group graphicsGroup(parser, "Graphics:", args::Group::Validators::Xor);
    args::Flag useMetal(graphicsGroup, "Metal", "Use Metal backend", {"metal"});
    args::Flag useVulkan(graphicsGroup, "Vulkan", "Use Vulkan backend", {"vulkan"});
    args::Group helpGroup(parser, "Help:");
    args::HelpFlag help(helpGroup, "help", "Display help menu", {"help"});
    if (argc > 1) {
        try {
            parser.ParseCLI(argc, args);
        } catch (args::Help) {
            std::cout << parser;
            return 0;
        } catch (args::ParseError e) {
            std::cerr << e.what() << std::endl;
            std::cerr << parser;
            return 1;
        } catch (args::ValidationError e) {
            std::cerr << e.what() << std::endl;
            std::cerr << parser;
            return 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print("SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    const char* winTitle;
    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    GraphicsBackend gfxBackend;
#if defined(__APPLE__)
    if (useVulkan) {
        winTitle = "Project Vapor (Vulkan)";
        winFlags |= SDL_WINDOW_VULKAN;
        gfxBackend = GraphicsBackend::Vulkan;
    } else {
        winTitle = "Project Vapor (Metal)";
        winFlags |= SDL_WINDOW_METAL;
        gfxBackend = GraphicsBackend::Metal;
    }
#else
    winTitle = "Project Vapor (Vulkan)";
    winFlags |= SDL_WINDOW_VULKAN;
    gfxBackend = GraphicsBackend::Vulkan;
#endif

    auto window = SDL_CreateWindow(
        winTitle,
        width.Get(),
        height.Get(),
        winFlags
    );
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    RNG rng;

    // ========================================================================
    // FSM Example: Simple Player State Machine
    // ========================================================================

    // Simple player struct for FSM example
    struct Player {
        glm::vec3 velocity{0.0f, 0.0f, 0.0f};
        std::string currentAnimation;
        bool isGrounded = true;
    };

    Player player;

    // Create custom state actions
    class SetVelocityAction : public Vapor::StateAction {
    public:
        SetVelocityAction(float x, float y, float z) : velocity(x, y, z) {}

        void execute(Vapor::FSM* fsm, float dt = 0.0f) override {
            auto* owner = fsm->getOwner<Player>();
            if (owner) {
                owner->velocity = velocity;
            }
        }
    private:
        glm::vec3 velocity;
    };

    class SetAnimationAction : public Vapor::StateAction {
    public:
        SetAnimationAction(const std::string& anim) : animation(anim) {}

        void execute(Vapor::FSM* fsm, float dt = 0.0f) override {
            auto* owner = fsm->getOwner<Player>();
            if (owner) {
                owner->currentAnimation = animation;
            }
        }
    private:
        std::string animation;
    };

    // Create FSM
    auto playerFSM = std::make_unique<Vapor::FSM>();
    playerFSM->setOwner(&player);

    // Create states
    auto idleState = std::make_shared<Vapor::State>("Idle");
    idleState->addEnterAction(std::make_unique<SetVelocityAction>(0.0f, 0.0f, 0.0f));
    idleState->addEnterAction(std::make_unique<SetAnimationAction>("idle"));
    idleState->addEnterAction(std::make_unique<Vapor::PrintAction>("Player entered Idle state"));

    auto runningState = std::make_shared<Vapor::State>("Running");
    runningState->addEnterAction(std::make_unique<SetAnimationAction>("run"));
    runningState->addEnterAction(std::make_unique<Vapor::PrintAction>("Player entered Running state"));
    runningState->addUpdateAction(std::make_unique<Vapor::CallbackAction>(
        [](Vapor::FSM* fsm, float dt) {
            auto* owner = fsm->getOwner<Player>();
            if (owner) {
                owner->velocity.x = 5.0f * dt;  // Move forward
            }
        }
    ));

    auto jumpingState = std::make_shared<Vapor::State>("Jumping");
    jumpingState->addEnterAction(std::make_unique<SetAnimationAction>("jump"));
    jumpingState->addEnterAction(std::make_unique<Vapor::PrintAction>("Player entered Jumping state"));
    jumpingState->addEnterAction(std::make_unique<Vapor::CallbackAction>(
        [](Vapor::FSM* fsm, float dt) {
            auto* owner = fsm->getOwner<Player>();
            if (owner) {
                owner->velocity.y = 10.0f;  // Jump impulse
                owner->isGrounded = false;
            }
        }
    ));

    // Add states to FSM
    playerFSM->addState(idleState);
    playerFSM->addState(runningState);
    playerFSM->addState(jumpingState);

    // Define transitions
    playerFSM->addTransition("Idle", "StartRunning", "Running");
    playerFSM->addTransition("Idle", "Jump", "Jumping");
    playerFSM->addTransition("Running", "StopRunning", "Idle");
    playerFSM->addTransition("Running", "Jump", "Jumping");
    playerFSM->addTransition("Jumping", "Land", "Idle");

    // Set initial state
    playerFSM->setState("Idle");

    // Print state diagram
    playerFSM->printStateDiagram();

    fmt::print("\nFSM Example initialized. Use keys:\n");
    fmt::print("  SPACE: Start/Stop running\n");
    fmt::print("  X: Jump\n\n");

    // Initialize engine core with enkiTS task scheduler
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init(); // Auto-detects thread count
    fmt::print("Engine core initialized\n");

    auto renderer = createRenderer(gfxBackend);
    renderer->init(window);

    // Initialize physics (Physics3D creates its own JoltEnkiJobSystem internally)
    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler());

    // Get resource manager
    auto& resourceManager = engineCore->getResourceManager();

    // Load scene asynchronously
    fmt::print("Loading scene asynchronously...\n");
    auto sceneResource = resourceManager.loadScene(
        std::string("assets/models/Sponza/Sponza.gltf"),
        true,  // optimized
        Vapor::LoadMode::Async,
        [](std::shared_ptr<Scene> loadedScene) {
            fmt::print("Scene loaded with {} nodes\n", loadedScene->nodes.size());
        }
    );

    // Wait for scene to be ready (blocking for now, but async in background)
    auto scene = sceneResource->get();
    fmt::print("Scene ready for rendering\n");
    scene->directionalLights.push_back({
        .direction = glm::vec3(0.5, -1.0, 0.0),
        .color = glm::vec3(1.0, 1.0, 1.0),
        .intensity = 10.0,
    });
    for (int i = 0; i < 8; i++) {
        scene->pointLights.push_back({
            .position = glm::vec3(rng.RandomFloatInRange(-5.0f, 5.0f), rng.RandomFloatInRange(0.0f, 5.0f), rng.RandomFloatInRange(-5.0f, 5.0f)),
            .color = glm::vec3(rng.RandomFloat(), rng.RandomFloat(), rng.RandomFloat()),
            .intensity = 5.0f * rng.RandomFloat(),
            .radius = 0.5f
        });
    }
    // Load textures asynchronously
    fmt::print("Loading textures...\n");
    auto albedoResource = resourceManager.loadImage(
        std::string("assets/textures/american_walnut_albedo.png"),
        Vapor::LoadMode::Async
    );
    auto normalResource = resourceManager.loadImage(
        std::string("assets/textures/american_walnut_normal.png"),
        Vapor::LoadMode::Async
    );
    auto roughnessResource = resourceManager.loadImage(
        std::string("assets/textures/american_walnut_roughness.png"),
        Vapor::LoadMode::Async
    );

    // Wait for all textures to load
    resourceManager.waitForAll();

    auto material = std::make_shared<Material>(Material {
        .albedoMap = albedoResource->get(),
        .normalMap = normalResource->get(),
        .roughnessMap = roughnessResource->get(),
    });
    fmt::print("Textures loaded\n");
    auto entity1 = scene->createNode("Cube 1");
    scene->addMeshToNode(entity1, MeshBuilder::buildCube(1.0f, material));
    entity1->setPosition(glm::vec3(-2.0f, 10.5f, 0.0f));
    entity1->body = physics->createBoxBody(glm::vec3(.5f, .5f, .5f), glm::vec3(-2.0f, 0.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Dynamic);
    physics->addBody(entity1->body, true);
    auto entity2 = scene->createNode("Cube 2");
    scene->addMeshToNode(entity2, MeshBuilder::buildCube(1.0f, material));
    entity2->setPosition(glm::vec3(2.0f, 0.5f, 0.0f));
    entity2->body = physics->createBoxBody(glm::vec3(.5f, .5f, .5f), glm::vec3(2.0f, 0.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Dynamic);
    physics->addBody(entity2->body, true);
    auto entity3 = scene->createNode("Floor");
    entity3->setPosition(glm::vec3(0.0f, -0.5f, 0.0f));
    entity3->body = physics->createBoxBody(glm::vec3(50.0f, .5f, 50.0f), glm::vec3(0.0f, -.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Static);
    physics->addBody(entity3->body, false);
    // auto entity4 = scene->createNode("Obj Model");
    // scene->addMeshToNode(entity4, AssetManager::loadOBJ(std::string("assets/models/Sibenik/sibenik.obj"), std::string("assets/models/Sibenik/")));

    renderer->stage(scene);

    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    Camera camera = Camera(
        glm::vec3(0.0f, 0.0f, 3.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::radians(60.0f),
        (float)windowWidth / (float)windowHeight,
        0.05f,
        500.0f
    );

    Uint32 frameCount = 0;
    float time = SDL_GetTicks() / 1000.0f;
    bool quit = false;
    std::unordered_map<SDL_Scancode, bool> keyboardState;
    std::unordered_map<SDL_Scancode, bool> prevKeyboardState;
    while (!quit) {
        prevKeyboardState = keyboardState;
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            switch (e.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN: {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit = true;
                }
                keyboardState[e.key.scancode] = true;

                // FSM Example: Handle key presses
                if (e.key.scancode == SDL_SCANCODE_SPACE && !prevKeyboardState[SDL_SCANCODE_SPACE]) {
                    // Toggle between Idle and Running
                    if (playerFSM->getCurrentStateName() == "Idle") {
                        playerFSM->handleEvent(Vapor::FSMEvent("StartRunning"));
                    } else if (playerFSM->getCurrentStateName() == "Running") {
                        playerFSM->handleEvent(Vapor::FSMEvent("StopRunning"));
                    }
                }

                if (e.key.scancode == SDL_SCANCODE_X && !prevKeyboardState[SDL_SCANCODE_X]) {
                    // Jump (from Idle or Running)
                    playerFSM->handleEvent(Vapor::FSMEvent("Jump"));
                }

                break;
            }
            case SDL_EVENT_KEY_UP: {
                keyboardState[e.key.scancode] = false;
                break;
            }
            case SDL_EVENT_MOUSE_MOTION: {
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED: {
                // renderer->resize(e.window.data1, e.window.data2);
                break;
            }
            default:
                break;
            }
        }

        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = currTime - time;
        time = currTime;

        if (keyboardState[SDL_SCANCODE_W]) {
            camera.dolly(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_S]) {
            camera.dolly(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_D]) {
            camera.truck(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_A]) {
            camera.truck(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_R]) {
            camera.pedestal(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_F]) {
            camera.pedestal(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_I]) {
            camera.tilt(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_K]) {
            camera.tilt(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_L]) {
            camera.pan(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_J]) {
            camera.pan(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_U]) {
            camera.roll(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_O]) {
            camera.roll(1.0f * deltaTime);
        }

        entity1->rotate(glm::vec3(0.0f, 1.0f, -1.0f), 1.5f * deltaTime);
        float speed = 0.5f;
        scene->directionalLights[0].direction = glm::vec3(0.5, -1.0, 0.05 * sin(time * speed));
        for (int i = 0; i < scene->pointLights.size(); i++) {
            auto& l = scene->pointLights[i];
            switch (i % 4) {
            case 0:  // circular motion
                l.position.x = 3.0f * cos(time * speed + i * 0.1f);
                l.position.z = 3.0f * sin(time * speed + i * 0.1f);
                l.position.y = 1.5f + 0.5f * sin(time * speed * 0.5f + i * 0.2f);
                break;

            case 1:  // figure-8 motion
                l.position.x = 4.0f * sin(time * speed * 0.7f + i * 0.15f);
                l.position.z = 4.0f * sin(time * speed * 0.7f + i * 0.15f) * cos(time * speed * 0.7f + i * 0.15f);
                l.position.y = 1.0f + 1.0f * cos(time * speed * 0.3f + i * 0.1f);
                break;

            case 2:  // linear motion
                l.position.x = 4.0f * sin(time * speed * 0.6f + i * 0.12f);
                l.position.z = 2.0f * cos(time * speed * 0.8f + i * 0.18f);
                l.position.y = 0.5f + 2.0f * abs(sin(time * speed * 0.4f + i * 0.14f));
                break;

            case 3:  // spiral motion
                float spiralRadius = 2.0f + 1.0f * sin(time * speed * 0.2f + i * 0.05f);
                l.position.x = spiralRadius * cos(time * speed * 0.5f + i * 0.08f);
                l.position.z = spiralRadius * sin(time * speed * 0.5f + i * 0.08f);
                l.position.y = 0.5f + 2.5f * (1.0f - cos(time * speed * 0.3f + i * 0.06f));
                break;
            }
            l.intensity = 3.0f + 2.0f * (0.5f + 0.5f * sin(time * 0.3f + i * 0.1f));
        }

        // Update engine core (handles async task completion)
        engineCore->update(deltaTime);

        // Update FSM
        playerFSM->update(deltaTime);

        // FSM Example: Simple jump landing logic
        if (playerFSM->getCurrentStateName() == "Jumping") {
            player.velocity.y -= 9.8f * deltaTime;  // Apply gravity
            if (player.velocity.y <= 0.0f && !player.isGrounded) {
                player.isGrounded = true;
                playerFSM->handleEvent(Vapor::FSMEvent("Land"));
            }
        }

        // Debug: Print current state and velocity occasionally
        static float debugTimer = 0.0f;
        debugTimer += deltaTime;
        if (debugTimer > 2.0f) {
            debugTimer = 0.0f;
            fmt::print("FSM State: {}, Velocity: ({:.2f}, {:.2f}, {:.2f}), Animation: {}\n",
                      playerFSM->getCurrentStateName(),
                      player.velocity.x, player.velocity.y, player.velocity.z,
                      player.currentAnimation);
        }

        scene->update(deltaTime);
        physics->process(scene, deltaTime);
        // scene->update(deltaTime);

        renderer->draw(scene, camera);

        frameCount++;
    }

    // Shutdown subsystems
    renderer->deinit();
    physics->deinit();
    engineCore->shutdown();

    ImGui::DestroyContext();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}