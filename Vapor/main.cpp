#include "SDL.h"
#include "fmt/core.h"
#include "renderer_metal.hpp"

int main(int argc, char* args[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fmt::print("SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }
    auto window = SDL_CreateWindow(
      "MyApp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 450, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    auto renderer = (Renderer*)new Renderer_Metal(window);
    renderer->init();

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            switch (e.type) {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_KEYDOWN:
                break;
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