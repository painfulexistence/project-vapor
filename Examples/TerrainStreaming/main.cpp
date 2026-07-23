// ============================================================================
// TerrainStreaming — 10.24 km x 10.24 km streamed open-world terrain.
//
// The Vapor port of Atmospheric's Examples/TerrainStreaming, driven entirely
// by the engine's streamed-terrain subsystem: this app spawns one entity with
// a StreamingTerrainComponent and ticks Vapor::TerrainSystem — the system
// owns the TerrainWorld core (Vapor/terrain_world.hpp), prewarms the whole
// world at the coarsest LOD during the first update (full horizon on frame
// one), then refines concentric detail rings around the active camera on
// task-scheduler workers while you fly, with no geometry leaks: every tile
// mesh comes from a fixed per-LOD slot pool registered once and rewritten in
// place (IRenderer::updateMeshGeometry) as the rings move.
//
// Differences from the original demo: no grass ring, no RmlUi HUD (keyboard
// only), no physics colliders, and splat texturing is approximated by baking
// (height, slope) into each vertex's UV and sampling a generated palette LUT
// — one material for every tile, zero per-tile textures.
//
// Controls: WASD move, R/F up/down, IJKL look, LShift sprint (x50),
//           G toggle ground-clamp, T teleport +2km (streaming stress test),
//           Esc quit. (--vulkan / --metal pick the backend.)
// ============================================================================

#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/irenderer.hpp"
#include "Vapor/render_scene.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/scene_blueprint.hpp"
#include "Vapor/systems.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"

namespace {

// ECS fly-camera driver — same demo-local system as Examples/MicroVoxel;
// perspectiveZO because the RHI's clip depth is [0,1] on both backends.
// Sprint is x50 here (streaming stress), matching the original demo.
struct FlyCameraSystem {
    static void update(entt::registry& reg, const Vapor::InputState& input, float deltaTime) {
        auto view = reg.view<Vapor::VirtualCameraComponent, Vapor::FlyCameraComponent>();
        for (auto entity : view) {
            auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);
            auto& fly = view.get<Vapor::FlyCameraComponent>(entity);
            if (!cam.isActive) continue;

            glm::vec2 look = input.getVector(Vapor::InputAction::LookLeft, Vapor::InputAction::LookRight,
                                             Vapor::InputAction::LookDown, Vapor::InputAction::LookUp);
            glm::vec2 move = input.getVector(Vapor::InputAction::StrafeLeft, Vapor::InputAction::StrafeRight,
                                             Vapor::InputAction::MoveBackward, Vapor::InputAction::MoveForward);
            float vertical = input.getAxis(Vapor::InputAction::MoveDown, Vapor::InputAction::MoveUp);
            float moveSpeed = fly.moveSpeed * (input.isHeld(Vapor::InputAction::Sprint) ? 50.0f : 1.0f);

            fly.pitch -= look.y * fly.rotateSpeed * deltaTime;
            fly.yaw -= look.x * fly.rotateSpeed * deltaTime;
            fly.pitch = glm::clamp(fly.pitch, -89.0f, 89.0f);

            cam.rotation = glm::quat(glm::vec3(glm::radians(-fly.pitch), glm::radians(fly.yaw - 90.0f), 0.0f));
            glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);
            glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
            glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);
            cam.position += move.x * right * moveSpeed * deltaTime;
            cam.position += move.y * front * moveSpeed * deltaTime;
            cam.position += vertical * up * moveSpeed * deltaTime;

            glm::mat4 rotation = glm::mat4_cast(cam.rotation);
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
            cam.viewMatrix = glm::inverse(translation * rotation);
            cam.projectionMatrix = glm::perspectiveZO(cam.fov, cam.aspect, cam.near, cam.far);
        }
    }
};

entt::entity getActiveCamera(entt::registry& reg) {
    auto view = reg.view<Vapor::VirtualCameraComponent>();
    for (auto entity : view) {
        if (view.get<Vapor::VirtualCameraComponent>(entity).isActive) return entity;
    }
    return entt::null;
}

}  // namespace

auto main(int argc, char* args[]) -> int {
    bool wantVulkan = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(args[i], "--vulkan") == 0) wantVulkan = true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print(stderr, "SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    GraphicsBackend gfxBackend;
    const char* winTitle;
#if defined(__APPLE__)
    if (wantVulkan) {
        winTitle = "TerrainStreaming (Vulkan)";
        winFlags |= SDL_WINDOW_VULKAN;
        gfxBackend = GraphicsBackend::Vulkan;
    } else {
        winTitle = "TerrainStreaming (Metal)";
        winFlags |= SDL_WINDOW_METAL;
        gfxBackend = GraphicsBackend::Metal;
    }
#else
    winTitle = "TerrainStreaming (Vulkan)";
    winFlags |= SDL_WINDOW_VULKAN;
    gfxBackend = GraphicsBackend::Vulkan;
#endif

    auto window = SDL_CreateWindow(winTitle, 1280, 720, winFlags);
    if (!window) {
        fmt::print(stderr, "Failed to create SDL_Window: {}\n", SDL_GetError());
        return 1;
    }
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    auto renderer = createRenderer(gfxBackend, window);
    if (!renderer) {
        fmt::print(stderr, "Failed to create renderer (backend unavailable?)\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Declarative scene: the terrain (streamingTerrain component), sun, sky
    // and fly camera are authored in scenes/main.json; instantiate() fills
    // the registry and TerrainSystem does the rest on first update. Only
    // window-derived state (aspect) and the post-prewarm camera height are
    // set from code.
    auto scene = std::make_shared<RenderScene>("terrain");
    entt::registry registry;
    {
        auto& resourceManager = engineCore->getResourceManager();
        auto sceneResource = resourceManager.loadScene(std::string("scenes/main.json"));
        auto sceneBlueprint = sceneResource->get();
        if (sceneBlueprint && sceneBlueprint->ok) {
            Vapor::instantiate(registry, *scene, *sceneBlueprint);
        } else {
            fmt::print(stderr, "TerrainStreaming: scene blueprint failed to load; the world will be empty\n");
        }
    }
    renderer->stage(scene);
    scene->stagedMeshes.clear();
    scene->stagedMeshTransforms.clear();

    // The terrain singleton entity (stats + teleport bounds below).
    entt::entity terrainEntity = entt::null;
    {
        auto terrainView = registry.view<Vapor::StreamingTerrainComponent>();
        for (auto e : terrainView) {
            terrainEntity = e;
            break;
        }
        if (terrainEntity == entt::null) {
            fmt::print(stderr, "TerrainStreaming: no streamingTerrain entity in the scene\n");
            return 1;
        }
    }
    {
        auto camView = registry.view<Vapor::VirtualCameraComponent>();
        camView.each([&](auto& cam) {
            cam.aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
        });
    }

    // First system tick prewarms the whole horizon (base coat for every tile,
    // built in parallel) before the first frame is drawn.
    const auto bootStart = SDL_GetTicks();
    Vapor::TerrainSystem::update(registry, renderer.get(), scene);
    {
        const auto& tc = registry.get<Vapor::StreamingTerrainComponent>(terrainEntity);
        const float side = tc.world.value->config().worldSize / 1000.0f;
        fmt::print("TerrainStreaming: full {:.2f} km x {:.2f} km horizon ready in {} ms\n", side, side,
                   SDL_GetTicks() - bootStart);
    }
    {
        auto& cam = registry.get<Vapor::VirtualCameraComponent>(getActiveCamera(registry));
        cam.position.y = Vapor::TerrainSystem::groundHeight(registry, cam.position.x, cam.position.z) + 40.0f;
    }

    bool groundClamp = true;
    fmt::print("WASD move, R/F up/down, IJKL look, LShift sprint (x50), G ground-clamp, "
               "T teleport +2km, Esc quit.\n");

    auto& inputManager = engineCore->getInputManager();
    bool quit = false;
    float time = SDL_GetTicks() / 1000.0f;
    float statsTimer = 0.0f;

    while (!quit) {
        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = currTime - time;
        time = currTime;

        inputManager.update(deltaTime);

        SDL_Event e;
        while (SDL_PollEvent(&e) != 0) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            inputManager.processEvent(e);
            switch (e.type) {
                case SDL_EVENT_QUIT:
                    quit = true;
                    break;
                case SDL_EVENT_KEY_DOWN: {
                    if (e.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
                    if (e.key.repeat) break;
                    if (e.key.scancode == SDL_SCANCODE_G) {
                        groundClamp = !groundClamp;
                        fmt::print("Ground clamp: {}\n", groundClamp ? "on" : "off");
                    }
                    if (e.key.scancode == SDL_SCANCODE_T) {
                        // Teleport 2 km along the view direction, kept inside
                        // the world — a streaming stress test.
                        entt::entity ce = getActiveCamera(registry);
                        if (ce != entt::null) {
                            auto& cam = registry.get<Vapor::VirtualCameraComponent>(ce);
                            const auto& tc = registry.get<Vapor::StreamingTerrainComponent>(terrainEntity);
                            const float worldSize = tc.world.value->config().worldSize;
                            const float bound = 0.5f * worldSize - tc.world.value->config().tileSize;
                            glm::vec3 fwd = cam.rotation * glm::vec3(0, 0, -1);
                            fwd.y = 0.0f;
                            fwd = glm::length(fwd) > 1e-3f ? glm::normalize(fwd) : glm::vec3(1, 0, 0);
                            glm::vec3 pos = cam.position + fwd * 2000.0f;
                            pos.x = glm::clamp(pos.x, -bound, bound);
                            pos.z = glm::clamp(pos.z, -bound, bound);
                            pos.y = Vapor::TerrainSystem::groundHeight(registry, pos.x, pos.z) + 200.0f;
                            cam.position = pos;
                            fmt::print("Teleported to ({}, {})\n", static_cast<int>(pos.x),
                                       static_cast<int>(pos.z));
                        }
                    }
                    break;
                }
                case SDL_EVENT_WINDOW_RESIZED: {
                    windowWidth = e.window.data1;
                    windowHeight = e.window.data2;
                    auto camView = registry.view<Vapor::VirtualCameraComponent>();
                    camView.each([&](auto& cam) {
                        cam.aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
                    });
                    break;
                }
                default:
                    break;
            }
        }

        const auto& input = inputManager.getInputState();
        FlyCameraSystem::update(registry, input, deltaTime);

        entt::entity activeCam = getActiveCamera(registry);
        if (activeCam != entt::null && groundClamp) {
            // Keep the fly camera above the streamed terrain (after movement,
            // so the clamp applies to this frame).
            auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCam);
            cam.position.y = std::max(
                cam.position.y,
                Vapor::TerrainSystem::groundHeight(registry, cam.position.x, cam.position.z) + 2.0f);
        }

        engineCore->update(deltaTime);
        Vapor::TerrainSystem::update(registry, renderer.get(), scene);
        Vapor::TransformSystem::update(registry);
        Vapor::LightGatherSystem::update(registry, scene.get());
        Vapor::SkySystem::update(registry, renderer.get());

        statsTimer += deltaTime;
        if (statsTimer >= 5.0f) {
            statsTimer = 0.0f;
            const auto& tc = registry.get<Vapor::StreamingTerrainComponent>(terrainEntity);
            const auto s = tc.world.value->stats();
            int scatterCount = 0;
            for (const auto& [tile, ents] : tc.world.value->scatterEntities)
                scatterCount += static_cast<int>(ents.size());
            entt::entity ce = getActiveCamera(registry);
            const glm::vec3 pos =
                ce != entt::null ? registry.get<Vapor::VirtualCameraComponent>(ce).position : glm::vec3(0.0f);
            fmt::print("cam ({}, {}) lod0 {} lod1 {} lod2 {} base {} pending {} queued {} scatter {} cache {}/{}\n",
                       static_cast<int>(pos.x), static_cast<int>(pos.z), s.lodCounts[0], s.lodCounts[1],
                       s.lodCounts[2], s.lodCounts[3], s.pendingJobs, s.queuedResults, scatterCount,
                       s.cacheHits, s.cacheHits + s.cacheMisses);
        }

        if (activeCam == entt::null) continue;
        const auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCam);
        Camera tempCamera;
        tempCamera.setEye(cam.position);
        tempCamera.setViewMatrix(cam.viewMatrix);
        tempCamera.setProjectionMatrix(cam.projectionMatrix);

        CameraRenderData camData;
        camData.proj = cam.projectionMatrix;
        camData.view = cam.viewMatrix;
        camData.invProj = glm::inverse(cam.projectionMatrix);
        camData.invView = glm::inverse(cam.viewMatrix);
        camData.nearPlane = cam.near;
        camData.farPlane = cam.far;
        camData.position = cam.position;

        renderer->beginFrame(camData);
        ImGui::NewFrame();
        renderer->invokeImGuiCallback();
        renderer->draw(registry, scene, tempCamera);
        ImGui::Render();
        renderer->endFrame();
    }

    // Tile-build jobs hold pointers into the terrain world; drain them before
    // teardown.
    engineCore->getTaskScheduler().waitForAll();
    engineCore->shutdown();
    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
