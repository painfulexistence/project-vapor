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

enum class RenderTargetUsage {
    COLOR_MSAA,
    COLOR,
    DEPTH_MSAA,
    DEPTH_STENCIL_MSAA,
    DEPTH,
    DEPTH_STENCIL
};

class Renderer {
public:
    ~Renderer(){};

    virtual void init() = 0;

    virtual void stage(std::shared_ptr<Scene> scene) = 0;

    virtual void draw(std::shared_ptr<Scene> scene, Camera& camera) = 0;

protected:
    const Uint32 MAX_FRAMES_IN_FLIGHT = 3;
    const Uint32 MSAA_SAMPLE_COUNT = 4;
    const Uint32 MAX_INSTANCES = 1000;
    glm::vec4 clearColor = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f);
    double clearDepth = 1.0;
    Uint32 clusterGridSizeX = 16;
    Uint32 clusterGridSizeY = 16;
    Uint32 clusterGridSizeZ = 24;
    Uint32 numClusters = clusterGridSizeX * clusterGridSizeY * clusterGridSizeZ;
    Uint32 currentFrameInFlight = 0;

    int calculateMipmapLevelCount(Uint32 width, Uint32 height) const {
        return static_cast<int>(std::floor(std::log2(std::max(width, height))) + 1);
    }

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