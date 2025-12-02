#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <args.hxx>
#include <iostream>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"

#include "Vapor/renderer.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/engine_core.hpp"

#include "hot_reload/game_memory.hpp"
#include "hot_reload/module_loader.hpp"

// Get the gameplay module path based on platform
std::string getGameplayModulePath() {
#ifdef _WIN32
    return "./gameplay/Gameplay";
#else
    return "./gameplay/libGameplay";
#endif
}

int main(int argc, char* args[]) {
    // Parse command line arguments
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

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print("SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    // Create window
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
        fmt::print("Failed to create window: {}\n", SDL_GetError());
        return 1;
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Initialize engine subsystems
    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();
    fmt::print("Engine core initialized\n");

    auto renderer = createRenderer(gfxBackend);
    renderer->init(window);

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler());

    // Setup GameMemory with engine services
    Game::GameMemory gameMemory;
    gameMemory.window = window;
    gameMemory.renderer = renderer.get();
    gameMemory.physics = physics.get();
    gameMemory.engine = engineCore.get();

    // Load gameplay module
    Game::ModuleLoader moduleLoader;
    if (!moduleLoader.load(getGameplayModulePath())) {
        fmt::print("Failed to load gameplay module: {}\n", moduleLoader.getLastError());
        fmt::print("Cannot run without gameplay DLL\n");
        return 1;
    }

    // Initialize game
    auto initFunc = moduleLoader.getInitFunc();
    if (!initFunc(&gameMemory)) {
        fmt::print("Game initialization failed\n");
        return 1;
    }

    // Main loop
    auto& inputManager = engineCore->getInputManager();
    float time = SDL_GetTicks() / 1000.0f;
    bool quit = false;

    while (!quit) {
        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = currTime - time;
        time = currTime;

        // Update input manager
        inputManager.update(deltaTime);

        // Process SDL events
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            inputManager.processEvent(e);

            switch (e.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit = true;
                }
                // F5 to hot reload
                if (e.key.scancode == SDL_SCANCODE_F5) {
                    fmt::print("[Main] Hot reload triggered (F5)\n");
                    if (moduleLoader.reload()) {
                        fmt::print("[Main] Hot reload successful\n");
                    } else {
                        fmt::print("[Main] Hot reload failed: {}\n", moduleLoader.getLastError());
                    }
                }
                break;
            default:
                break;
            }
        }

        // Call game update
        Game::FrameInput frameInput;
        frameInput.deltaTime = deltaTime;
        frameInput.totalTime = time;
        frameInput.inputState = &inputManager.getInputState();

        auto updateFunc = moduleLoader.getUpdateFunc();
        if (updateFunc) {
            bool continueRunning = updateFunc(&gameMemory, &frameInput);
            if (!continueRunning) {
                quit = true;
            }
        }
    }

    // Shutdown
    if (moduleLoader.isLoaded()) {
        auto shutdownFunc = moduleLoader.getShutdownFunc();
        if (shutdownFunc) {
            auto memoryFunc = moduleLoader.getMemoryFunc();
            if (memoryFunc) {
                shutdownFunc(memoryFunc());
            }
        }
        moduleLoader.unload();
    }

    renderer->deinit();
    physics->deinit();
    engineCore->shutdown();

    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
