#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <args.hxx>
#include <fmt/core.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include <iostream>

#include "Vapor/asset_manager.hpp"
#include "Vapor/camera.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/mesh_builder.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/rmlui_manager.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/scene.hpp"
#include <RmlUi/Core/ElementDocument.h>
#include <entt/entt.hpp>


#include "components.hpp"

// System Functions
void updateLightMovementSystem(entt::registry& registry, Scene* scene, float deltaTime) {
    auto view = registry.view<ScenePointLightReferenceComponent, LightMovementLogicComponent>();

    for (auto entity : view) {
        auto& ref = view.get<ScenePointLightReferenceComponent>(entity);
        auto& logic = view.get<LightMovementLogicComponent>(entity);

        if (ref.lightIndex < 0 || ref.lightIndex >= scene->pointLights.size()) continue;

        auto& light = scene->pointLights[ref.lightIndex];
        logic.timer += deltaTime * logic.speed;

        float x = 0.0f, y = 0.0f, z = 0.0f;

        switch (logic.pattern) {
        case MovementPattern::Circle:
            x = cos(logic.timer) * logic.radius;
            z = sin(logic.timer) * logic.radius;
            y = logic.height;
            break;
        case MovementPattern::Figure8:
            x = cos(logic.timer) * logic.radius;
            z = sin(logic.timer * 2.0f) * (logic.radius * 0.5f);
            y = logic.height;
            break;
        case MovementPattern::Linear:
            x = sin(logic.timer) * logic.radius;
            y = logic.height;
            z = 0.0f;
            break;
        case MovementPattern::Spiral:
            x = cos(logic.timer) * (logic.radius + sin(logic.timer * 0.5f));
            z = sin(logic.timer) * (logic.radius + sin(logic.timer * 0.5f));
            y = logic.height + sin(logic.timer * 0.2f);
            break;
        }

        light.position = glm::vec3(x, y, z);
        // Optional: intensity modulation
        // light.intensity = 5.0f + sin(logic.timer * 2.0f) * 2.0f;
    }
}

void updateAutoRotateSystem(entt::registry& registry, float deltaTime) {
    auto view = registry.view<SceneNodeReferenceComponent, AutoRotateComponent>();
    for (auto entity : view) {
        auto& ref = view.get<SceneNodeReferenceComponent>(entity);
        auto& rotate = view.get<AutoRotateComponent>(entity);
        if (ref.node) {
            ref.node->rotate(rotate.axis, rotate.speed * deltaTime);
        }
    }
}

void updateDirectionalLightSystem(entt::registry& registry, Scene* scene, float deltaTime) {
    auto view = registry.view<SceneDirectionalLightReferenceComponent, DirectionalLightLogicComponent>();
    for (auto entity : view) {
        auto& ref = view.get<SceneDirectionalLightReferenceComponent>(entity);
        auto& logic = view.get<DirectionalLightLogicComponent>(entity);
        if (ref.lightIndex >= 0 && ref.lightIndex < scene->directionalLights.size()) {
            logic.timer += deltaTime * logic.speed;
            // Simple oscillation on Z axis relative to base direction
            glm::vec3 newDir = logic.baseDirection;
            newDir.z += logic.magnitude * sin(logic.timer);
            scene->directionalLights[ref.lightIndex].direction = glm::normalize(newDir);
        }
    }
}

void updateCameraSystem(entt::registry& registry, Vapor::InputManager& inputManager, float deltaTime) {
    auto view = registry.view<Vapor::VirtualCameraComponent>();
    const auto& inputState = inputManager.getInputState();

    for (auto entity : view) {
        auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);
        if (!cam.isActive) continue;

        // 1. Handle Fly Camera Logic
        if (auto* fly = registry.try_get<FlyCameraComponent>(entity)) {
            // Rotation
            if (inputState.isHeld(Vapor::InputAction::LookUp)) fly->pitch -= fly->rotateSpeed * deltaTime;
            if (inputState.isHeld(Vapor::InputAction::LookDown)) fly->pitch += fly->rotateSpeed * deltaTime;
            if (inputState.isHeld(Vapor::InputAction::LookLeft)) fly->yaw += fly->rotateSpeed * deltaTime;
            if (inputState.isHeld(Vapor::InputAction::LookRight)) fly->yaw -= fly->rotateSpeed * deltaTime;

            fly->pitch = glm::clamp(fly->pitch, -89.0f, 89.0f);

            cam.rotation = glm::quat(glm::vec3(glm::radians(-fly->pitch), glm::radians(fly->yaw - 90.0f), 0.0f));

            glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);
            glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
            glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);

            float speed = fly->moveSpeed * deltaTime;
            if (inputState.isHeld(Vapor::InputAction::MoveForward)) cam.position += front * speed;
            if (inputState.isHeld(Vapor::InputAction::MoveBackward)) cam.position -= front * speed;
            if (inputState.isHeld(Vapor::InputAction::StrafeLeft)) cam.position -= right * speed;
            if (inputState.isHeld(Vapor::InputAction::StrafeRight)) cam.position += right * speed;
            if (inputState.isHeld(Vapor::InputAction::MoveUp)) cam.position += up * speed;
            if (inputState.isHeld(Vapor::InputAction::MoveDown)) cam.position -= up * speed;
        }

        // 2. Handle Follow Camera Logic
        if (auto* follow = registry.try_get<FollowCameraComponent>(entity)) {
            if (follow->targetNode) {
                glm::vec3 targetPos = follow->targetNode->getWorldPosition();
                glm::vec3 desiredPos = targetPos + follow->offset;

                cam.position = glm::mix(cam.position, desiredPos, 1.0f - pow(follow->smoothFactor, deltaTime));
                cam.rotation = glm::quatLookAt(glm::normalize(targetPos - cam.position), glm::vec3(0, 1, 0));
            }
        }

        // 3. Update Matrices
        glm::mat4 rotation = glm::mat4_cast(cam.rotation);
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
        cam.viewMatrix = glm::inverse(translation * rotation);
        cam.projectionMatrix = glm::perspective(cam.fov, cam.aspect, cam.near, cam.far);
    }
}

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
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    RNG rng;

    // Initialize engine core with enkiTS task scheduler
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();// Auto-detects thread count
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
        true,// optimized
        Vapor::LoadMode::Async,
        [](std::shared_ptr<Scene> loadedScene) {
            fmt::print("Scene loaded with {} nodes\n", loadedScene->nodes.size());
        }
    );

    entt::registry registry;

    // Wait for scene to be ready (blocking for now, but async in background)
    auto scene = sceneResource->get();
    fmt::print("Scene ready for rendering\n");
    scene->directionalLights.push_back({
        .direction = glm::vec3(0.5, -1.0, 0.0),
        .color = glm::vec3(1.0, 1.0, 1.0),
        .intensity = 10.0,
    });
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
    // Load textures asynchronously
    fmt::print("Loading textures...\n");
    auto albedoResource =
        resourceManager.loadImage(std::string("assets/textures/american_walnut_albedo.png"), Vapor::LoadMode::Async);
    auto normalResource =
        resourceManager.loadImage(std::string("assets/textures/american_walnut_normal.png"), Vapor::LoadMode::Async);
    auto roughnessResource =
        resourceManager.loadImage(std::string("assets/textures/american_walnut_roughness.png"), Vapor::LoadMode::Async);

    // Wait for all textures to load
    resourceManager.waitForAll();

    auto material = std::make_shared<Material>(Material{
        .albedoMap = albedoResource->get(),
        .normalMap = normalResource->get(),
        .roughnessMap = roughnessResource->get(),
    });
    fmt::print("Textures loaded\n");
    auto entity1 = scene->createNode("Cube 1");
    scene->addMeshToNode(entity1, MeshBuilder::buildCube(1.0f, material));
    entity1->setPosition(glm::vec3(-2.0f, 10.5f, 0.0f));
    entity1->body = physics->createBoxBody(
        glm::vec3(.5f, .5f, .5f), glm::vec3(-2.0f, 0.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Dynamic
    );
    physics->addBody(entity1->body, true);
    auto entity2 = scene->createNode("Cube 2");
    scene->addMeshToNode(entity2, MeshBuilder::buildCube(1.0f, material));
    entity2->setPosition(glm::vec3(2.0f, 0.5f, 0.0f));
    entity2->body = physics->createBoxBody(
        glm::vec3(.5f, .5f, .5f), glm::vec3(2.0f, 0.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Dynamic
    );
    physics->addBody(entity2->body, true);
    auto entity3 = scene->createNode("Floor");
    entity3->setPosition(glm::vec3(0.0f, -0.5f, 0.0f));
    entity3->body = physics->createBoxBody(
        glm::vec3(50.0f, .5f, 50.0f), glm::vec3(0.0f, -.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Static
    );
    physics->addBody(entity3->body, false);

    renderer->stage(scene);

    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

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

    // Create Camera Entities

    // 1. Fly Camera
    auto flyCam = registry.create();
    {
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(flyCam);
        cam.isActive = true;// Start active
        cam.fov = glm::radians(60.0f);
        cam.aspect = (float)windowWidth / (float)windowHeight;
        cam.position = glm::vec3(0.0f, 0.0f, 3.0f);// Set initial position directly

        auto& fly = registry.emplace<FlyCameraComponent>(flyCam);
        fly.moveSpeed = 5.0f;
    }

    // 2. Follow Camera (following Cube 1)
    auto followCam = registry.create();
    {
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(followCam);
        cam.isActive = false;
        cam.aspect = (float)windowWidth / (float)windowHeight;
        cam.position = glm::vec3(0.0f, 2.0f, 5.0f);// Initial pos

        auto& follow = registry.emplace<FollowCameraComponent>(followCam);
        follow.targetNode = entity1;// Direct reference to Node
        follow.offset = glm::vec3(0.0f, 2.0f, 5.0f);
    }

    // Create Auto-Rotate Entity for Cube 1
    auto rotateEntity = registry.create();
    registry.emplace<SceneNodeReferenceComponent>(rotateEntity);
    registry.get<SceneNodeReferenceComponent>(rotateEntity).node = entity1;

    registry.emplace<AutoRotateComponent>(rotateEntity);
    auto& rotateComp = registry.get<AutoRotateComponent>(rotateEntity);
    rotateComp.axis = glm::vec3(0.0f, 1.0f, -1.0f);
    rotateComp.speed = 1.5f;

    // Create Directional Light Logic Entity
    auto dirLightEntity = registry.create();
    registry.emplace<SceneDirectionalLightReferenceComponent>(dirLightEntity);
    registry.get<SceneDirectionalLightReferenceComponent>(dirLightEntity).lightIndex = 0;

    registry.emplace<DirectionalLightLogicComponent>(dirLightEntity);
    auto& dirLightLogic = registry.get<DirectionalLightLogicComponent>(dirLightEntity);
    dirLightLogic.baseDirection = glm::vec3(0.5, -1.0, 0.0);
    dirLightLogic.speed = 0.5f;
    dirLightLogic.magnitude = 0.05f;

    Rml::ElementDocument* hudDocument = nullptr;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    if (engineCore->initRmlUI(windowWidth, windowHeight) && renderer->initUI()) {
        if (auto* rmluiManager = engineCore->getRmlUiManager()) {
            hudDocument = rmluiManager->LoadDocument("assets/ui/hud.rml");
            if (hudDocument) {
                hudDocument->Show();
            }
        }
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
                    quit = true;
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
            auto view = registry.view<Vapor::VirtualCameraComponent>();
            view.each([&](auto entity, auto& cam) { cam.isActive = registry.all_of<FlyCameraComponent>(entity); });
        }
        if (inputState.isPressed(Vapor::InputAction::Hotkey2)) {
            auto view = registry.view<Vapor::VirtualCameraComponent>();
            view.each([&](auto entity, auto& cam) { cam.isActive = registry.all_of<FollowCameraComponent>(entity); });
        }

        // Gameplay updates
        updateCameraSystem(registry, inputManager, deltaTime);
        updateAutoRotateSystem(registry, deltaTime);
        updateDirectionalLightSystem(registry, scene.get(), deltaTime);
        updateLightMovementSystem(registry, scene.get(), deltaTime);

        // Engine updates
        engineCore->update(deltaTime);

        // Transform
        scene->update(deltaTime);

        // Physics
        physics->process(scene, deltaTime);

        // Rendering
        entt::entity activeCamEntity = getActiveCamera(registry);
        if (activeCamEntity != entt::null) {
            auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCamEntity);
            Camera tempCamera;// Create a temporary Camera object
            tempCamera.setEye(cam.position);// Set position for lighting/etc
            tempCamera.setViewMatrix(cam.viewMatrix);
            tempCamera.setProjectionMatrix(cam.projectionMatrix);

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