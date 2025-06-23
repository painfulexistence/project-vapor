#pragma once
#include <SDL3/SDL_video.h>
#include <memory>
#include "graphics.hpp"
#include "scene.hpp"
#include "camera.hpp"

enum class GraphicsBackend {
    Metal,
    Vulkan
};

enum class BufferUsage {
    VERTEX,
    INDEX,
    UNIFORM,
    STORAGE,
    COPY_SRC,
    COPY_DST
};

enum class GPUImageUsage {
    COLOR_MSAA,
    COLOR,
    DEPTH,
    DEPTH_STENCIL
};

class Renderer {
public:
    ~Renderer(){};

    virtual void init() = 0;

    virtual void stage(std::shared_ptr<Scene> scene) = 0;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) = 0;
};

std::unique_ptr<Renderer> createRendererMetal(SDL_Window* window);
std::unique_ptr<Renderer> createRendererVulkan(SDL_Window* window);

inline std::unique_ptr<Renderer> createRenderer(GraphicsBackend backend, SDL_Window* window) {
    switch (backend) {
    case GraphicsBackend::Metal:
        return createRendererMetal(window);
    case GraphicsBackend::Vulkan:
        return createRendererVulkan(window);
    default:
        return nullptr;
    }
}