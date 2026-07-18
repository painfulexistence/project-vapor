// ============================================================================
// MicroVoxel — raymarched micro-voxel diorama demo.
//
// The Vapor port of Atmospheric's Examples/MicroVoxel: three independent
// 5 cm-voxel volumes (procedural terrain + caves + ore + floating crystals +
// emissive glowstone), raymarched with a two-level DDA over sparse
// page-table/brick-pool storage — no triangles. The volumes depth-composite
// with each other through the MicroVoxel pass's depth writes, an angled warm
// sun gives raking shadows, and holding E digs spheres out of the terrain
// (per-brick uploads, no remeshing — the point of the raymarch model).
//
// The middle diorama runs 2.5 cm voxels (8x the density of the original) to
// show off per-volume detail scaling. Pass --big for a single
// 1024 x 256 x 1024 world (51 m across at 5 cm) instead of the dioramas —
// generation streams in chunk-by-chunk on the task scheduler while you fly.
//
// Controls: WASD move, R/F up/down, IJKL look, LShift sprint, hold E to dig,
// 0-6 debug views, H sun shadow, O ambient occlusion, X reflections, Esc quit.
// (--vulkan / --metal pick the backend, same as the Vaporware demo.)
// ============================================================================

#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/irenderer.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/scene.hpp"
#include "Vapor/systems.hpp"
#include "Vapor/voxel_world.hpp"

#include <SDL3/SDL.h>
#include <cstring>
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"

namespace {

struct FlyCamera {
    glm::vec3 position { 1.0f, 10.0f, 15.0f };
    float yaw = 0.0f;    // degrees; 0 = -Z forward
    float pitch = 0.0f;  // degrees
    float fov = glm::radians(60.0f);
    float nearPlane = 0.05f;
    float farPlane = 500.0f;

    glm::quat rotation() const {
        return glm::quat(glm::vec3(glm::radians(pitch), glm::radians(yaw), 0.0f));
    }
    glm::vec3 forward() const { return rotation() * glm::vec3(0.0f, 0.0f, -1.0f); }
};

entt::entity spawnVolume(entt::registry& registry, const char* name, glm::vec3 position,
                         glm::ivec3 gridDim, float voxelSize, Uint32 seed, Uint32 brickCapacity) {
    auto e = registry.create();
    registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent { name });
    auto& t = registry.emplace<Vapor::TransformComponent>(e);
    t.position = position;
    auto& vv = registry.emplace<Vapor::VoxelVolumeComponent>(e);
    vv.gridDim = gridDim;
    vv.voxelSize = voxelSize;
    vv.seed = seed;
    vv.brickCapacity = brickCapacity;
    return e;
}

}  // namespace

auto main(int argc, char* args[]) -> int {
    bool wantVulkan = false, wantMetal = false, bigWorld = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(args[i], "--vulkan") == 0) wantVulkan = true;
        if (std::strcmp(args[i], "--metal") == 0) wantMetal = true;
        if (std::strcmp(args[i], "--big") == 0) bigWorld = true;
    }
    (void)wantMetal;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print(stderr, "SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    GraphicsBackend gfxBackend;
    const char* winTitle;
#if defined(__APPLE__)
    if (wantVulkan) {
        winTitle = "MicroVoxel (Vulkan)";
        winFlags |= SDL_WINDOW_VULKAN;
        gfxBackend = GraphicsBackend::Vulkan;
    } else {
        winTitle = "MicroVoxel (Metal)";
        winFlags |= SDL_WINDOW_METAL;
        gfxBackend = GraphicsBackend::Metal;
    }
#else
    winTitle = "MicroVoxel (Vulkan)";
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

    // Empty scene: everything visible is raymarched, but draw()/light gather
    // still route through the Scene object.
    auto scene = std::make_shared<Scene>();
    renderer->stage(scene);

    entt::registry registry;

    // ---- Scene setup -------------------------------------------------------
    if (bigWorld) {
        // One large streaming world: 51.2 x 12.8 x 51.2 m of 5 cm voxels.
        // Surface bricks land in the pool; uniform stone interiors collapse to
        // page entries, so 128k slots (~75 MB) cover the whole thing.
        spawnVolume(registry, "Volume.Big", glm::vec3(0.0f), glm::ivec3(1024, 256, 1024), 0.05f,
                    1337u, 1u << 17);
    } else {
        // Three dioramas side by side, seeds matching the original demo. The
        // center one runs 2.5 cm voxels (256^3 = a 6.4 m diorama): twice the
        // detail per meter of the original's 5 cm — the "finer picture" the
        // sparse storage makes affordable.
        const float ext5 = 256 * 0.05f;  // 12.8 m
        spawnVolume(registry, "Volume.Center", glm::vec3(0.0f), glm::ivec3(256, 256, 256), 0.025f,
                    1337u, 1u << 16);
        spawnVolume(registry, "Volume.Right", glm::vec3(ext5, 0.0f, 0.0f), glm::ivec3(256), 0.05f,
                    7u, 1u << 16);
        spawnVolume(registry, "Volume.Left", glm::vec3(-ext5, 0.0f, 0.0f), glm::ivec3(256), 0.05f,
                    99u, 1u << 16);
    }

    // An angled warm sun, low over the horizon like the original demo, so the
    // terrain gets long raking shadows and grazing light. The MicroVoxel pass
    // reads it through atmosphereData (LightGatherSystem -> renderer).
    {
        auto sun = registry.create();
        registry.emplace<Vapor::NameComponent>(sun, Vapor::NameComponent { "Sun" });
        auto& dl = registry.emplace<Vapor::DirectionalLightComponent>(sun);
        // Original aims (-0.451, 10.179, -236.35) TOWARD the sun; Vapor's
        // directional light stores the light's travel direction.
        dl.direction = -glm::normalize(glm::vec3(-0.451f, 10.179f, -236.350f));
        dl.color = glm::vec3(1.0f, 0.95f, 0.85f);
        dl.intensity = 10.0f;
        registry.emplace<Vapor::SunComponent>(sun);

        auto env = registry.create();
        registry.emplace<Vapor::NameComponent>(env, Vapor::NameComponent { "Environment" });
        registry.emplace<Vapor::SkyComponent>(env);  // atmosphere sky + IBL rebake
    }

    // Initial camera: matches the original (at (1, 10, 15), pitched down 16°,
    // yawed to look across the dioramas toward -X).
    FlyCamera camera;
    camera.yaw = 90.0f;
    camera.pitch = -16.0f;
    if (bigWorld) {
        camera.position = glm::vec3(0.0f, 18.0f, 30.0f);
        camera.yaw = 0.0f;
    }

    fmt::print("MicroVoxel loaded. WASD move, R/F up/down, IJKL look, LShift sprint, Esc quit.\n");
    fmt::print("Raymarched 5 cm voxel volumes — no triangles. Hold E to dig into them.\n");
    fmt::print("Debug: 0=final 1=albedo 2=normals 3=AO 4=shadow 6=material | H/O/X toggle shadow/AO/reflections.\n");

    // The MicroVoxel tunables live on the concrete RHI renderer (also editable
    // in its ImGui panel); hotkeys poke them directly when available.
    auto* rhiRenderer = dynamic_cast<Renderer*>(renderer.get());

    auto& inputManager = engineCore->getInputManager();
    bool quit = false;
    float time = SDL_GetTicks() / 1000.0f;

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
                    if (rhiRenderer && !e.key.repeat) {
                        auto& mv = rhiRenderer->getMicroVoxelSettings();
                        auto setDebug = [&](int mode, const char* name) {
                            mv.params.y = static_cast<float>(mode);
                            fmt::print("MicroVoxel debug view: {}\n", name);
                        };
                        switch (e.key.scancode) {
                            case SDL_SCANCODE_0: setDebug(0, "final shading"); break;
                            case SDL_SCANCODE_1: setDebug(1, "albedo"); break;
                            case SDL_SCANCODE_2: setDebug(2, "normals"); break;
                            case SDL_SCANCODE_3: setDebug(3, "ambient occlusion"); break;
                            case SDL_SCANCODE_4: setDebug(4, "sun shadow"); break;
                            case SDL_SCANCODE_5: setDebug(5, "GI buffer"); break;
                            case SDL_SCANCODE_6: setDebug(6, "material index"); break;
                            case SDL_SCANCODE_H: {
                                float& sh = mv.sunDirection.w;
                                sh = (sh > 0.5f) ? 0.0f : 1.0f;
                                fmt::print("MicroVoxel sun shadow: {}\n", sh > 0.5f ? "on" : "off");
                                break;
                            }
                            case SDL_SCANCODE_O: {
                                float& ao = mv.params.x;
                                ao = (ao > 0.0f) ? 0.0f : 0.7f;
                                fmt::print("MicroVoxel AO: {}\n", ao > 0.0f ? "on" : "off");
                                break;
                            }
                            case SDL_SCANCODE_X: {
                                float& rf = mv.params.z;
                                rf = (rf > 0.5f) ? 0.0f : 1.0f;
                                fmt::print("MicroVoxel reflections: {}\n", rf > 0.5f ? "on" : "off");
                                break;
                            }
                            default:
                                break;
                        }
                    }
                    break;
                }
                case SDL_EVENT_WINDOW_RESIZED:
                    windowWidth = e.window.data1;
                    windowHeight = e.window.data2;
                    break;
                default:
                    break;
            }
        }

        // ---- Fly camera ----------------------------------------------------
        const auto& input = inputManager.getInputState();
        {
            glm::vec2 look = input.getVector(Vapor::InputAction::LookLeft, Vapor::InputAction::LookRight,
                                             Vapor::InputAction::LookDown, Vapor::InputAction::LookUp);
            glm::vec2 move = input.getVector(Vapor::InputAction::StrafeLeft, Vapor::InputAction::StrafeRight,
                                             Vapor::InputAction::MoveBackward, Vapor::InputAction::MoveForward);
            float vertical = input.getAxis(Vapor::InputAction::MoveDown, Vapor::InputAction::MoveUp);
            const float lookSpeed = 90.0f;  // deg/s
            float moveSpeed = input.isHeld(Vapor::InputAction::Sprint) ? 18.0f : 6.0f;

            camera.yaw -= look.x * lookSpeed * deltaTime;
            camera.pitch += look.y * lookSpeed * deltaTime;
            camera.pitch = glm::clamp(camera.pitch, -89.0f, 89.0f);

            const glm::quat rot = camera.rotation();
            camera.position += (rot * glm::vec3(1, 0, 0)) * move.x * moveSpeed * deltaTime;
            camera.position += (rot * glm::vec3(0, 0, -1)) * move.y * moveSpeed * deltaTime;
            camera.position += glm::vec3(0, 1, 0) * vertical * moveSpeed * deltaTime;
        }

        // Hold E to dig: carve a sphere of air at the first solid voxel along
        // the camera ray. No remeshing — the pass re-uploads only the touched
        // bricks, which is the whole point of the raymarch model.
        if (input.isHeld(Vapor::InputAction::Interact)) {
            Vapor::VoxelVolumeSystem::dig(registry, camera.position, camera.forward(),
                                          /*maxDist=*/60.0f, /*radius=*/0.45f);
        }

        // ---- Engine + ECS systems ------------------------------------------
        engineCore->update(deltaTime);
        Vapor::TransformSystem::update(registry);
        Vapor::LightGatherSystem::update(registry, scene.get());
        Vapor::SkySystem::update(registry, renderer.get());
        Vapor::VoxelVolumeSystem::update(registry, renderer.get());

        // ---- Render --------------------------------------------------------
        Camera tempCamera;
        const glm::quat rot = camera.rotation();
        const glm::mat4 view = glm::inverse(glm::translate(glm::mat4(1.0f), camera.position) * glm::mat4_cast(rot));
        // perspectiveZO: the RHI's clip-space depth is [0,1] on both backends —
        // the MicroVoxel pass derives its written depth from this matrix.
        const float aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
        const glm::mat4 proj = glm::perspectiveZO(camera.fov, aspect, camera.nearPlane, camera.farPlane);
        tempCamera.setEye(camera.position);
        tempCamera.setViewMatrix(view);
        tempCamera.setProjectionMatrix(proj);

        CameraRenderData camData;
        camData.proj = proj;
        camData.view = view;
        camData.invProj = glm::inverse(proj);
        camData.invView = glm::inverse(view);
        camData.nearPlane = camera.nearPlane;
        camData.farPlane = camera.farPlane;
        camData.position = camera.position;

        renderer->beginFrame(camData);
        ImGui::NewFrame();
        renderer->invokeImGuiCallback();
        renderer->draw(registry, scene, tempCamera);
        ImGui::Render();
        renderer->endFrame();
    }

    engineCore->shutdown();
    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
