#include "SDL3/SDL.h"
#include "fmt/core.h"
#include "args.hxx"
#include "renderer_metal.hpp"
#include "renderer_vulkan.hpp"

enum GraphicsBackend {
    Metal,
    Vulkan
};

Renderer* createRenderer(GraphicsBackend backend, SDL_Window* window) {
    switch (backend) {
    case Metal:
        return (Renderer*)new Renderer_Metal(window);
    case Vulkan:
        return (Renderer*)new Renderer_Vulkan(window);
    default:
        return nullptr;
    }
}

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
    title = "Project Vapor (Vulkan)";
    flags |= SDL_WINDOW_VULKAN;
    backend = GraphicsBackend::Vulkan;
#endif

    auto window = SDL_CreateWindow(
        winTitle, 800, 600, winFlags
    );

    auto renderer = createRenderer(gfxBackend, window);
    renderer->init();

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN: {
                switch (e.key.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    quit = true;
                    break;
                default:
                    break;
                }
                break;
            }
            case SDL_EVENT_KEY_UP: {
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
        renderer->draw();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}