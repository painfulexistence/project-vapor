#pragma once
#include "renderer.hpp"

#include "SDL.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

class Renderer_Metal final : Renderer {
public:
    Renderer_Metal(SDL_Window* window);

    ~Renderer_Metal();

    virtual void init();

    virtual void draw();

private:
    SDL_Renderer* renderer;
    CA::MetalLayer* swapchain;
    MTL::Device* device;
    MTL::CommandQueue* queue;
};