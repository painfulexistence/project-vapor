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

#include "camera_manager.hpp"
#include "hot_reload/game_memory.hpp"
#include "hot_reload/module_loader.hpp"

// Get the gameplay module path based on platform and build config
std::string getGameplayModulePath() {
#ifdef _WIN32
    return "./gameplay/Gameplay";
#else
    return "./gameplay/libGameplay";
#endif
}

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

    // Create camera manager with FlyCam and FollowCam
    Vapor::CameraManager cameraManager;

    // Add FlyCam (free-flying camera)
    auto flyCam = std::make_unique<Vapor::FlyCam>(
        glm::vec3(0.0f, 2.0f, 8.0f),     // Eye position
        glm::vec3(0.0f, 0.0f, 0.0f),     // Look at center
        glm::vec3(0.0f, 1.0f, 0.0f),     // Up vector
        glm::radians(60.0f),              // FOV
        (float)windowWidth / (float)windowHeight,  // Aspect ratio
        0.05f,                            // Near plane
        500.0f,                           // Far plane
        5.0f,                             // Move speed
        1.5f                              // Rotate speed
    );
    cameraManager.addCamera("fly", std::move(flyCam));

    // Add FollowCam (follows entity1)
    auto followCam = std::make_unique<Vapor::FollowCam>(
        entity1,                          // Target to follow
        glm::vec3(0.0f, 1.0f, 2.0f),     // Offset (behind and above)
        glm::radians(60.0f),              // FOV
        (float)windowWidth / (float)windowHeight,  // Aspect ratio
        0.05f,                            // Near plane
        500.0f,                           // Far plane
        0.1f,                             // Smooth factor (lower = smoother)
        0.1f                              // Deadzone
    );
    cameraManager.addCamera("follow", std::move(followCam));

    // Start with fly camera
    cameraManager.switchCamera("fly");
    fmt::print("Camera controls:\n");
    fmt::print("  Press '1' - Switch to Fly Camera (free movement with WASDRF + IJKL)\n");
    fmt::print("  Press '2' - Switch to Follow Camera (follows Cube 1)\n");

    Uint32 frameCount = 0;
    float time = SDL_GetTicks() / 1000.0f;
    bool quit = false;

    auto& inputManager = engineCore->getInputManager();

    // Initialize hot reload system
    Game::GameMemory gameMemory;
    gameMemory.scene = scene.get();
    gameMemory.physics = physics.get();
    gameMemory.renderer = renderer.get();
    gameMemory.input = &inputManager;
    gameMemory.engine = engineCore.get();

    Game::ModuleLoader moduleLoader;
    bool gameplayLoaded = moduleLoader.load(getGameplayModulePath());
    if (gameplayLoaded) {
        auto initFunc = moduleLoader.getInitFunc();
        if (initFunc) {
            initFunc(&gameMemory);
        }
    } else {
        fmt::print("[Main] Failed to load gameplay module: {}\n", moduleLoader.getLastError());
        fmt::print("[Main] Running without gameplay DLL\n");
    }

    while (!quit) {
        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = currTime - time;
        time = currTime;

        // IMPORTANT: Update input manager FIRST to clear previous frame's pressed/released actions
        // This must happen BEFORE processing new events
        inputManager.update(deltaTime);

        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            ImGui_ImplSDL3_ProcessEvent(&e);

            inputManager.processEvent(e);

            // Handle special events
            switch (e.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN: {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit = true;
                }
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

        const auto& inputState = inputManager.getInputState();

        if (inputState.isPressed(Vapor::InputAction::Hotkey1)) {
            cameraManager.switchCamera("fly");
            fmt::print("[Main] Switched to Fly Camera\n");
        }
        if (inputState.isPressed(Vapor::InputAction::Hotkey2)) {
            cameraManager.switchCamera("follow");
            fmt::print("[Main] Switched to Follow Camera\n");
        }

        cameraManager.update(deltaTime, inputState);

        engineCore->update(deltaTime);

        // Call gameplay module update
        if (moduleLoader.isLoaded()) {
            Game::FrameInput frameInput;
            frameInput.deltaTime = deltaTime;
            frameInput.inputState = &inputState;
            auto updateFunc = moduleLoader.getUpdateFunc();
            if (updateFunc) {
                updateFunc(&gameMemory, &frameInput);
            }
        } else {
            // Fallback: run built-in gameplay logic if module not loaded
            entity1->rotate(glm::vec3(0.0f, 1.0f, -1.0f), 1.5f * deltaTime);
        }

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

        scene->update(deltaTime);
        physics->process(scene, deltaTime);
        // scene->update(deltaTime);

        // Debug panel
        if (ImGui::Begin("Debug")) {
            if (ImGui::CollapsingHeader("Hot Reload", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Module: %s", moduleLoader.isLoaded() ? "Loaded" : "Not loaded");
                if (moduleLoader.isLoaded()) {
                    ImGui::Text("Path: %s", moduleLoader.getModulePath().c_str());
                    ImGui::Text("Game Time: %.2f", gameMemory.state.gameTime);
                    ImGui::Text("Score: %d", gameMemory.state.score);
                    ImGui::Text("Paused: %s", gameMemory.state.isPaused ? "Yes" : "No");
                    if (ImGui::Button("Reload Gameplay")) {
                        if (moduleLoader.reload(&gameMemory)) {
                            fmt::print("[Main] Gameplay module reloaded successfully\n");
                        } else {
                            fmt::print("[Main] Reload failed: {}\n", moduleLoader.getLastError());
                        }
                    }
                } else {
                    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Error: %s", moduleLoader.getLastError().c_str());
                    if (ImGui::Button("Retry Load")) {
                        gameplayLoaded = moduleLoader.load(getGameplayModulePath());
                        if (gameplayLoaded) {
                            auto initFunc = moduleLoader.getInitFunc();
                            if (initFunc) {
                                initFunc(&gameMemory);
                            }
                        }
                    }
                }
            }
            if (ImGui::CollapsingHeader("Resources")) {
                ImGui::Text("Images: %zu", resourceManager.getImageCacheSize());
                ImGui::Text("Scenes: %zu", resourceManager.getSceneCacheSize());
                ImGui::Text("Meshes: %zu", resourceManager.getMeshCacheSize());
                if (ImGui::Button("Clear All Caches")) {
                    resourceManager.clearAllCaches();
                }
            }
            if (ImGui::CollapsingHeader("Stats")) {
                ImGui::Text("Frame: %u", frameCount);
                ImGui::Text("Delta: %.3f ms", deltaTime * 1000.0f);
                ImGui::Text("FPS: %.1f", 1.0f / deltaTime);
            }
        }
        ImGui::End();

        // Get current camera from camera manager
        auto* currentCam = cameraManager.getCurrentCamera();
        if (currentCam) {
            renderer->draw(scene, currentCam->getCamera());
        }

        frameCount++;
    }

    // Shutdown gameplay module
    if (moduleLoader.isLoaded()) {
        auto shutdownFunc = moduleLoader.getShutdownFunc();
        if (shutdownFunc) {
            shutdownFunc(&gameMemory);
        }
        moduleLoader.unload();
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