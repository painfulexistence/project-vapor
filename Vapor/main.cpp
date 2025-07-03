#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <args.hxx>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include <iostream>

#include "scene.hpp"
#include "renderer.hpp"
#include "graphics.hpp"
#include "physics_3d.hpp"
#include "asset_manager.hpp"
#include "mesh_builder.hpp"
#include "camera.hpp"
#include "rng.hpp"


int main(int argc, char* args[]) {
    args::ArgumentParser parser { "This is Project Vapor." };
    args::Group group(parser, "Select a gfx backend:", args::Group::Validators::Xor);
    args::Flag useMetal(group, "Metal", "Metal", {"metal"});
    args::Flag useVulkan(group, "Vulkan", "Vulkan", {"vulkan"});
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
        winTitle, 800, 800, winFlags
    );
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    RNG rng;

    auto renderer = createRenderer(gfxBackend);
    renderer->init(window);

    auto physics = std::make_unique<Physics3D>();
    physics->init();

    // auto scene = std::make_shared<Scene>();
    // auto entity = scene->CreateNode("Mesh 1");
    // auto mesh = MeshBuilder::buildCube(1.0f);
    // mesh->material = std::make_shared<Material>(Material {
    //     .albedoMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_diff.png")),
    //     .normalMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_norm_dx.png")),
    //     .metallicRoughnessMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_rough.png")),
    //     .occlusionMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_ao.png")),
    //     .displacementMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_disp.png"))
    // });
    // scene->AddMeshToNode(entity, mesh);
    // scene->AddMeshToNode(entity, AssetManager::loadOBJ(std::string("assets/models/Sibenik/sibenik.obj"), std::string("assets/models/Sibenik/")));
    auto scene = AssetManager::loadGLTFOptimized(std::string("assets/models/Sponza/Sponza.gltf"));
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
    auto material = std::make_shared<Material>(Material {
        .albedoMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_diff.jpg")),
        .normalMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_norm_dx.jpg")),
        .metallicRoughnessMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_rough.jpg")),
        .occlusionMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_ao.jpg")),
        .displacementMap = AssetManager::loadImage(std::string("assets/textures/medieval_blocks_disp.jpg"))
    });
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

        scene->update(deltaTime);
        physics->process(scene, deltaTime);
        // scene->update(deltaTime);

        renderer->draw(scene, camera);

        frameCount++;
    }
    renderer->deinit();

    ImGui::DestroyContext();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}