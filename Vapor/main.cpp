#include "SDL.h"
#include "fmt/core.h"
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
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fmt::print("SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }
    auto window = SDL_CreateWindow(
      "MyApp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 600,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN
    );

    auto renderer = createRenderer(GraphicsBackend::Vulkan, window);
    renderer->init();

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            switch (e.type) {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_KEYDOWN: {
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    quit = true;
                    break;
                default:
                    break;
                }
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