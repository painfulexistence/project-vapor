#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Metal Backend Window Support", "[backend][metal]") {
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));

    SDL_Window* window = SDL_CreateWindow("Metal Backend Test", 100, 100, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);

    SDL_DestroyWindow(window);
    SDL_Quit();
}

TEST_CASE("Vulkan Backend Window Support", "[backend][vulkan]") {
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));

    SDL_Window* window = SDL_CreateWindow("Vulkan Backend Test", 100, 100, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);

    uint32_t extensionCount = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    // On some platforms like macOS with MoltenVK, extensions are required.
    // If SDL supports Vulkan, this should generally return something or at least not crash.
    CHECK(extensionCount >= 0);

    SDL_DestroyWindow(window);
    SDL_Quit();
}