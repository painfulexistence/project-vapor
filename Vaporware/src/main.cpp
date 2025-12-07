#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <args.hxx>
#include <fmt/core.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/trigonometric.hpp>
#include <iostream>

#include "Vapor/asset_manager.hpp"
#include "Vapor/camera.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/font_manager.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/rmlui_manager.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/scene.hpp"
#include <RmlUi/Core/ElementDocument.h>
#include <entt/entt.hpp>


#include "components.hpp"
#include "systems.hpp"

entt::entity getActiveCamera(entt::registry& registry) {
    auto view = registry.view<Vapor::VirtualCameraComponent>();
    for (auto entity : view) {
        if (view.get<Vapor::VirtualCameraComponent>(entity).isActive) {
            return entity;
        }
    }
    return entt::null;
}

int main(int argc, char* args[]) {
    args::ArgumentParser parser{ "This is Project Vapor." };
    args::Group windowGroup(parser, "Window:");
    args::ValueFlag<Uint32> width(windowGroup, "number", "Window width", { 'w', "width" }, 1280);
    args::ValueFlag<Uint32> height(windowGroup, "number", "Window height", { 'h', "height" }, 720);
    args::Group graphicsGroup(parser, "Graphics:", args::Group::Validators::Xor);
    args::Flag useMetal(graphicsGroup, "Metal", "Use Metal backend", { "metal" });
    args::Flag useVulkan(graphicsGroup, "Vulkan", "Use Vulkan backend", { "vulkan" });
    args::Group helpGroup(parser, "Help:");
    args::HelpFlag help(helpGroup, "help", "Display help menu", { "help" });
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

    auto window = SDL_CreateWindow(winTitle, width.Get(), height.Get(), winFlags);
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Initialization
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    Vapor::RNG rng;

    auto renderer = createRenderer(gfxBackend);
    renderer->init(window);

    // Load a font for text rendering
    FontHandle gameFont = renderer->loadFont("assets/fonts/Arial Black.ttf", 48.0f);
    if (gameFont.isValid()) {
        fmt::print("Font loaded successfully\n");
    } else {
        fmt::print("Failed to load font\n");
    }

    // Load a sprite texture for 2D/3D batch rendering demo
    auto spriteImage = AssetManager::loadImage("assets/textures/default_albedo.png");
    TextureHandle spriteTexture = renderer->createTexture(spriteImage);
    fmt::print("Sprite texture loaded\n");

    if (engineCore->initRmlUI(windowWidth, windowHeight) && renderer->initUI()) {
        fmt::print("RmlUI System Initialized\n");
    }

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler(), renderer->getDebugDraw());
    physics->setDebugEnabled(true);

    fmt::print("Engine initialized\n");

    // Resource loading
    auto& resourceManager = engineCore->getResourceManager();

    fmt::print("Loading scene asynchronously...\n");
    auto sceneResource = resourceManager.loadScene(
        std::string("assets/models/Sponza/Sponza.gltf"),
        true,// optimized
        Vapor::LoadMode::Async,
        [](std::shared_ptr<Scene> loadedScene) {
            fmt::print("Scene loaded with {} nodes\n", loadedScene->nodes.size());
        }
    );
    auto albedoResource =
        resourceManager.loadImage(std::string("assets/textures/american_walnut_albedo.png"), Vapor::LoadMode::Async);
    auto normalResource =
        resourceManager.loadImage(std::string("assets/textures/american_walnut_normal.png"), Vapor::LoadMode::Async);
    auto roughnessResource =
        resourceManager.loadImage(std::string("assets/textures/american_walnut_roughness.png"), Vapor::LoadMode::Async);

    // NOTES: optionally call resourceManager.waitForAll();

    auto scene = sceneResource->get();

    auto material = std::make_shared<Material>(Material{
        .albedoMap = albedoResource->get(),
        .normalMap = normalResource->get(),
        .roughnessMap = roughnessResource->get(),
    });

    entt::registry registry;

    auto cube1 = registry.create();
    {
        auto& transform = registry.emplace<Vapor::TransformComponent>(cube1);
        transform.position = glm::vec3(-2.0f, 0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(cube1);
        col.halfSize = glm::vec3(.5f, .5f, .5f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(cube1);
        rb.motionType = BodyMotionType::Dynamic;

        auto node = scene->createNode("Cube 1");
        scene->addMeshToNode(node, MeshBuilder::buildCube(1.0f, material));
        node->setPosition(transform.position);
        node->body = physics->createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics->addBody(node->body, true);

        auto& nodeRef = registry.emplace<SceneNodeReferenceComponent>(cube1);
        nodeRef.node = node;

        auto& rotateComp = registry.emplace<AutoRotateComponent>(cube1);
        rotateComp.axis = glm::vec3(0.0f, 1.0f, -1.0f);
        rotateComp.speed = 1.5f;
    }

    auto cube2 = registry.create();
    {
        auto& transform = registry.emplace<Vapor::TransformComponent>(cube2);
        transform.position = glm::vec3(2.0f, 0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(cube2);
        col.halfSize = glm::vec3(.5f, .5f, .5f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(cube2);
        rb.motionType = BodyMotionType::Dynamic;

        auto node = scene->createNode("Cube 2");
        scene->addMeshToNode(node, MeshBuilder::buildCube(1.0f, material));
        node->setPosition(transform.position);
        node->body = physics->createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics->addBody(node->body, true);

        auto& nodeRef = registry.emplace<SceneNodeReferenceComponent>(cube2);
        nodeRef.node = node;
    }

    auto floor = registry.create();
    {
        auto& transform = registry.emplace<Vapor::TransformComponent>(floor);
        transform.position = glm::vec3(0.0f, -0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(floor);
        col.halfSize = glm::vec3(50.0f, .5f, 50.0f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(floor);
        rb.motionType = BodyMotionType::Static;

        auto node = scene->createNode("Floor");
        node->setPosition(transform.position);
        node->body = physics->createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics->addBody(node->body, false);

        auto& nodeRef = registry.emplace<SceneNodeReferenceComponent>(floor);
        nodeRef.node = node;
    }

    scene->directionalLights.push_back({
        .direction = glm::vec3(0.5, -1.0, 0.0),
        .color = glm::vec3(1.0, 1.0, 1.0),
        .intensity = 10.0,
    });
    auto sunLight = registry.create();
    {
        auto& ref = registry.emplace<SceneDirectionalLightReferenceComponent>(sunLight);
        ref.lightIndex = 0;

        auto& logic = registry.emplace<DirectionalLightLogicComponent>(sunLight);
        logic.baseDirection = glm::vec3(0.5, -1.0, 0.0);
        logic.speed = 0.5f;
        logic.magnitude = 0.05f;
    }

    for (int i = 0; i < 8; i++) {
        scene->pointLights.push_back({ .position = glm::vec3(
                                           rng.RandomFloatInRange(-5.0f, 5.0f),
                                           rng.RandomFloatInRange(0.0f, 5.0f),
                                           rng.RandomFloatInRange(-5.0f, 5.0f)
                                       ),
                                       .color = glm::vec3(rng.RandomFloat(), rng.RandomFloat(), rng.RandomFloat()),
                                       .intensity = 5.0f * rng.RandomFloat(),
                                       .radius = 0.5f });
    }
    for (int i = 0; i < scene->pointLights.size(); ++i) {
        auto e = registry.create();

        auto& ref = registry.emplace<ScenePointLightReferenceComponent>(e);
        ref.lightIndex = i;

        auto& logic = registry.emplace<LightMovementLogicComponent>(e);
        logic.speed = 0.5f;
        logic.timer = i * 0.1f;// Offset start time

        switch (i % 4) {
        case 0:
            logic.pattern = MovementPattern::Circle;
            logic.radius = 3.0f;
            logic.height = 1.5f;
            break;
        case 1:
            logic.pattern = MovementPattern::Figure8;
            logic.radius = 3.0f;// Base radius, logic adds 1.0
            break;
        case 2:
            logic.pattern = MovementPattern::Linear;
            logic.radius = 3.0f;// Base radius
            break;
        case 3:
            logic.pattern = MovementPattern::Spiral;
            break;
        }
    }

    auto flyCam = registry.create();
    {
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(flyCam);
        cam.isActive = true;// Start active
        cam.fov = glm::radians(60.0f);
        cam.aspect = (float)windowWidth / (float)windowHeight;
        cam.position = glm::vec3(0.0f, 0.0f, 3.0f);// Set initial position directly

        auto& fly = registry.emplace<FlyCameraComponent>(flyCam);
        fly.moveSpeed = 5.0f;

        registry.emplace<CharacterIntent>(flyCam);
    }

    auto followCam = registry.create();
    {
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(followCam);
        cam.isActive = false;
        cam.aspect = (float)windowWidth / (float)windowHeight;
        cam.position = glm::vec3(0.0f, 2.0f, 5.0f);// Initial pos

        auto& follow = registry.emplace<FollowCameraComponent>(followCam);
        follow.target = cube1;
        follow.offset = glm::vec3(0.0f, 2.0f, 5.0f);
    }

    auto hud = registry.create();
    {
        auto& hudState = registry.emplace<HUDComponent>(hud);
        hudState.documentPath = "assets/ui/hud.rml";
        hudState.isVisible = false;
    }

    auto global = registry.create();

    scene->update(0.0f);
    // TODO: migrate to body create system (remember to use body destroy system, too)
    // BodyCreateSystem::update(registry, physics.get());
    renderer->stage(scene);

    Uint32 frameCount = 0;
    float time = SDL_GetTicks() / 1000.0f;
    bool quit = false;

    auto& inputManager = engineCore->getInputManager();

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
            engineCore->processRmlUIEvent(e);
            inputManager.processEvent(e);

            switch (e.type) {
            case SDL_EVENT_QUIT: {
                quit = true;
                break;
            }
            case SDL_EVENT_KEY_DOWN: {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit = true;
                }
                if (e.key.scancode == SDL_SCANCODE_H) {
                    auto view = registry.view<HUDComponent>();
                    for (auto entity : view) {
                        auto& hud = view.get<HUDComponent>(entity);
                        hud.isVisible = !hud.isVisible;
                        fmt::print("HUD Visibility toggled: {}\n", hud.isVisible);
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F3) {
                    physics->setDebugEnabled(!physics->isDebugEnabled());
                    fmt::print("Physics Debug Renderer: {}\n", physics->isDebugEnabled() ? "Enabled" : "Disabled");
                }
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED: {
                windowWidth = e.window.data1;
                windowHeight = e.window.data2;
                // renderer->resize(windowWidth, windowHeight);
                // engineCore->onWindowResize(windowWidth, windowHeight);

                // Update Camera Aspect Ratio
                auto view = registry.view<Vapor::VirtualCameraComponent>();
                view.each([&](auto& cam) { cam.aspect = (float)windowWidth / (float)windowHeight; });
                break;
            }
            default:
                break;
            }
        }

        // Input
        const auto& inputState = inputManager.getInputState();
        if (inputState.isPressed(Vapor::InputAction::Hotkey1)) {
            auto& request = registry.emplace_or_replace<CameraSwitchRequest>(global);
            request.mode = CameraSwitchRequest::Mode::Free;
        }
        if (inputState.isPressed(Vapor::InputAction::Hotkey2)) {
            auto& request = registry.emplace_or_replace<CameraSwitchRequest>(global);
            request.mode = CameraSwitchRequest::Mode::Follow;
        }
        registry.view<CharacterIntent>().each([&](auto& intent) {
            intent.lookVector = inputState.getVector(
                Vapor::InputAction::LookLeft,
                Vapor::InputAction::LookRight,
                Vapor::InputAction::LookDown,
                Vapor::InputAction::LookUp
            );
            intent.moveVector = inputState.getVector(
                Vapor::InputAction::StrafeLeft,
                Vapor::InputAction::StrafeRight,
                Vapor::InputAction::MoveBackward,
                Vapor::InputAction::MoveForward
            );
            intent.moveVerticalAxis = inputState.getAxis(Vapor::InputAction::MoveDown, Vapor::InputAction::MoveUp);
            intent.jump = inputState.isPressed(Vapor::InputAction::Jump);
            intent.sprint = inputState.isPressed(Vapor::InputAction::Sprint);
        });

        // Gameplay updates
        CameraSwitchSystem::update(registry, global);
        updateCameraSystem(registry, deltaTime);
        updateAutoRotateSystem(registry, deltaTime);
        updateLightMovementSystem(registry, scene.get(), deltaTime);
        updateHUDSystem(registry, engineCore->getRmlUiManager(), deltaTime);

        // Engine updates
        engineCore->update(deltaTime);

        scene->update(deltaTime);
        physics->process(scene, deltaTime);
        scene->update(deltaTime);

        // Rendering
        entt::entity activeCamEntity = getActiveCamera(registry);
        if (activeCamEntity != entt::null) {
            auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCamEntity);
            Camera tempCamera;// Create a temporary Camera object
            tempCamera.setEye(cam.position);// Set position for lighting/etc
            tempCamera.setViewMatrix(cam.viewMatrix);
            tempCamera.setProjectionMatrix(cam.projectionMatrix);

            // ===== 2D Canvas Demo (Screen Space) =====
            // Note: When camera is perspective (default), CanvasPass uses screen space (pixel coords)
            // When camera is orthographic, CanvasPass uses world space ortho
            float quadSize = 20.0f;
            float spacing = 25.0f;
            int cols = 10;
            int rows = 5;
            for (int y = 0; y < rows; y++) {
                for (int x = 0; x < cols; x++) {
                    float px = 50.0f + x * spacing;
                    float py = 50.0f + y * spacing;
                    // Rainbow colors based on position
                    float hue = (float)(x + y * cols) / (float)(cols * rows);
                    glm::vec4 color = glm::vec4(
                        0.5f + 0.5f * sin(hue * 6.28f),
                        0.5f + 0.5f * sin(hue * 6.28f + 2.09f),
                        0.5f + 0.5f * sin(hue * 6.28f + 4.18f),
                        0.8f
                    );
                    renderer->drawQuad2D(glm::vec2(px, py), glm::vec2(quadSize, quadSize), color);
                }
            }
            renderer->drawCircleFilled2D(glm::vec2(400.0f, 100.0f), 30.0f, glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
            renderer->drawRect2D(
                glm::vec2(450.0f, 70.0f), glm::vec2(60.0f, 60.0f), glm::vec4(0.0f, 1.0f, 0.5f, 1.0f), 2.0f
            );
            renderer->drawTriangleFilled2D(
                glm::vec2(550.0f, 130.0f),
                glm::vec2(520.0f, 70.0f),
                glm::vec2(580.0f, 70.0f),
                glm::vec4(0.5f, 0.0f, 1.0f, 1.0f)
            );
            renderer->drawRotatedQuad2D(
                glm::vec2(650.0f, 100.0f),
                glm::vec2(40.0f, 40.0f),
                time * 2.0f,// rotation in radians
                spriteTexture,
                glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)
            );

            // ===== Text Rendering Demo (Screen Space) =====
            if (gameFont.isValid()) {
                // Draw text at screen positions (pixel coordinates)
                renderer->drawText2D(
                    gameFont, "Project Vapor", glm::vec2(50.0f, 200.0f), 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)
                );

                renderer->drawText2D(
                    gameFont, "Press H to toggle HUD", glm::vec2(50.0f, 250.0f), 0.5f, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f)
                );

                // Show FPS
                renderer->drawText2D(
                    gameFont,
                    fmt::format("FPS: {:.1f}", 1.0f / deltaTime),
                    glm::vec2(50.0f, 300.0f),
                    0.5f,
                    glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)
                );
            }

            // ===== 3D Batch Demo =====
            renderer->drawQuad3D(
                glm::vec3(0.0f, 2.0f, 0.0f), glm::vec2(1.0f, 1.0f), spriteTexture, glm::vec4(1.0f, 0.5f, 0.5f, 1.0f)
            );

            renderer->draw(scene, tempCamera);
        } else {
            // Fallback camera or warning
            // fmt::print(stderr, "Warning: No active camera found for rendering.\n");
        }

        frameCount++;
    }

    // Shutdown subsystems
    physics->deinit();
    engineCore->shutdown();
    renderer->deinit();

    ImGui::DestroyContext();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}