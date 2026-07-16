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
#include "Vapor/stats_log.hpp"
#include "Vapor/systems.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/scene.hpp"
#include <RmlUi/Core/ElementDocument.h>
#include <entt/entt.hpp>


#include "Vapor/scene_inspector.hpp"
#include "Vapor/scene_serializer.hpp"
#include "components.hpp"
#include "pages/hud_page.hpp"
#include "pages/letterbox_page.hpp"
#include "pages/page_system.hpp"
#include "scene_builder.hpp"
#include "systems.hpp"

static void setupCustomDrawers(Vapor::SceneInspector& inspector) {
    // Register app-specific components for auto field-by-field drawing.
    inspector.registerComponent<PointLightComponent>("Point Light");
    inspector.registerComponent<SpotLightComponent>("Spot Light");
    inspector.registerComponent<Vapor::RectLightComponent>("Rect Light");
    inspector.registerComponent<DirectionalLightComponent>("Directional Light");
    inspector.registerComponent<CharacterIntent>("Character Intent");
    inspector.registerComponent<CharacterControllerComponent>("Character Controller");
    inspector.registerComponent<GrabbableComponent>("Grabbable");
    inspector.registerComponent<FirstPersonCameraComponent>("First Person Camera");
    inspector.registerComponent<AutoRotateComponent>("Auto Rotate");
    inspector.registerComponent<DirectionalLightLogicComponent>("Directional Light Logic");
    inspector.registerComponent<ChapterTitleComponent>("Chapter Title");
    inspector.registerComponent<ChapterTitleTriggerComponent>("Chapter Title Trigger");
    inspector.registerComponent<SceneTransitionComponent>("Scene Transition");
    inspector.registerComponent<ScrollTextQueueComponent>("Scroll Text Queue");
    inspector.registerComponent<Vapor::ParticleEmitterComponent>("Particle Emitter");
    inspector.registerComponent<Vapor::ParticleAttractorComponent>("Particle Attractor");
    inspector.registerComponent<Vapor::WindFieldComponent>("Wind Field");
    inspector.registerComponent<Vapor::ParticleBurstRequest>("Particle Burst");
    inspector.registerComponent<Vapor::SpellBoltComponent>("Spell Bolt");

    // LightMovementLogicComponent — keep custom drawer for the named Pattern combo.
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<LightMovementLogicComponent>(e)) {
            if (ImGui::CollapsingHeader("Light Movement Logic", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* patterns[] = { "Circle", "Figure8", "Linear", "Spiral" };
                int p = static_cast<int>(c->pattern);
                if (ImGui::Combo("pattern", &p, patterns, 4))
                    c->pattern = static_cast<MovementPattern>(p);
                ImGui::DragFloat("speed",      &c->speed,  0.01f);
                ImGui::DragFloat("radius",     &c->radius, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("height",     &c->height, 0.1f, -50.0f, 50.0f);
                ImGui::LabelText("timer", "%.2f", c->timer);
            }
        }
    });

    // CameraSwitchRequest — named combo for mode enum.
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<CameraSwitchRequest>(e)) {
            if (ImGui::CollapsingHeader("Camera Switch Request", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* modes[] = { "Free", "Follow", "FirstPerson" };
                int m = static_cast<int>(c->mode);
                if (ImGui::Combo("mode", &m, modes, 3))
                    c->mode = static_cast<CameraSwitchRequest::Mode>(m);
            }
        }
    });

    // SubtitleQueueComponent — cross-component FSM state lookup.
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<SubtitleQueueComponent>(e)) {
            if (ImGui::CollapsingHeader("Subtitle Queue", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::LabelText("queue size",       "%zu", c->queue.size());
                ImGui::LabelText("currentIndex",     "%d",  c->currentIndex);
                ImGui::Checkbox("advanceRequested",  &c->advanceRequested);
                ImGui::Checkbox("autoAdvance",       &c->autoAdvance);
                ImGui::LabelText("displayTimer",     "%.2f", c->displayTimer);
                if (auto* fsm = reg.try_get<Vapor::FSMStateComponent>(e)) {
                    const char* states[] = {
                        "Idle", "WaitingForVisible", "Displaying", "WaitingForHidden"
                    };
                    ImGui::LabelText("FSM state", "%s", states[fsm->currentState]);
                    ImGui::LabelText("FSM time",  "%.2f", fsm->stateTime);
                }
            }
        }
    });

    // SceneTransitionComponent — FSM state + progress bar.
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<SceneTransitionComponent>(e)) {
            if (ImGui::CollapsingHeader("Scene Transition (FSM)", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (auto* fsm = reg.try_get<Vapor::FSMStateComponent>(e)) {
                    const char* states[] = {
                        "Idle", "FadingInLoadingScreen", "UnloadingScene",
                        "LoadingAssets", "BuildingScene", "FadingOutLoadingScreen"
                    };
                    ImGui::LabelText("state", "%s", states[fsm->currentState]);
                }
                ImGui::ProgressBar(c->progress);
            }
        }
    });

    // Tag components — colored bullet headers (no fields).
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (reg.all_of<PersistentTag>(e)) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.4f, 0.2f, 1.0f));
            ImGui::CollapsingHeader("PersistentTag", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet);
            ImGui::PopStyleColor();
        }
        if (reg.all_of<DeadTag>(e)) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            ImGui::CollapsingHeader("DeadTag", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet);
            ImGui::PopStyleColor();
        }
    });
}

auto getActiveCamera(entt::registry& registry) -> entt::entity {
    auto view = registry.view<Vapor::VirtualCameraComponent>();
    for (auto entity : view) {
        if (view.get<Vapor::VirtualCameraComponent>(entity).isActive) {
            return entity;
        }
    }
    return entt::null;
}

auto main(int argc, char* args[]) -> int {
    args::ArgumentParser parser{ "This is Project Vapor." };
    args::Group windowGroup(parser, "Window:");
    args::ValueFlag<Uint32> width(windowGroup, "number", "Window width", { 'w', "width" }, 1280);
    args::ValueFlag<Uint32> height(windowGroup, "number", "Window height", { 'h', "height" }, 720);
    // AtMostOne (not Xor): the backend flags stay mutually exclusive, but
    // omitting both is allowed and falls through to the per-platform default
    // (Metal on Apple, Vulkan elsewhere). Xor here forced every invocation that
    // passed ANY flag (e.g. --stats) to also name a backend.
    args::Group graphicsGroup(parser, "Graphics:", args::Group::Validators::AtMostOne);
    args::Flag useMetal(graphicsGroup, "Metal", "Use Metal backend", { "metal" });
    args::Flag useVulkan(graphicsGroup, "Vulkan", "Use Vulkan backend", { "vulkan" });
    args::Group debugGroup(parser, "Debug:");
    args::Flag statsFlag(debugGroup, "stats", "Enable per-frame telemetry log (stderr + vapor_stats.log)", { "stats" });
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

    Vapor::StatsLog::get().setEnabled(static_cast<bool>(statsFlag));

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
    if (!window) {
        fmt::print(stderr, "Failed to create SDL_Window: {}\n", SDL_GetError());
        return 1;
    }
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Initialization
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    Vapor::RNG rng;

    auto renderer = createRenderer(gfxBackend, window);
    if (!renderer) {
        fmt::print(stderr, "Failed to create renderer (backend unavailable?)\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // Renderer is already initialized by createRenderer()

    // Optional: load an external HDRI for IBL instead of the procedural sky.
    // Place your .hdr file at:  <assets>/textures/env/sky.hdr
    // and uncomment the line below:
    // renderer->loadHDRI("textures/env/sky.hdr");

    // Scene serializer — engine pre-registers transform/meshRenderer;
    // game registers game-specific component writers.
    Vapor::SceneSerializer sceneSerializer;
    sceneSerializer.registerComponent("autoRotate",
        [](Vapor::json& out, entt::registry& reg, entt::entity e) {
            if (auto* c = reg.try_get<AutoRotateComponent>(e))
                out = { {"axis", Vapor::toJson(c->axis)}, {"speed", c->speed} };
        });

    Vapor::SceneInspector sceneInspector;
    sceneInspector.attachSerializer(sceneSerializer);
    sceneInspector.setGltfPath("models/Sponza/Sponza.gltf", /*optimized=*/true);
    // Exclude GLTF-spawned geometry — the inspector decides what to serialize,
    // not the serializer.
    sceneInspector.setEntityProvider([](entt::registry& reg) {
        std::vector<entt::entity> out;
        for (auto e : reg.storage<entt::entity>())
            if (!reg.all_of<SceneGeometryTag>(e)) out.push_back(e);
        return out;
    });
    setupCustomDrawers(sceneInspector);

    // Load a font for text rendering
    FontHandle gameFont = renderer->loadFont("fonts/NotoSans-SemiBold.ttf", 48.0f);
    if (gameFont.isValid()) {
        fmt::print("Font loaded successfully\n");
    } else {
        fmt::print("Failed to load font\n");
    }

    // Load a sprite texture for 2D/3D batch rendering demo
    auto spriteImage = AssetManager::loadImage("textures/default_albedo.png");
    TextureHandle spriteTexture = renderer->createTexture(spriteImage);
    fmt::print("Sprite texture loaded\n");

    // Create a render texture for render-to-texture demo
    RenderTextureDesc rtDesc;
    rtDesc.width = 512;
    rtDesc.height = 512;
    rtDesc.hasDepth = true;
    rtDesc.isHDR = true;// HDR for post-processing effects
    RenderTextureHandle renderTexture = renderer->createRenderTexture(rtDesc);
    fmt::print("Render texture created: {}x{}\n", rtDesc.width, rtDesc.height);

    // Camera for the render texture (different angle from main camera)
    Camera rtCamera(
        glm::vec3(5.0f, 3.0f, 5.0f),// Eye position
        glm::vec3(0.0f, 0.0f, 0.0f),// Look at origin
        glm::vec3(0.0f, 1.0f, 0.0f),// Up
        glm::radians(60.0f),// FOV
        1.0f,// Aspect (square)
        0.1f,// Near
        100.0f// Far
    );

    if (renderer->initUI()) {
        fmt::print("UI System Initialized\n");
    }

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler(), renderer->getDebugDraw());
    physics->setDebugEnabled(true);

    fmt::print("Engine initialized\n");

    // Resource loading
    auto& resourceManager = engineCore->getResourceManager();

    // Register single-frame atlas for the demo sprite texture
    SpriteAtlas demoAtlas;
    demoAtlas.name    = "demo_sprite";
    demoAtlas.texture = spriteTexture;
    demoAtlas.size    = glm::vec2(1.0f, 1.0f);
    demoAtlas.frames.push_back(SpriteFrame{
        "default", {0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {0.5f, 0.5f}, false
    });
    demoAtlas.nameToIndex["default"] = 0;
    AtlasHandle demoAtlasHandle = resourceManager.registerAtlas("demo_sprite", std::move(demoAtlas));

    fmt::print("Loading scene asynchronously...\n");
    auto sceneResource = resourceManager.loadScene(
        std::string("models/Sponza/Sponza.gltf"),
        true,// optimized
        Vapor::LoadMode::Async,
        [](std::shared_ptr<Scene> loadedScene) -> void {
            fmt::print("Scene loaded with {} staged meshes\n", loadedScene->stagedMeshes.size());
        }
    );
    auto albedoResource =
        resourceManager.loadImage(std::string("textures/american_walnut_albedo.png"), Vapor::LoadMode::Async);
    auto normalResource =
        resourceManager.loadImage(std::string("textures/american_walnut_normal.png"), Vapor::LoadMode::Async);
    auto roughnessResource =
        resourceManager.loadImage(std::string("textures/american_walnut_roughness.png"), Vapor::LoadMode::Async);

    // NOTES: optionally call resourceManager.waitForAll();

    auto scene = sceneResource->get();
    // Record how many meshes the GLTF scene contributes before buildScene adds more
    const size_t sponzaMeshCount = scene->stagedMeshes.size();

    auto material = std::make_shared<Vapor::Material>(Vapor::Material{
        .albedoMap = albedoResource->get(),
        .normalMap = normalResource->get(),
        .roughnessMap = roughnessResource->get(),
    });

    entt::registry registry;

    // Wire the recorder into the renderer: registers the engine ImGui window
    // (recording controls) and sets the output directory.
    {
        const char* basePath = SDL_GetBasePath();
        std::string outputDir = basePath ? std::string(basePath) + "output" : "output";
        engineCore->attachRenderer(renderer.get(), outputDir);
    }

    // Particle controls the game layer owns because they must reach both the
    // GPU sim and the CPU-side ECS timers:
    //   Pause — freezes the GPU sim AND the CPU reclaim/emission timers, so
    //           frozen particles neither age nor get their slots reclaimed.
    //   Emit  — graceful stop: existing particles live out their lifetime, no
    //           new ones are produced. (Hide is renderer-only, in Effects panel.)
    bool particlePaused = false;
    bool particleEmissionEnabled = true;

    renderer->setImGuiCallback([&]() {
        sceneInspector.draw(registry);
        if (ImGui::Begin("Particles")) {
            ImGui::Checkbox("Pause (freeze sim + reclaim)", &particlePaused);
            ImGui::Checkbox("Emit (graceful stop when off)", &particleEmissionEnabled);
        }
        ImGui::End();
    });

    auto [sceneBuilt, materialBuilt, cube1, global] =
        buildScene(registry, *physics, scene, material, windowWidth, windowHeight, rng);

    renderer->stage(scene);

    // Convert GLTF scene meshes to ECS entities so they appear in the inspector
    // and are rendered through the unified registry draw path.
    for (size_t i = 0; i < sponzaMeshCount && i < scene->stagedMeshes.size(); ++i) {
        auto& mesh = scene->stagedMeshes[i];
        const glm::mat4& worldMat =
            i < scene->stagedMeshTransforms.size() ? scene->stagedMeshTransforms[i] : glm::identity<glm::mat4>();

        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{ fmt::format("Sponza_{}", i) });
        auto& tc = registry.emplace<Vapor::TransformComponent>(e);
        // Extract scale correctly from world matrix and scale down by 0.01 for Sponza
        // The Khronos Sponza GLTF is authored in meters (~30m across) — use
        // its world transforms as-is. (An earlier 0.01 scale shrank the whole
        // scene to a 30cm dollhouse the camera flew straight past.)
        tc.scale = glm::vec3(
            glm::length(glm::vec3(worldMat[0])),
            glm::length(glm::vec3(worldMat[1])),
            glm::length(glm::vec3(worldMat[2]))
        );
        tc.position = glm::vec3(worldMat[3]);
        if (tc.scale.x > 0.0f && tc.scale.y > 0.0f && tc.scale.z > 0.0f) {
            glm::mat3 rotMat(
                glm::vec3(worldMat[0]) / tc.scale.x,
                glm::vec3(worldMat[1]) / tc.scale.y,
                glm::vec3(worldMat[2]) / tc.scale.z
            );
            tc.rotation = glm::quat_cast(rotMat);
        }
        tc.isDirty = true; // Let TransformSystem compute the scaled world matrix
        auto& mrc = registry.emplace<Vapor::MeshRendererComponent>(e);
        mrc.meshes.push_back(mesh);
        registry.emplace<SceneGeometryTag>(e);// marks GLTF-spawned geometry for serializer
    }
    // Clear stagedMeshes: GLTF meshes are now ECS entities; manually built
    // meshes (cubes, floor) are already in MeshRendererComponent and were
    // staged (materialID/instanceID set) so their mesh objects remain valid.
    scene->stagedMeshes.clear();
    scene->stagedMeshTransforms.clear();

    // Demo sprite entity — replaces the old drawRotatedQuad2D(spriteTexture) call
    {
        auto spriteEntity = registry.create();
        registry.emplace<Vapor::NameComponent>(spriteEntity, Vapor::NameComponent{"DemoSprite"});
        auto& tc = registry.emplace<Vapor::TransformComponent>(spriteEntity);
        tc.position = glm::vec3(650.0f, 100.0f, 0.0f);
        tc.isDirty  = true;
        auto& sc    = registry.emplace<Vapor::SpriteComponent>(spriteEntity);
        sc.atlas      = demoAtlasHandle;
        sc.frameIndex = 0;
        sc.size       = glm::vec2(40.0f, 40.0f);
        sc.tint       = glm::vec4(1.0f);
        registry.emplace<AutoRotateComponent>(spriteEntity, AutoRotateComponent{
            .axis  = glm::vec3(0.0f, 0.0f, 1.0f),
            .speed = 2.0f
        });
    }


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
                    if (PageSystem::isTopOfStack(registry, PageID::PauseMenu)) {
                        PageSystem::pop(registry);
                    } else if (PageSystem::isStackEmpty(registry)) {
                        PageSystem::push(registry, PageID::PauseMenu);
                    } else {
                        quit = true;
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F5) {
                    auto* hudPage = PageSystem::getPage<HUDPage>(registry, PageID::HUD);
                    if (hudPage) {
                        bool nowVisible = !hudPage->isFullyVisible();
                        if (nowVisible)
                            PageSystem::show(registry, PageID::HUD);
                        else
                            PageSystem::hide(registry, PageID::HUD);
                        fmt::print("HUD toggled: {}\n", nowVisible ? "on" : "off");
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F3) {
                    physics->setDebugEnabled(!physics->isDebugEnabled());
                    fmt::print("Physics Debug Renderer: {}\n", physics->isDebugEnabled() ? "Enabled" : "Disabled");
                }
                if (e.key.scancode == SDL_SCANCODE_RETURN) {
                    registry.view<ScrollTextQueueComponent>().each([](auto& q) { q.advanceRequested = true; });
                }
                if (e.key.scancode == SDL_SCANCODE_F6) {
                    auto* lb = PageSystem::getPage<LetterboxPage>(registry, PageID::Letterbox);
                    if (lb) {
                        bool open = !lb->isOpen();
                        if (open)
                            PageSystem::show(registry, PageID::Letterbox);
                        else
                            PageSystem::hide(registry, PageID::Letterbox);
                        fmt::print("Letterbox toggled: {}\n", open ? "opening" : "closing");
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F7) {
                    auto view = registry.view<SubtitleQueueComponent, Vapor::FSMStateComponent>();
                    for (auto entity : view) {
                        auto& q = view.get<SubtitleQueueComponent>(entity);
                        auto& fsm = view.get<Vapor::FSMStateComponent>(entity);
                        if (q.currentIndex >= (int)q.queue.size() - 1 && fsm.currentState == SubtitleStates::Idle) {
                            q.restartRequested = true;
                            fmt::print("Subtitles restarted\n");
                        } else {
                            q.advanceRequested = true;
                        }
                    }
                }
                if (e.key.scancode == SDL_SCANCODE_F8) {
                    registry.view<ChapterTitleTriggerComponent>().each([](auto& t) {
                        t.number = "Chapter I";
                        t.title = "The Beginning";
                        t.showRequested = true;
                    });
                    fmt::print("Chapter title requested\n");
                }
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED: {
                windowWidth = e.window.data1;
                windowHeight = e.window.data2;
                // Update Camera Aspect Ratio
                auto view = registry.view<Vapor::VirtualCameraComponent>();
                view.each([&](auto& cam) -> auto { cam.aspect = (float)windowWidth / (float)windowHeight; });
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
        registry.view<CharacterIntent>().each([&](auto& intent) -> auto {
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
        LightMovementSystem::update(registry, deltaTime);
        // Subtitle systems (split into single-responsibility)
        SubtitleInputSystem::update(registry);
        SubtitlePageSensorSystem::update(registry);
        SubtitleTimerSystem::update(registry, deltaTime);
        Vapor::FSMInitSystem::update(registry);
        Vapor::FSMSystem::update(registry, deltaTime);
        SubtitleActionSystem::update(registry);
        ScrollTextQueueSystem::update(registry);
        ChapterTitleTriggerSystem::update(registry);
        PageSystem::update(registry, engineCore->getRmlUiManager(), deltaTime);

        // Engine updates
        engineCore->update(deltaTime);

        physics->process(registry, deltaTime);
        Vapor::TransformSystem::update(registry);
        // Pause freezes the GPU sim (renderer) and the CPU-side emitter/reclaim
        // timers (skip the system entirely) so nothing advances while paused.
        renderer->setParticleSimPaused(particlePaused);
        Vapor::ParticleForceFieldSystem::update(registry, renderer.get());
        if (!particlePaused)
            Vapor::ParticleEmitterSystem::update(registry, renderer.get(), deltaTime, particleEmissionEnabled);
        LightGatherSystem::update(registry, scene.get());
        FlipbookSystem::update(registry, deltaTime);
        SpriteRenderSystem::update(registry, renderer.get(), &resourceManager);

        // Rendering
        entt::entity activeCamEntity = getActiveCamera(registry);
        if (activeCamEntity != entt::null) {
            auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCamEntity);
            Camera tempCamera;// Create a temporary Camera object
            tempCamera.setEye(cam.position);// Set position for lighting/etc
            tempCamera.setViewMatrix(cam.viewMatrix);
            tempCamera.setProjectionMatrix(cam.projectionMatrix);

            CameraRenderData camData;
            camData.proj = tempCamera.getProjMatrix();
            camData.view = tempCamera.getViewMatrix();
            camData.invProj = glm::inverse(camData.proj);
            camData.invView = glm::inverse(camData.view);
            camData.nearPlane = tempCamera.near();
            camData.farPlane = tempCamera.far();
            camData.position = tempCamera.getEye();

            renderer->beginFrame(camData);

            ImGui::NewFrame();
            renderer->invokeImGuiCallback();

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

            // ===== Render-to-Texture "TV" Demo =====
            // Disabled by default — it's a demo affordance, not part of the
            // scene. Flip to 1 to render the scene from an orbiting camera into
            // a texture and hang it on a world-space quad (a "TV screen").
#if 0
            {
                // Update RT camera to orbit around the scene
                float rtAngle = time * 0.5f;
                rtCamera.setEye(glm::vec3(sin(rtAngle) * 8.0f, 4.0f, cos(rtAngle) * 8.0f));
                rtCamera.setCenter(glm::vec3(0.0f, 0.0f, 0.0f));

                // Render scene to texture with different camera angle
                renderer->renderToTexture(renderTexture, scene, rtCamera, glm::vec4(0.1f, 0.1f, 0.15f, 1.0f));

                // Apply post-processing effects to the render texture
                renderer->applyBloom(renderTexture, 0.8f, 0.3f);
                renderer->applyToneMapping(renderTexture, 1.2f);

                // Get the render texture as a regular texture for drawing
                TextureHandle rtTexHandle = renderer->getRenderTextureAsTexture(renderTexture);

                // Draw the render texture on a 3D quad (like a TV screen in the world)
                if (rtTexHandle.isValid()) {
                    // Create a transform for the "TV screen"
                    glm::mat4 tvTransform = glm::mat4(1.0f);
                    tvTransform = glm::translate(tvTransform, glm::vec3(-3.0f, 2.0f, 0.0f));
                    tvTransform = glm::rotate(tvTransform, glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                    tvTransform = glm::scale(tvTransform, glm::vec3(2.0f, 2.0f, 1.0f));

                    // Rendered textures store row 0 = top of the view, but the batch
                    // quad's default UVs put v=0 at the quad's BOTTOM corner (image
                    // convention) — sample with V flipped so the TV reads upright.
                    // Corner order: BL, BR, TR, TL.
                    static const glm::vec2 kRttUVs[4] = {
                        { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f }
                    };
                    renderer->drawQuad3D(tvTransform, rtTexHandle, kRttUVs, glm::vec4(1.0f));
                }
            }
#endif


            renderer->draw(registry, scene, tempCamera);

            ImGui::Render();
            renderer->endFrame();
        }

        frameCount++;
    }

    // Shutdown subsystems
    physics->deinit();
    engineCore->shutdown();
    renderer->shutdown();

    ImGui::DestroyContext();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}