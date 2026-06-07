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
    // 1. PointLightComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<PointLightComponent>(e)) {
            if (ImGui::CollapsingHeader("Point Light")) {
                ImGui::ColorEdit3("Color", &c->color.x);
                ImGui::DragFloat("Intensity", &c->intensity, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Radius", &c->radius, 0.05f, 0.01f, 50.0f);
            }
        }
    });

    // 2. DirectionalLightComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<DirectionalLightComponent>(e)) {
            if (ImGui::CollapsingHeader("Directional Light")) {
                ImGui::DragFloat3("Direction", &c->direction.x, 0.01f, -1.0f, 1.0f);
                ImGui::ColorEdit3("Color", &c->color.x);
                ImGui::DragFloat("Intensity", &c->intensity, 0.1f, 0.0f, 100.0f);
            }
        }
    });

    // 3. CharacterIntent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<CharacterIntent>(e)) {
            if (ImGui::CollapsingHeader("Character Intent")) {
                ImGui::LabelText("Look Vector", "(%.2f, %.2f)", c->lookVector.x, c->lookVector.y);
                ImGui::LabelText("Move Vector", "(%.2f, %.2f)", c->moveVector.x, c->moveVector.y);
                ImGui::LabelText("Vertical Axis", "%.2f", c->moveVerticalAxis);
                ImGui::LabelText("Jump", c->jump ? "true" : "false");
                ImGui::LabelText("Sprint", c->sprint ? "true" : "false");
                ImGui::LabelText("Interact", c->interact ? "true" : "false");
            }
        }
    });

    // 4. CharacterControllerComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<CharacterControllerComponent>(e)) {
            if (ImGui::CollapsingHeader("Character Controller")) {
                ImGui::DragFloat("Move Speed", &c->moveSpeed, 0.1f, 0.1f, 50.0f);
                ImGui::DragFloat("Rotate Speed", &c->rotateSpeed, 1.0f, 1.0f, 360.0f);
            }
        }
    });

    // 5. GrabbableComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<GrabbableComponent>(e)) {
            if (ImGui::CollapsingHeader("Grabbable")) {
                ImGui::DragFloat("Pickup Range", &c->pickupRange, 0.1f, 0.0f, 50.0f);
                ImGui::DragFloat("Hold Offset", &c->holdOffset, 0.1f, 0.0f, 20.0f);
                ImGui::DragFloat("Throw Force", &c->throwForce, 10.0f, 0.0f, 5000.0f);
                ImGui::Checkbox("Is Held", &c->isHeld);
            }
        }
    });

    // 6. LightMovementLogicComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<LightMovementLogicComponent>(e)) {
            if (ImGui::CollapsingHeader("Light Movement Logic")) {
                const char* patterns[] = { "Circle", "Figure8", "Linear", "Spiral" };
                int p = static_cast<int>(c->pattern);
                if (ImGui::Combo("Pattern", &p, patterns, 4))
                    c->pattern = static_cast<MovementPattern>(p);
                ImGui::DragFloat("Speed", &c->speed, 0.01f);
                ImGui::DragFloat("Radius", &c->radius, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Height", &c->height, 0.1f, -50.0f, 50.0f);
                ImGui::LabelText("Timer", "%.2f", c->timer);
            }
        }
    });

    // 7. FirstPersonCameraComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<FirstPersonCameraComponent>(e)) {
            if (ImGui::CollapsingHeader("First Person Camera")) {
                ImGui::DragFloat("Move Speed", &c->moveSpeed, 0.1f, 0.1f, 50.0f);
                ImGui::DragFloat("Rotate Speed", &c->rotateSpeed, 1.0f, 1.0f, 360.0f);
                ImGui::DragFloat("Yaw", &c->yaw, 0.5f);
                ImGui::DragFloat("Pitch", &c->pitch, 0.5f, -89.0f, 89.0f);
            }
        }
    });

    // 8. CameraSwitchRequest
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<CameraSwitchRequest>(e)) {
            if (ImGui::CollapsingHeader("Camera Switch Request")) {
                const char* modes[] = { "Free", "Follow", "FirstPerson" };
                int m = static_cast<int>(c->mode);
                if (ImGui::Combo("Mode", &m, modes, 3))
                    c->mode = static_cast<CameraSwitchRequest::Mode>(m);
            }
        }
    });

    // 9. AutoRotateComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<AutoRotateComponent>(e)) {
            if (ImGui::CollapsingHeader("Auto Rotate")) {
                ImGui::DragFloat3("Axis", &c->axis.x, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat("Speed", &c->speed, 0.05f);
            }
        }
    });

    // 10. DirectionalLightLogicComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<DirectionalLightLogicComponent>(e)) {
            if (ImGui::CollapsingHeader("Directional Light Logic")) {
                ImGui::DragFloat3("Base Dir", &c->baseDirection.x, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat("Speed", &c->speed, 0.05f);
                ImGui::DragFloat("Magnitude", &c->magnitude, 0.005f, 0.0f, 1.0f);
                ImGui::LabelText("Timer", "%.2f", c->timer);
            }
        }
    });

    // 11. SubtitleQueueComponent + FSM
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<SubtitleQueueComponent>(e)) {
            if (ImGui::CollapsingHeader("Subtitle Queue Component")) {
                ImGui::LabelText("Queue Size", "%zu", c->queue.size());
                ImGui::LabelText("Current Index", "%d", c->currentIndex);
                ImGui::Checkbox("Advance Requested", &c->advanceRequested);
                ImGui::Checkbox("Auto Advance", &c->autoAdvance);
                if (auto* fsm = reg.try_get<Vapor::FSMStateComponent>(e)) {
                    const char* states[] = { "Idle", "WaitingForVisible", "Displaying", "WaitingForHidden" };
                    ImGui::LabelText("State", "%s", states[fsm->currentState]);
                    ImGui::LabelText("State Time", "%.2f", fsm->stateTime);
                }
                ImGui::LabelText("Display Timer", "%.2f", c->displayTimer);
            }
        }
    });

    // 12. ScrollTextQueueComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<ScrollTextQueueComponent>(e)) {
            if (ImGui::CollapsingHeader("Scroll Text Queue")) {
                ImGui::LabelText("Lines Count", "%zu", c->lines.size());
                ImGui::LabelText("Current Index", "%d", c->currentIndex);
                ImGui::Checkbox("Advance Requested", &c->advanceRequested);
            }
        }
    });

    // 13. ChapterTitleTriggerComponent
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<ChapterTitleTriggerComponent>(e)) {
            if (ImGui::CollapsingHeader("Chapter Title Trigger")) {
                ImGui::LabelText("Num", "%s", c->number.c_str());
                ImGui::LabelText("Title", "%s", c->title.c_str());
                ImGui::Checkbox("Show Requested", &c->showRequested);
            }
        }
    });

    // 14. SceneTransitionComponent + FSM
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (auto* c = reg.try_get<SceneTransitionComponent>(e)) {
            if (ImGui::CollapsingHeader("Scene Transition")) {
                ImGui::LabelText("Target Scene", "%s", c->targetScene.c_str());
                if (auto* fsm = reg.try_get<Vapor::FSMStateComponent>(e)) {
                    const char* states[] = { "Idle", "FadingInLoadingScreen", "UnloadingScene", "LoadingAssets", "BuildingScene", "FadingOutLoadingScreen" };
                    ImGui::LabelText("State", "%s", states[fsm->currentState]);
                }
                ImGui::ProgressBar(c->progress);
            }
        }
    });

    // 15. PersistentTag
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (reg.all_of<PersistentTag>(e)) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.4f, 0.2f, 1.0f));
            ImGui::CollapsingHeader("PersistentTag", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet);
            ImGui::PopStyleColor();
        }
    });

    // 16. DeadTag
    inspector.registerCustomDrawer([](entt::registry& reg, entt::entity e) {
        if (reg.all_of<DeadTag>(e)) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            ImGui::CollapsingHeader("DeadTag", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet);
            ImGui::PopStyleColor();
        }
    });

    // Custom menu drawers
    inspector.registerCustomMenuDrawer([](entt::registry& reg, entt::entity e) {
        auto tryAdd = [&]<typename T>(const char* label) {
            if (!reg.all_of<T>(e) && ImGui::MenuItem(label)) {
                reg.emplace<T>(e);
                ImGui::CloseCurrentPopup();
            }
        };
        tryAdd.operator()<AutoRotateComponent>("Auto Rotate");
        tryAdd.operator()<GrabbableComponent>("Grabbable");
        tryAdd.operator()<CharacterControllerComponent>("Character Controller");
        tryAdd.operator()<CharacterIntent>("Character Intent");
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
    // Renderer is already initialized by createRenderer()

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
    FontHandle gameFont = renderer->loadFont("fonts/Arial Black.ttf", 48.0f);
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

    if (engineCore->initRmlUI(windowWidth, windowHeight) && renderer->initUI()) {
        fmt::print("RmlUI System Initialized\n");
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
    renderer->setImGuiCallback([&]() { sceneInspector.draw(registry); });

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
        // Decompose baked world matrix so the inspector shows meaningful values
        tc.position = glm::vec3(worldMat[3]);
        tc.scale = glm::vec3(
            glm::length(glm::vec3(worldMat[0])),
            glm::length(glm::vec3(worldMat[1])),
            glm::length(glm::vec3(worldMat[2]))
        );
        if (tc.scale.x > 0.0f && tc.scale.y > 0.0f && tc.scale.z > 0.0f) {
            glm::mat3 rotMat(
                glm::vec3(worldMat[0]) / tc.scale.x,
                glm::vec3(worldMat[1]) / tc.scale.y,
                glm::vec3(worldMat[2]) / tc.scale.z
            );
            tc.rotation = glm::quat_cast(rotMat);
        }
        tc.worldTransform = worldMat;
        tc.isDirty = false;// worldTransform already correct; skip TransformSystem
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

            // ===== Render-to-Texture Demo =====
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

            // ===== 3D Batch Demo =====
            renderer->drawQuad3D(
                glm::vec3(0.0f, 2.0f, 0.0f), glm::vec2(1.0f, 1.0f), spriteTexture, glm::vec4(1.0f, 0.5f, 0.5f, 1.0f)
            );

            // Draw the render texture on a 3D quad (like a TV screen in the world)
            if (rtTexHandle.isValid()) {
                // Create a transform for the "TV screen"
                glm::mat4 tvTransform = glm::mat4(1.0f);
                tvTransform = glm::translate(tvTransform, glm::vec3(-3.0f, 2.0f, 0.0f));
                tvTransform = glm::rotate(tvTransform, glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                tvTransform = glm::scale(tvTransform, glm::vec3(2.0f, 2.0f, 1.0f));

                renderer->drawQuad3D(tvTransform, rtTexHandle, nullptr, glm::vec4(1.0f));
            }


            renderer->draw(registry, scene, tempCamera);
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