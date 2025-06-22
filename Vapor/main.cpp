#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <args.hxx>
#include <iostream>
#include "scene.hpp"
#include "renderer.hpp"
#include "graphics.hpp"
#include "asset_manager.hpp"
#include "camera.hpp"


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
        winTitle, 800, 600, winFlags
    );

    auto renderer = createRenderer(gfxBackend, window);
    renderer->init();

    Scene scene;
    auto entity = scene.CreateNode("Mesh 1");
    scene.AddMeshToNode(entity, MeshBuilder::buildCube(1.0f));
    // scene.AddMeshToNode(entity, AssetManager::loadOBJ(std::string("assets/models/Sibenik/sibenik.obj"), std::string("assets/models/Sibenik/")));

    renderer->stage(scene);

    Camera camera = Camera(
        glm::vec3(0.0f, 0.0f, 3.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::radians(60.0f),
        800 / (float)600,
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
            camera.Dolly(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_S]) {
            camera.Dolly(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_D]) {
            camera.Truck(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_A]) {
            camera.Truck(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_R]) {
            camera.Pedestal(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_F]) {
            camera.Pedestal(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_I]) {
            camera.Tilt(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_K]) {
            camera.Tilt(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_L]) {
            camera.Pan(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_J]) {
            camera.Pan(1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_U]) {
            camera.Roll(-1.0f * deltaTime);
        }
        if (keyboardState[SDL_SCANCODE_O]) {
            camera.Roll(1.0f * deltaTime);
        }

        scene.Update(deltaTime);

        renderer->draw(scene, camera);

        frameCount++;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}