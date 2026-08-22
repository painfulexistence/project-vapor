// ============================================================================
// Poolrooms — a liminal "pool core" walking demo and the showcase for the RHI
// water stack (FFT spectral surface + planar reflections + refraction +
// projected caustics).
//
// The environment is DATA: assets/scenes/poolrooms.json declares the textures
// (baked by Vapor::proctex at load), the materials that reference them, the
// sun, the sky and the haze — and one entity naming the "poolroom" composite
// mesh generator (poolroom_generator.hpp), which returns the tiled hall as
// per-material geometry buckets plus the static colliders, the water surface,
// the porthole lamps and the spawn / ladder / skylight markers.
//
// What is left in C++ here is what is not scene data: the character
// controller, the camera, input, the swim/climb rules and the debug panel.
//
// Drop real tile photos into Res/textures/poolrooms/ (see
// assets/textures/poolrooms/README.md) and they replace the placeholders slot
// by slot — geometry and UVs are authored so a single-tile texture maps one
// tile per cell. Editing the "@name" in the scene file to a path does the
// same thing.
//
// The player is a Jolt CharacterVirtual (ECS CharacterBodyComponent): walk the
// deck, jump, wade in — below the waterline movement switches to a swim; get
// out via the mosaic stairs or by holding W against a handrail entry.
//
// Two moods: the sunlit pool, and `--flooded` (or the panel checkbox) — the
// whole hall waist-deep in murky warm-green water under a dim ember sun, lamp
// glows smearing across smooth viscous swells.
//
// Controls: WASD move, mouse look (Tab toggles capture), Space jump / swim up,
// LCtrl swim down, LShift sprint, Esc quit. --vulkan / --metal pick the
// backend (water requires the RHI renderer, the default on both).
// ============================================================================

#include "Vapor/asset_manager.hpp"
#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/file_system.hpp"
#include "Vapor/graphics_effects.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/irenderer.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/render_scene.hpp"
#include "Vapor/systems.hpp"

#include <SDL3/SDL.h>
#include <cstring>
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <utility>  // std::pair — the ladder-zone list
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"

#include "Vapor/scene_blueprint.hpp"
#include "poolroom_generator.hpp"
#include "poolroom_scene.hpp"

namespace {

// Entities the scene file (and the poolroom generator) place by name. The app
// looks them up rather than holding handles, because it did not create them.
entt::entity findByName(entt::registry& reg, const std::string& name) {
    for (auto e : reg.view<Vapor::NameComponent>())
        if (reg.get<Vapor::NameComponent>(e).name == name) return e;
    return entt::null;
}

// ── User texture overrides ──────────────────────────────────────────────────
// The scene file names procedural placeholders. If the user has dropped a real
// photograph at Res/textures/poolrooms/<slot>_<kind>.png, swap it in after the
// blueprint loads — the alternative is editing the JSON, which is also
// supported (a slot takes a path just as happily as an "@name").
void applyUserTextureOverrides(Vapor::SceneBlueprint& blueprint) {
    // Scene-material name -> the slot prefix the README documents.
    static const std::pair<const char*, const char*> kSlots[] = {
        { "deckTile", "deck_tile" },           { "wallTile", "wall_tile" },
        { "poolWallTile", "pool_wall_tile" },  { "poolFloorTile", "pool_floor_tile" },
        { "accentTile", "accent_tile" },       { "groutMat", "grout" },
        { "copingMat", "coping" },             { "ceilingPlaster", "ceiling_plaster" },
        { "darkPlaster", "dark_plaster" },     { "metalMat", "metal" },
        { "lampGlow", "lamp_glow" },           { "slideBlue", "slide_blue" },
        { "slideViolet", "slide_violet" },
    };
    for (auto& material : blueprint.materials) {
        if (!material) continue;
        const char* slot = nullptr;
        for (const auto& [matName, slotName] : kSlots)
            if (material->name == matName) slot = slotName;
        if (!slot) continue;

        auto swap = [&](std::shared_ptr<Vapor::Image>& target, const char* kind) {
            const std::string rel = fmt::format("textures/poolrooms/{}_{}.png", slot, kind);
            if (!FileSystem::instance().resolvePath(rel)) return;
            if (auto img = AssetManager::loadImage(rel)) {
                fmt::print("Poolrooms: using user texture '{}'\n", rel);
                target = std::move(img);
            }
        };
        swap(material->albedoMap, "albedo");
        swap(material->normalMap, "normal");
        swap(material->roughnessMap, "roughness");
    }
}


// ── Active water volume ─────────────────────────────────────────────────────
// Which body of water the swim logic answers to. Normal mode: the sunken
// pool. Flooded mode: the whole hall at waist height.
struct WaterVolume {
    glm::vec2 minXZ;
    glm::vec2 maxXZ;
    float level = 0.0f;
};

// ── First-person controller (mouse look + walk/swim/climb intent) ───────────

struct PlayerState {
    entt::entity entity = entt::null;
    float yaw = 0.0f;    // degrees, 0 = looking -Z
    float pitch = 0.0f;  // degrees
    bool mouseCaptured = true;
    bool swimming = false;
    bool climbing = false;
    float eyeHeight = 0.72f;  // above the capsule center (~1.6 m above feet)
};

glm::vec2 mouseDeltaThisFrame;  // accumulated from SDL relative motion events

void updatePlayer(entt::registry& reg, PlayerState& player, const Vapor::InputState& input,
                  const std::vector<std::pair<glm::vec3, glm::vec3>>& ladderZones,
                  const WaterVolume& water, float deltaTime) {
    (void)deltaTime;  // look/motion are velocity-based; the physics step integrates
    auto* body = reg.try_get<Vapor::CharacterBodyComponent>(player.entity);
    if (!body) return;

    // Mouse look.
    const float lookSpeed = 0.11f;  // degrees per pixel
    player.yaw -= mouseDeltaThisFrame.x * lookSpeed;
    player.pitch -= mouseDeltaThisFrame.y * lookSpeed;
    player.pitch = glm::clamp(player.pitch, -89.0f, 89.0f);

    const float yawRad = glm::radians(player.yaw);
    const glm::vec3 forward(-std::sin(yawRad), 0.0f, -std::cos(yawRad));
    const glm::vec3 right(-forward.z, 0.0f, forward.x);

    glm::vec2 move = input.getVector(Vapor::InputAction::StrafeLeft, Vapor::InputAction::StrafeRight,
                                     Vapor::InputAction::MoveBackward, Vapor::InputAction::MoveForward);
    if (glm::dot(move, move) > 1.0f) move = glm::normalize(move);
    const glm::vec3 moveDir = forward * move.y + right * move.x;

    const glm::vec3 center = body->controller ? body->controller->getPosition()
                                              : reg.get<Vapor::TransformComponent>(player.entity).position;

    // Water state: capsule center below the waterline (with a little slack so
    // wading stays walking).
    player.swimming = center.y < water.level + 0.12f &&
                      center.x > water.minXZ.x && center.x < water.maxXZ.x &&
                      center.z > water.minXZ.y && center.z < water.maxXZ.y;

    // Ladder: inside a climb zone and pushing forward.
    player.climbing = false;
    if (move.y > 0.1f) {
        for (const auto& z : ladderZones) {
            if (center.x > z.first.x && center.x < z.second.x &&
                center.y > z.first.y - 0.5f && center.y < z.second.y &&
                center.z > z.first.z && center.z < z.second.z) {
                player.climbing = true;
                break;
            }
        }
    }

    if (!body->controller) {
        body->desiredVelocity = moveDir;
        return;
    }

    if (player.climbing) {
        // Steady rise; jump() detaches from the floor, then the per-frame
        // velocity keeps the climb going (gravity is re-added each fixed step
        // and cancelled here every frame).
        body->desiredVelocity = moveDir * 0.4f;
        if (body->controller->isOnGround()) {
            body->controller->jump(1.8f);
        } else {
            glm::vec3 v = body->controller->getVelocity();
            body->controller->setLinearVelocity(glm::vec3(v.x, 2.0f, v.z));
        }
        body->controller->setMaxSpeed(1.2f);
    } else if (player.swimming) {
        body->controller->setMaxSpeed(1.9f);
        body->desiredVelocity = moveDir;
        float targetVy = -0.18f;  // gentle sink when idle
        if (input.isHeld(Vapor::InputAction::Jump)) targetVy = 1.6f;
        if (input.isHeld(Vapor::InputAction::Crouch)) targetVy = -1.6f;
        // Add a bit of look-directed glide when moving forward.
        targetVy += move.y * -std::sin(glm::radians(player.pitch)) * 1.2f;
        if (body->controller->isOnGround()) {
            if (targetVy > 0.2f) body->controller->jump(glm::min(targetVy, 1.6f));
        } else {
            glm::vec3 v = body->controller->getVelocity();
            body->controller->setLinearVelocity(glm::vec3(v.x, glm::mix(v.y, targetVy, 0.35f), v.z));
        }
    } else {
        const bool sprint = input.isHeld(Vapor::InputAction::Sprint);
        body->controller->setMaxSpeed(sprint ? 5.0f : 3.1f);
        body->desiredVelocity = moveDir;
        if (input.isPressed(Vapor::InputAction::Jump) && body->controller->isOnGround()) {
            body->jumpRequested = true;
        }
    }
}

}  // namespace

auto main(int argc, char* args[]) -> int {
    bool wantVulkan = false, wantMetal = false, startFlooded = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(args[i], "--vulkan") == 0) wantVulkan = true;
        if (std::strcmp(args[i], "--metal") == 0) wantMetal = true;
        if (std::strcmp(args[i], "--flooded") == 0) startFlooded = true;
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
        winTitle = "Poolrooms (Vulkan)";
        winFlags |= SDL_WINDOW_VULKAN;
        gfxBackend = GraphicsBackend::Vulkan;
    } else {
        winTitle = "Poolrooms (Metal)";
        winFlags |= SDL_WINDOW_METAL;
        gfxBackend = GraphicsBackend::Metal;
    }
#else
    winTitle = "Poolrooms (Vulkan)";
    winFlags |= SDL_WINDOW_VULKAN;
    gfxBackend = GraphicsBackend::Vulkan;
#endif

    auto window = SDL_CreateWindow(winTitle, 1600, 900, winFlags);
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

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler(), renderer->getDebugDraw());
    physics->setDebugEnabled(false);

    auto scene = std::make_shared<RenderScene>("poolrooms");
    entt::registry registry;
    physics->attach(registry);

    // ---- Load the scene --------------------------------------------------
    // Everything the environment is — geometry, materials, textures, water,
    // lights, fog — lives in scenes/poolrooms.json. The hall itself is one
    // entity naming the "poolroom" composite generator, which also emits the
    // static colliders, the water surface, the porthole lamps and the spawn /
    // ladder / skylight markers as children.
    fmt::print("Poolrooms: loading scene...\n");
    poolgen::registerPoolroomGenerator();  // before the load: the scene names it

    Vapor::SceneBlueprint blueprint = Vapor::loadSceneBlueprint("scenes/poolrooms.json");
    if (!blueprint.ok) {
        fmt::print(stderr, "Poolrooms: failed to load scenes/poolrooms.json\n");
        return 1;
    }
    applyUserTextureOverrides(blueprint);
    // The mood is a generator parameter, so --flooded has to reach the
    // blueprint before instantiate runs the generator.
    for (auto& e : blueprint.entities) {
        if (e.procMesh.generator != "poolroom") continue;
        auto params = nlohmann::json::parse(e.procMesh.paramsJson, nullptr, false);
        if (!params.is_object()) params = nlohmann::json::object();
        params["flooded"] = startFlooded;
        e.procMesh.paramsJson = params.dump();
    }
    Vapor::instantiate(registry, *scene, blueprint);

    // Upload the geometry pool + register materials/textures, then drop the
    // staging lists so later frames don't re-upload (GLTFViewer pattern).
    renderer->stage(scene);
    scene->stagedMeshes.clear();
    scene->stagedMeshTransforms.clear();

    // Realize the colliders the generator emitted. They arrive as data-only
    // {Rigidbody, BoxCollider, Transform} children; this is what turns them
    // into Jolt bodies. TransformSystem first, so the parented ones know where
    // they are in the world.
    Vapor::TransformSystem::update(registry);
    Vapor::PhysicsBodySystem::update(registry, physics.get());

    // Constants the gameplay code needs (spawn, pool bounds, water level).
    // These are plain member initializers on PoolroomScene — no geometry is
    // rebuilt to read them, and they are the same numbers the generator used.
    const poolgen::PoolroomScene world{};

    // Climb volumes, read back from the markers the generator placed. Their
    // half-extents ride in the transform scale.
    std::vector<std::pair<glm::vec3, glm::vec3>> ladderZones;
    for (auto e : registry.view<Vapor::NameComponent, Vapor::TransformComponent>()) {
        const std::string& n = registry.get<Vapor::NameComponent>(e).name;
        if (n.rfind("LadderZone", 0) != 0) continue;
        const auto& t = registry.get<Vapor::TransformComponent>(e);
        ladderZones.emplace_back(t.position - t.scale, t.position + t.scale);
    }

    const entt::entity waterEntity = findByName(registry, "PoolWater");
    if (waterEntity == entt::null) {
        fmt::print(stderr, "Poolrooms: the scene has no PoolWater entity\n");
        return 1;
    }

    // ---- Environment -----------------------------------------------------
    // Sun, sky and the humid haze are entities in the scene file; the porthole
    // lamps are children the generator placed. The app only needs handles to
    // the three the mood switch retunes.
    const entt::entity sunEntity = findByName(registry, "Sun");
    const entt::entity envEntity = findByName(registry, "Environment");
    const entt::entity fogEntity = findByName(registry, "Pool Haze");
    if (sunEntity == entt::null || envEntity == entt::null || fogEntity == entt::null) {
        fmt::print(stderr, "Poolrooms: the scene is missing Sun / Environment / Pool Haze\n");
        return 1;
    }

    // ---- Water (two moods) ----------------------------------------------
    // Normal: the sunlit pool. Flooded: the whole hall knee-deep in murky
    // warm-green water — smooth viscous swells, lamp glows smearing across
    // the surface, dim ember sun. Switchable from the panel (or --flooded).
    WaterVolume waterVol;
    bool floodedMode = startFlooded;
    float sunAzimuthDeg = 52.0f;
    float sunElevationDeg = 66.0f;

    auto applyWaterMode = [&](bool flooded) {
        floodedMode = flooded;
        const float level = flooded ? poolgen::kFloodLevel : world.waterLevel;

        // The surface itself: re-apply the generator's own water blob for this
        // mood through the ordinary applier, so the app and the scene file
        // cannot drift apart on what "flooded" means. WaterSystem pushes it to
        // the renderer on the next frame.
        const auto blob = nlohmann::json::parse(poolgen::waterComponentJson(flooded, world),
                                                nullptr, /*allow_exceptions=*/false);
        if (blob.is_object() && blob.contains("water"))
            Vapor::BlueprintComponents::instance().apply("water", registry, waterEntity,
                                                         blob.at("water"));
        registry.get<Vapor::TransformComponent>(waterEntity).position.y = level;
        registry.get<Vapor::TransformComponent>(waterEntity).isDirty = true;

        // Which body of water the swim logic answers to.
        waterVol = flooded ? WaterVolume{ glm::vec2(-12.0f, -5.0f), glm::vec2(12.0f, 5.0f), level }
                           : WaterVolume{ world.poolMinXZ, world.poolMaxXZ, level };

        auto& dl = registry.get<Vapor::DirectionalLightComponent>(sunEntity);
        auto& fv = registry.get<Vapor::VolumetricFogComponent>(fogEntity);
        if (!flooded) {
            // Steep late-morning sun: the shafts from the skylight wells land
            // on the water and the north deck.
            sunElevationDeg = 66.0f;
            dl.direction = glm::normalize(glm::vec3(0.28f, -1.0f, 0.22f));
            dl.color = glm::vec3(1.0f, 0.97f, 0.90f);
            dl.intensity = 8.0f;

            fv.density = 0.016f;
            fv.albedo = glm::vec3(0.92f, 0.96f, 1.0f);
            fv.ambientIntensity = 0.35f;
        } else {
            // Ember dusk through the wells; the lamps carry the room.
            sunElevationDeg = 34.0f;
            const float el = glm::radians(sunElevationDeg), az = glm::radians(sunAzimuthDeg);
            dl.direction = -glm::normalize(glm::vec3(std::cos(el) * std::sin(az), std::sin(el),
                                                     std::cos(el) * std::cos(az)));
            dl.color = glm::vec3(1.0f, 0.68f, 0.42f);
            dl.intensity = 1.1f;

            fv.density = 0.034f;
            fv.albedo = glm::vec3(1.0f, 0.86f, 0.68f);
            fv.ambientIntensity = 0.5f;
        }
        // The sun moved: force the sky/IBL rebake so ambient follows the mood.
        registry.get<Vapor::SkyComponent>(envEntity).dirty = true;
    };
    applyWaterMode(startFlooded);

    // ---- Player --------------------------------------------------------
    PlayerState player;
    {
        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{ "Player" });
        auto& t = registry.emplace<Vapor::TransformComponent>(e);
        t.position = world.spawnPosition;
        auto& body = registry.emplace<Vapor::CharacterBodyComponent>(e);
        body.settings.height = 1.75f;
        body.settings.radius = 0.32f;
        body.settings.maxSlopeAngle = 46.0f;

        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(e);
        cam.isActive = true;
        cam.fov = glm::radians(72.0f);
        cam.aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
        cam.near = 0.05f;
        cam.far = 200.0f;

        player.entity = e;
        // Face the pool from the spawn on the north-west deck.
        player.yaw = world.spawnYawDeg;
        player.pitch = -8.0f;
    }
    SDL_SetWindowRelativeMouseMode(window, true);

    // ---- UI -------------------------------------------------------------
    bool pendingFlooded = floodedMode;  // applied at the top of the frame
    renderer->setImGuiCallback([&] {
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Poolrooms");
        ImGui::TextWrapped("WASD move, mouse look (Tab release), Space jump/swim up, "
                           "LCtrl swim down, LShift sprint, Esc quit.");
        ImGui::Checkbox("Flooded hall (liminal dusk)", &pendingFlooded);
        if (auto* body = registry.try_get<Vapor::CharacterBodyComponent>(player.entity);
            body && body->controller) {
            const glm::vec3 p = body->controller->getPosition();
            ImGui::Text("pos (%.1f  %.1f  %.1f)  %s", p.x, p.y, p.z,
                        player.climbing ? "climbing" : (player.swimming ? "swimming" : "walking"));
        }
        ImGui::SeparatorText("Sun");
        bool sunChanged = false;
        sunChanged |= ImGui::SliderFloat("Elevation", &sunElevationDeg, 5.0f, 89.0f, "%.0f deg");
        sunChanged |= ImGui::SliderFloat("Azimuth", &sunAzimuthDeg, -180.0f, 180.0f, "%.0f deg");
        if (sunChanged && sunEntity != entt::null) {
            auto& dl = registry.get<Vapor::DirectionalLightComponent>(sunEntity);
            const float el = glm::radians(sunElevationDeg), az = glm::radians(sunAzimuthDeg);
            dl.direction = -glm::normalize(glm::vec3(std::cos(el) * std::sin(az), std::sin(el),
                                                     std::cos(el) * std::cos(az)));
        }
        ImGui::SeparatorText("Teleport");
        auto teleport = [&](const glm::vec3& pos) {
            if (auto* body = registry.try_get<Vapor::CharacterBodyComponent>(player.entity);
                body && body->controller) {
                body->controller->warp(pos);
            }
        };
        if (ImGui::Button("Deck")) teleport(world.spawnPosition);
        ImGui::SameLine();
        if (ImGui::Button("Pool floor")) teleport(glm::vec3(0.0f, world.poolFloorY + 1.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::Button("Corridor")) teleport(glm::vec3(-13.6f, 0.9f, 0.0f));
        ImGui::TextDisabled("water tuning: engine panel -> Water Settings");
        ImGui::End();
    });

    fmt::print("Poolrooms loaded. WASD + mouse look, Tab toggles the cursor, Esc quits.\n");

    auto& inputManager = engineCore->getInputManager();
    bool quit = false;
    float time = SDL_GetTicks() / 1000.0f;

    while (!quit) {
        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = glm::min(currTime - time, 0.1f);
        time = currTime;

        inputManager.update(deltaTime);
        mouseDeltaThisFrame = glm::vec2(0.0f);

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
                    if (e.key.scancode == SDL_SCANCODE_TAB) {
                        player.mouseCaptured = !player.mouseCaptured;
                        SDL_SetWindowRelativeMouseMode(window, player.mouseCaptured);
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (player.mouseCaptured) {
                        mouseDeltaThisFrame.x += e.motion.xrel;
                        mouseDeltaThisFrame.y += e.motion.yrel;
                    }
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

        // Apply a panel-requested mode switch before the frame opens (not from
        // inside the ImGui callback, which runs mid-frame).
        if (pendingFlooded != floodedMode) applyWaterMode(pendingFlooded);

        const auto& input = inputManager.getInputState();
        updatePlayer(registry, player, input, ladderZones, waterVol, deltaTime);

        engineCore->update(deltaTime);
        physics->process(registry, deltaTime);
        Vapor::TransformSystem::update(registry);
        Vapor::LightGatherSystem::update(registry, scene.get());
        Vapor::SkySystem::update(registry, renderer.get());
        // Drives the FFT surface from the scene's WaterComponent. After
        // TransformSystem, so a moved surface takes its world position with it.
        Vapor::WaterSystem::update(registry, renderer.get());
        // NOTE: the "Pool Haze" VolumetricFogComponent is authored but never
        // reaches the renderer — this demo has never run VolumetricFogSystem,
        // so the fog has always been inert. Adding the call here would change
        // how the room looks, which is a separate decision from this refactor.

        // First-person camera from the interpolated character position.
        auto& cam = registry.get<Vapor::VirtualCameraComponent>(player.entity);
        {
            glm::vec3 center = world.spawnPosition;
            if (auto* body = registry.try_get<Vapor::CharacterBodyComponent>(player.entity);
                body && body->controller) {
                center = body->controller->getInterpolatedPosition(physics->getInterpolationAlpha());
            }
            cam.position = center + glm::vec3(0.0f, player.eyeHeight, 0.0f);
            cam.rotation = glm::quat(glm::vec3(glm::radians(player.pitch), glm::radians(player.yaw), 0.0f));
            glm::mat4 rotation = glm::mat4_cast(cam.rotation);
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
            cam.viewMatrix = glm::inverse(translation * rotation);
            cam.projectionMatrix = glm::perspectiveZO(cam.fov, cam.aspect, cam.near, cam.far);
        }

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

    physics->deinit();
    engineCore->shutdown();
    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
