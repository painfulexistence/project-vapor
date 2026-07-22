// ============================================================================
// USDViewer — fly-camera viewer for USD scenes through the SceneBlueprint
// import line (AssetManager::loadModel -> instantiate).
//
// The Vapor port of Atmospheric's Examples/USDViewer. USD is a first-class
// import format here (TinyUSDZ behind VAPOR_USE_TINYUSDZ, ON by default):
// a model file decodes into a SceneBlueprint (node hierarchy = entities,
// meshes/materials/images = payload) and instantiate() turns it into entt
// entities that draw as GPU instances.
//
// Two demo assets:
//   - assets/models/cube.usda — tiny committed sample, always loaded.
//   - assets/models/kitchen/Kitchen_set.usd — Pixar's Kitchen_set, the real
//     composition stress test. Not committed (large); fetch it with
//     scripts/downloadUSDSamples.sh, and OnLoad imports it when present.
//     Kitchen_set is authored in centimetres but omits the metersPerUnit
//     stage metadatum, so the importer applies no unit scale — the root
//     entity is scaled to metres here, like the original demo.
//
// Controls: WASD move, R/F up/down, IJKL look, LShift sprint, Esc quit.
// (--vulkan / --metal pick the backend.)
// ============================================================================

#include "Vapor/asset_manager.hpp"
#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/file_system.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/irenderer.hpp"
#include "Vapor/render_scene.hpp"
#include "Vapor/scene_blueprint.hpp"
#include "Vapor/systems.hpp"

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

// ECS fly-camera driver — same demo-local system as Examples/MicroVoxel
// (game systems live in the app layer; the engine provides the components).
// perspectiveZO because the RHI's clip depth is [0,1] on both backends.
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
            float moveSpeed = fly.moveSpeed * (input.isHeld(Vapor::InputAction::Sprint) ? 4.0f : 1.0f);

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

// Import a model file into the world; returns the root entity (entt::null on
// failure) and logs what arrived.
entt::entity importModel(entt::registry& registry, RenderScene& scene, const std::string& path,
                         const std::string& name) {
    Vapor::SceneBlueprint bp = AssetManager::loadModel(path);
    if (!bp.ok) {
        fmt::print(stderr, "USDViewer: failed to import '{}' (is VAPOR_USE_TINYUSDZ on?)\n", path);
        return entt::null;
    }
    size_t verts = 0;
    for (const auto& mesh : bp.meshes)
        if (mesh) verts += mesh->vertices.size();
    fmt::print("{}: {} meshes, {} materials, {} verts\n", name, bp.meshes.size(), bp.materials.size(), verts);
    return Vapor::instantiate(registry, scene, bp, entt::null, name);
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
        winTitle = "USDViewer (Vulkan)";
        winFlags |= SDL_WINDOW_VULKAN;
        gfxBackend = GraphicsBackend::Vulkan;
    } else {
        winTitle = "USDViewer (Metal)";
        winFlags |= SDL_WINDOW_METAL;
        gfxBackend = GraphicsBackend::Metal;
    }
#else
    winTitle = "USDViewer (Vulkan)";
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

    auto scene = std::make_shared<RenderScene>("usdviewer");
    renderer->stage(scene);

    entt::registry registry;

    // ---- USD content -------------------------------------------------------
    // The committed sample cube is always present; Pixar's Kitchen_set loads
    // when the user has downloaded it (see the header comment).
    importModel(registry, *scene, "models/cube.usda", "cube");
    const std::string kitchen = "models/kitchen/Kitchen_set.usd";
    if (FileSystem::instance().resolvePath(kitchen)) {
        entt::entity root = importModel(registry, *scene, kitchen, "Kitchen_set");
        if (root != entt::null) {
            // Kitchen_set is centimetres with no metersPerUnit metadatum, so
            // the importer applies no unit scale; bring it to metres here.
            if (auto* t = registry.try_get<Vapor::TransformComponent>(root)) {
                t->scale = glm::vec3(0.01f);
                t->isDirty = true;
            }
        }
    } else {
        fmt::print("Kitchen_set not found — run scripts/downloadUSDSamples.sh to fetch it "
                   "(showing the sample cube instead)\n");
    }

    // ---- Environment -------------------------------------------------------
    {
        auto sun = registry.create();
        registry.emplace<Vapor::NameComponent>(sun, Vapor::NameComponent { "Sun" });
        auto& dl = registry.emplace<Vapor::DirectionalLightComponent>(sun);
        dl.direction = glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f));
        dl.color = glm::vec3(1.0f, 0.98f, 0.92f);
        dl.intensity = 8.0f;
        registry.emplace<Vapor::SunComponent>(sun);

        auto env = registry.create();
        registry.emplace<Vapor::NameComponent>(env, Vapor::NameComponent { "Environment" });
        registry.emplace<Vapor::SkyComponent>(env);
    }

    // Camera entity a few metres back from the origin, looking at the content.
    {
        auto camEntity = registry.create();
        registry.emplace<Vapor::NameComponent>(camEntity, Vapor::NameComponent { "Fly Camera" });
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(camEntity);
        cam.isActive = true;
        cam.aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
        cam.position = glm::vec3(3.0f, 2.5f, 5.0f);
        auto& fly = registry.emplace<Vapor::FlyCameraComponent>(camEntity);
        fly.moveSpeed = 4.0f;
        fly.rotateSpeed = 90.0f;
        fly.yaw = 120.0f;   // look back toward the origin
        fly.pitch = 12.0f;  // slightly down
    }

    // Everything imported above lands in the staging list; register it once.
    renderer->stage(scene);
    scene->stagedMeshes.clear();
    scene->stagedMeshTransforms.clear();

    fmt::print("USDViewer loaded. WASD move, R/F up/down, IJKL look, LShift sprint, Esc quit.\n");

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
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
                    break;
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

        engineCore->update(deltaTime);
        Vapor::TransformSystem::update(registry);
        Vapor::LightGatherSystem::update(registry, scene.get());
        Vapor::SkySystem::update(registry, renderer.get());

        entt::entity activeCam = getActiveCamera(registry);
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

    engineCore->shutdown();
    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
