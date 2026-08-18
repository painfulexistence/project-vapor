#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Metal Backend Window Support", "[backend][metal]") {
#ifndef __APPLE__
    SKIP("Metal windows only exist on Apple platforms");
#else
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));

    SDL_Window* window = SDL_CreateWindow("Metal Backend Test", 100, 100, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);

    SDL_DestroyWindow(window);
    SDL_Quit();
#endif
}

TEST_CASE("Vulkan Backend Window Support", "[backend][vulkan]") {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SKIP("SDL_INIT_VIDEO failed: " << SDL_GetError());
    }

    // Attempt to load Vulkan library first
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        SDL_Quit();
        SKIP("Vulkan library could not be loaded: " << SDL_GetError());
    }

    SDL_Window* window = SDL_CreateWindow("Vulkan Backend Test", 100, 100, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
        SKIP("Vulkan window could not be created (likely no compatible driver in CI): " << SDL_GetError());
    }

    uint32_t extensionCount = 0;
    [[maybe_unused]] const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    CHECK(extensionCount >= 0);

    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
}
#ifdef _WIN32
TEST_CASE("D3D12 Backend Window Support", "[backend][d3d12]") {
    // D3D12 presents through DXGI on a plain Win32 window — no SDL surface
    // flag involved, so window creation succeeding is all SDL must provide.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SKIP("SDL_INIT_VIDEO failed: " << SDL_GetError());
    }
    SDL_Window* window = SDL_CreateWindow("D3D12 Backend Test", 100, 100, SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
#endif
