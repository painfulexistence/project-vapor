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
#include "scene_builder.hpp"
#include "systems.hpp"
#include "pages/page_system.hpp"
#include "pages/hud_page.hpp"
#include "pages/letterbox_page.hpp"

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

    auto [sceneBuilt, materialBuilt, cube1, global] =
        buildScene(registry, *physics, scene, material, windowWidth, windowHeight, rng);

    scene->update(0.0f);
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
                    auto& ui = registry.view<UIStateComponent>().get<UIStateComponent>(
                        registry.view<UIStateComponent>().front());
                    if (!ui.menuStack.empty() && ui.menuStack.back() == PageID::PauseMenu) {
                        PageSystem::pop(registry);
                    } else if (ui.menuStack.empty()) {
                        PageSystem::push(registry, PageID::PauseMenu);
                    } else {
                        quit = true;
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F5) {
                    auto* hudPage = PageSystem::getPage<HUDPage>(registry, PageID::HUD);
                    if (hudPage) {
                        bool nowVisible = !hudPage->isFullyVisible();
                        if (nowVisible) PageSystem::show(registry, PageID::HUD);
                        else            PageSystem::hide(registry, PageID::HUD);
                        fmt::print("HUD toggled: {}\n", nowVisible ? "on" : "off");
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F3) {
                    physics->setDebugEnabled(!physics->isDebugEnabled());
                    fmt::print("Physics Debug Renderer: {}\n", physics->isDebugEnabled() ? "Enabled" : "Disabled");
                }
                if (e.key.scancode == SDL_SCANCODE_RETURN) {
                    ScrollTextQueueSystem::advance(registry);
                }
                if (e.key.scancode == SDL_SCANCODE_F6) {
                    auto* lb = PageSystem::getPage<LetterboxPage>(registry, PageID::Letterbox);
                    if (lb) {
                        bool open = !lb->isOpen();
                        if (open) PageSystem::show(registry, PageID::Letterbox);
                        else      PageSystem::hide(registry, PageID::Letterbox);
                        fmt::print("Letterbox toggled: {}\n", open ? "opening" : "closing");
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F7) {
                    auto view = registry.view<SubtitleQueueComponent>();
                    for (auto entity : view) {
                        auto& q = view.get<SubtitleQueueComponent>(entity);
                        if (q.currentIndex >= (int)q.queue.size() - 1
                            && q.state == SubtitleQueueState::Idle) {
                            SubtitleQueueSystem::restart(registry);
                            fmt::print("Subtitles restarted\n");
                        } else {
                            SubtitleQueueSystem::advance(registry);
                        }
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F8) {
                    ChapterTitleTriggerSystem::request(registry, "Chapter I", "The Beginning");
                    fmt::print("Chapter title requested\n");
                }
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED: {
                windowWidth = e.window.data1;
                windowHeight = e.window.data2;
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
        CameraSystem::update(registry, deltaTime);
        AutoRotateSystem::update(registry, deltaTime);
        LightMovementSystem::update(registry, scene.get(), deltaTime);
        SubtitleQueueSystem::update(registry, deltaTime);
        ScrollTextQueueSystem::update(registry);
        ChapterTitleTriggerSystem::update(registry);
        PageSystem::update(registry, engineCore->getRmlUiManager(), deltaTime);

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
                    gameFont,
                    "F5: HUD | F6: Letterbox | F7: Subtitles",
                    glm::vec2(50.0f, 250.0f),
                    0.5f,
                    glm::vec4(0.8f, 0.8f, 0.8f, 1.0f)
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