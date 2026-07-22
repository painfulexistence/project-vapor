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
#include "Vapor/render_scene.hpp"
#include "Vapor/scene_blueprint.hpp"
#include "Vapor/systems.hpp"
#include "Vapor/voxel_world.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstring>
#include <entt/entt.hpp>
#include <fmt/core.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"

namespace {

// ECS fly-camera driver — the demo-local twin of Vaporware's CameraSystem
// (game systems live in the app layer; the engine provides the components).
// Same VirtualCamera/FlyCamera component semantics and integration math; the
// one deliberate deviation is perspectiveZO, because the RHI's clip depth is
// [0,1] on both backends and the MicroVoxel pass derives its written depth
// from this projection.
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
            float moveSpeed = fly.moveSpeed * (input.isHeld(Vapor::InputAction::Sprint) ? 3.0f : 1.0f);

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

// ── Quad flora (Minecraft-style crossed billboards) ──────────────────────────
// Raster vegetation over the raymarched voxels: the MicroVoxel pass writes true
// hit depth, so ordinary alpha-cutout quads depth-composite with the terrain in
// both directions for free. Each plant is two quads crossed at 90°, duplicated
// with reversed winding so back faces survive back-face culling; normals point
// +Y so blades light like the ground they grow from (no dark backside).

std::shared_ptr<Vapor::Image> makeGrassTuftImage() {
    constexpr int N = 48;
    auto img = std::make_shared<Vapor::Image>();
    img->uri = "microvoxel_flora_grass";  // unique key for the renderer's texture cache
    img->width = N;
    img->height = N;
    img->channelCount = 4;
    img->byteArray.assign(static_cast<size_t>(N) * N * 4, 0);
    auto put = [&](int x, int y, glm::vec3 c) {
        if (x < 0 || x >= N || y < 0 || y >= N) return;
        // The quad UVs put v=0 at the blade roots and v=0 samples byte row 0,
        // so plant-space y maps straight to the image row (row 0 = root).
        const size_t i = (static_cast<size_t>(y) * N + x) * 4;
        img->byteArray[i + 0] = static_cast<Uint8>(c.r * 255.0f);
        img->byteArray[i + 1] = static_cast<Uint8>(c.g * 255.0f);
        img->byteArray[i + 2] = static_cast<Uint8>(c.b * 255.0f);
        img->byteArray[i + 3] = 255;
    };
    Uint32 rng = 0xC0FFEEu;
    auto rand01 = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    };
    const glm::vec3 root(0.13f, 0.30f, 0.08f), tip(0.42f, 0.72f, 0.22f);
    for (int s = 0; s < 14; s++) {
        const float bx = 3.0f + rand01() * (N - 6);
        const float height = N * (0.55f + 0.4f * rand01());
        const float lean = (rand01() - 0.5f) * 10.0f;  // px of tip drift
        for (int y = 0; y < static_cast<int>(height); y++) {
            const float t = y / height;
            const int x = static_cast<int>(bx + lean * t * t);
            const glm::vec3 c = glm::mix(root, tip, t);
            put(x, y, c);
            if (t < 0.6f) put(x + 1, y, c);  // blades taper toward the tip
        }
    }
    return img;
}

std::shared_ptr<Vapor::Image> makeFlowerImage(const char* uri, glm::vec3 petal, glm::vec3 center) {
    constexpr int N = 48;
    auto img = std::make_shared<Vapor::Image>();
    img->uri = uri;
    img->width = N;
    img->height = N;
    img->channelCount = 4;
    img->byteArray.assign(static_cast<size_t>(N) * N * 4, 0);
    auto put = [&](int x, int y, glm::vec3 c) {
        if (x < 0 || x >= N || y < 0 || y >= N) return;
        const size_t i = (static_cast<size_t>(y) * N + x) * 4;  // row 0 = root (see grass)
        img->byteArray[i + 0] = static_cast<Uint8>(c.r * 255.0f);
        img->byteArray[i + 1] = static_cast<Uint8>(c.g * 255.0f);
        img->byteArray[i + 2] = static_cast<Uint8>(c.b * 255.0f);
        img->byteArray[i + 3] = 255;
    };
    const glm::vec3 stem(0.16f, 0.38f, 0.10f);
    const int cx = N / 2;
    for (int y = 0; y < N * 58 / 100; y++) {  // stem with a slight S-curve
        const int x = cx + static_cast<int>(1.5f * std::sin(y * 0.25f));
        put(x, y, stem);
        put(x + 1, y, stem * 1.15f);
    }
    put(cx - 4, N / 4, stem); put(cx - 3, N / 4, stem);       // leaf nubs
    put(cx + 5, N / 3, stem); put(cx + 4, N / 3, stem);
    const glm::vec2 head(cx, N * 0.72f);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const glm::vec2 d(x - head.x, y - head.y);
            const float r = glm::length(d);
            // Five-petal scallop: radius swells with cos(5*angle).
            const float petalR = 8.5f + 2.5f * std::cos(5.0f * std::atan2(d.y, d.x));
            if (r < 3.4f) put(x, y, center);
            else if (r < petalR) put(x, y, petal * (0.85f + 0.15f * (1.0f - r / petalR)));
        }
    }
    return img;
}

// Two quads crossed at 90°, each also emitted with reversed winding (the Main
// pass culls back faces; a reversed duplicate is cheaper than a per-material
// cull switch). Base at the local origin, `height` tall, `width` wide.
std::shared_ptr<Vapor::Mesh> buildCrossedQuads(float width, float height,
                                               std::shared_ptr<Vapor::Material> material) {
    auto mesh = std::make_shared<Vapor::Mesh>();
    std::vector<Vapor::VertexData> verts;
    std::vector<Uint32> inds;
    const float hw = width * 0.5f;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);   // foliage trick: light like the ground
    const glm::vec4 tan(1.0f, 0.0f, 0.0f, 1.0f);
    auto quad = [&](glm::vec3 a, glm::vec3 b, bool reversed) {
        const Uint32 base = static_cast<Uint32>(verts.size());
        verts.push_back({ { a.x, 0.0f, a.z }, { 0.0f, 0.0f }, up, tan });
        verts.push_back({ { b.x, 0.0f, b.z }, { 1.0f, 0.0f }, up, tan });
        verts.push_back({ { b.x, height, b.z }, { 1.0f, 1.0f }, up, tan });
        verts.push_back({ { a.x, height, a.z }, { 0.0f, 1.0f }, up, tan });
        if (reversed) inds.insert(inds.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
        else          inds.insert(inds.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    };
    quad({ -hw, 0, 0 }, { hw, 0, 0 }, false);
    quad({ -hw, 0, 0 }, { hw, 0, 0 }, true);
    quad({ 0, 0, -hw }, { 0, 0, hw }, false);
    quad({ 0, 0, -hw }, { 0, 0, hw }, true);
    mesh->initialize(verts, inds);
    mesh->material = std::move(material);
    return mesh;
}

// width/height are in VOXEL units (the mesh is built at 1 m per voxel and the
// spawner scales by the owning volume's voxelSize), so plants stay one voxel
// wide and at most ~two voxels tall at ANY volume resolution — the 2.5 cm
// centre diorama gets half-size flora automatically.
std::shared_ptr<Vapor::Mesh> makeFloraMesh(std::shared_ptr<Vapor::Image> albedo, const char* name,
                                           float width, float height) {
    auto mat = std::make_shared<Vapor::Material>();
    mat->name = name;
    mat->alphaMode = Vapor::AlphaMode::MASK;   // cutout: RHIMain/PrePass discard below the cutoff
    mat->alphaCutoff = 0.5f;
    mat->doubleSided = true;                   // belt & braces on backends that honor it
    mat->albedoMap = std::move(albedo);
    mat->roughnessFactor = 0.95f;
    mat->metallicFactor = 0.0f;
    return buildCrossedQuads(width, height, std::move(mat));
}

// Scatter flora on every generated volume's terrain surface. terrainHeight is a
// pure function of (x, z, seed) — chunks still generating don't matter — and
// the grass band mirrors the generator's snow/sand lines so flowers never grow
// on beaches or snowcaps. One-shot: returns false until every world exists.
bool spawnFlora(entt::registry& registry,
                const std::vector<std::shared_ptr<Vapor::Mesh>>& floraMeshes,
                std::vector<entt::entity>& outEntities) {
    auto view = registry.view<Vapor::VoxelVolumeComponent>();
    for (auto entity : view) {
        if (!view.get<Vapor::VoxelVolumeComponent>(entity).world.value) return false;
    }
    for (auto entity : view) {
        auto& vv = view.get<Vapor::VoxelVolumeComponent>(entity);
        const auto& world = *vv.world.value;
        const glm::vec3 origin = Vapor::VoxelVolumeSystem::volumeOrigin(registry, entity, world);
        const glm::ivec3 dim = world.dim();
        const float voxelSize = world.voxelSizeMeters();
        // The generator's biome lines (voxel_world.cpp generateColumnChunk).
        const float snowLine = (0.10f + 0.75f * 0.34f) * static_cast<float>(dim.y);
        const float sandLine = (0.10f + 0.12f * 0.34f) * static_cast<float>(dim.y);

        const float areaM2 = (dim.x * voxelSize) * (dim.z * voxelSize);
        // Voxel-scale plants need real density to read at all (~4 attempts/m²);
        // instancing + castShadow=false make the count nearly free, but cap the
        // spawned total per volume so a --big world stays well inside the
        // renderer's MAX_INSTANCES budget alongside everything else.
        const int attempts = static_cast<int>(areaM2 * 4.0f);
        const size_t maxPlants = 1500;
        Uint32 rng = vv.seed * 2654435761u + 77u;
        auto rand01 = [&rng] {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
        };
        // Roll every placement first (stable RNG stream), bucketed per mesh,
        // then create the entities MESH BY MESH: entities spawned grouped by
        // mesh collect into consecutive drawables -> consecutive instance
        // slots -> the renderer's per-object path collapses each group into a
        // handful of instanced draws instead of one draw per plant.
        struct Placement { glm::vec3 pos; float yaw; float scale; };
        std::array<std::vector<Placement>, 3> perMesh;
        for (int i = 0; i < attempts; i++) {
            const int gx = static_cast<int>(rand01() * (dim.x - 1));
            const int gz = static_cast<int>(rand01() * (dim.z - 1));
            const float kind = rand01();     // draw every random up-front (stable stream)
            const float yaw = rand01() * 6.2831853f;
            const float scale = 0.75f + 0.35f * rand01();
            const float h = world.terrainHeight(gx, gz);
            if (h <= sandLine + 0.5f || h >= snowLine - 0.5f) continue;  // grass band only
            const int top = static_cast<int>(h);
            if (top + 1 >= dim.y) continue;
            // 70% grass tufts, 30% flowers (alternating the two variants).
            if (perMesh[0].size() + perMesh[1].size() + perMesh[2].size() >= maxPlants) break;
            const size_t meshIdx = (kind < 0.7f) ? 0 : 1 + (static_cast<size_t>(kind * 100.0f) & 1);
            perMesh[meshIdx].push_back({
                origin + glm::vec3((gx + 0.5f) * voxelSize, (top + 1) * voxelSize,
                                   (gz + 0.5f) * voxelSize),
                yaw, scale });
        }
        for (size_t meshIdx = 0; meshIdx < perMesh.size(); meshIdx++) {
            for (const Placement& pl : perMesh[meshIdx]) {
                auto e = registry.create();
                auto& t = registry.emplace<Vapor::TransformComponent>(e);
                t.position = pl.pos;
                t.rotation = glm::angleAxis(pl.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                t.scale = glm::vec3(voxelSize * pl.scale);  // voxel units -> meters
                auto& mr = registry.emplace<Vapor::MeshRendererComponent>(e);
                mr.meshes.push_back(floraMeshes[meshIdx]);
                // Tiny cutout quads contribute almost nothing to the cascades
                // but each one is a full caster draw x every shadow pass — the
                // whole reason flora defaulted off. Skip the shadow passes.
                mr.castShadow = false;
                outEntities.push_back(e);
            }
        }
    }
    return true;
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

    // Declarative scene: the dioramas (voxelVolume components), sun, sky and
    // fly camera are all authored in the scene JSON — main.json is the three
    // side-by-side dioramas (the 512^3 @ 2.5 cm centre between two 5 cm ones),
    // big.json the single 51.2 x 12.8 x 51.2 m streaming world. The registry
    // is populated by instantiate(); VoxelVolumeSystem generates the worlds on
    // first sight, exactly as with code-spawned components.
    auto scene = std::make_shared<RenderScene>("microvoxel");
    entt::registry registry;
    {
        auto& resourceManager = engineCore->getResourceManager();
        auto sceneResource =
            resourceManager.loadScene(std::string(bigWorld ? "scenes/big.json" : "scenes/main.json"));
        auto sceneBlueprint = sceneResource->get();
        if (sceneBlueprint && sceneBlueprint->ok) {
            Vapor::instantiate(registry, *scene, *sceneBlueprint);
        } else {
            fmt::print(stderr, "MicroVoxel: scene blueprint failed to load; the world will be empty\n");
        }
    }
    // Quad flora meshes must be staged (mesh + material registration) before
    // stage(); the entities that draw them spawn lazily on the first P toggle,
    // once the voxel worlds exist and their surface heights can be queried.
    // Default OFF: each plant is its own entity/drawable (not yet instanced),
    // and the directional-shadow cascades re-record every drawable per pass —
    // hundreds of plants cost real CPU time until the flora path is instanced.
    // Dims in voxels: tufts one voxel, flowers a bit under two (the jitter
    // below caps at 1.1x, so nothing exceeds ~2 voxels tall).
    std::vector<std::shared_ptr<Vapor::Mesh>> floraMeshes = {
        makeFloraMesh(makeGrassTuftImage(), "flora_grass", 1.0f, 1.2f),
        makeFloraMesh(makeFlowerImage("microvoxel_flora_poppy",
                                      glm::vec3(0.85f, 0.18f, 0.12f), glm::vec3(0.12f, 0.09f, 0.02f)),
                      "flora_poppy", 1.0f, 1.8f),
        makeFloraMesh(makeFlowerImage("microvoxel_flora_daisy",
                                      glm::vec3(0.95f, 0.95f, 0.92f), glm::vec3(0.98f, 0.80f, 0.15f)),
                      "flora_daisy", 1.0f, 1.7f),
    };
    for (const auto& m : floraMeshes) scene->addMesh(m);
    bool floraEnabled = false;   // P toggles (see the comment above)
    bool floraSpawned = false;
    std::vector<entt::entity> floraEntities;

    renderer->stage(scene);
    scene->stagedMeshes.clear();
    scene->stagedMeshTransforms.clear();

    // Window-derived state the JSON can't know: the camera aspect.
    {
        auto camView = registry.view<Vapor::VirtualCameraComponent>();
        camView.each([&](auto& cam) {
            cam.aspect = (windowHeight > 0) ? float(windowWidth) / float(windowHeight) : 1.0f;
        });
    }

    fmt::print("MicroVoxel loaded. WASD move, R/F up/down, IJKL look, LShift sprint, Esc quit.\n");
    fmt::print("Raymarched 5 cm voxel volumes — no triangles. Hold E to dig into them.\n");
    fmt::print("Debug: 0=final 1=albedo 2=normals 3=AO 4=shadow 5=GI 6=material | "
               "G/O/H/X/N/V toggle GI/AO/shadow/reflections/denoiser/cross-volume | B = split raw|denoised | "
               "P = quad flora.\n");

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
                    if (!e.key.repeat && e.key.scancode == SDL_SCANCODE_P) {
                        floraEnabled = !floraEnabled;
                        for (auto fe : floraEntities) {
                            if (auto* mr = registry.try_get<Vapor::MeshRendererComponent>(fe))
                                mr->visible = floraEnabled;
                        }
                        fmt::print("Quad flora: {}\n", floraEnabled ? "on" : "off");
                    }
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
                            case SDL_SCANCODE_G: {
                                float& gi = rhiRenderer->getMicroVoxelGIStrength();
                                gi = (gi > 0.0f) ? 0.0f : 1.0f;
                                fmt::print("MicroVoxel GI: {}\n", gi > 0.0f ? "on" : "off (flat ambient)");
                                break;
                            }
                            case SDL_SCANCODE_N: {
                                int& its = rhiRenderer->getMicroVoxelGIAtrousIterations();
                                its = (its > 0) ? 0 : 3;
                                fmt::print("MicroVoxel GI denoiser: {}\n",
                                           its > 0 ? "on (a-trous)" : "off (raw temporal)");
                                break;
                            }
                            case SDL_SCANCODE_B: {
                                float& split = rhiRenderer->getMicroVoxelGISplitX();
                                split = (split >= 0.0f) ? -1.0f : 0.5f;
                                fmt::print("MicroVoxel GI split: {}\n",
                                           split >= 0.0f ? "on (left raw | right denoised)" : "off");
                                break;
                            }
                            case SDL_SCANCODE_V: {
                                bool& cv = rhiRenderer->getMicroVoxelGICrossVolume();
                                cv = !cv;
                                fmt::print("MicroVoxel GI: {}\n",
                                           cv ? "cross-volume (light bleeds between volumes)"
                                              : "primary volume only");
                                break;
                            }
                            default:
                                break;
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

        // ---- Gameplay systems ----------------------------------------------
        const auto& input = inputManager.getInputState();
        FlyCameraSystem::update(registry, input, deltaTime);

        // Hold E to dig: carve a sphere of air at the first solid voxel along
        // the camera ray. No remeshing — the pass re-uploads only the touched
        // bricks, which is the whole point of the raymarch model.
        entt::entity activeCam = getActiveCamera(registry);
        if (activeCam != entt::null && input.isHeld(Vapor::InputAction::Interact)) {
            const auto& cam = registry.get<Vapor::VirtualCameraComponent>(activeCam);
            Vapor::VoxelVolumeSystem::dig(registry, cam.position,
                                          cam.rotation * glm::vec3(0.0f, 0.0f, -1.0f),
                                          /*maxDist=*/60.0f, /*radius=*/0.45f);
        }

        // ---- Engine + ECS systems ------------------------------------------
        engineCore->update(deltaTime);
        Vapor::TransformSystem::update(registry);
        Vapor::LightGatherSystem::update(registry, scene.get());
        Vapor::SkySystem::update(registry, renderer.get());
        Vapor::VoxelVolumeSystem::update(registry, renderer.get());

        // Lazy one-shot flora scatter (P toggles; surface height is a pure
        // function, so chunk generation can still be streaming).
        if (floraEnabled && !floraSpawned) {
            floraSpawned = spawnFlora(registry, floraMeshes, floraEntities);
        }

        // ---- Render --------------------------------------------------------
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
