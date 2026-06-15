#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <args.hxx>
#include <fmt/core.h>
#include <iostream>

#include "Vapor/camera.hpp"
#include "Vapor/components.hpp"
#include "Vapor/engine_core.hpp"
#include "Vapor/input_manager.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/renderer.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/scene.hpp"
#include "Vapor/systems.hpp"
#include <entt/entt.hpp>

#include "components.hpp"
#include "scene_builder.hpp"
#include "systems.hpp"

// ============================================================================
// Whiteout Survival ad-game — entry point
//
// The minigame from the ads, on its own: torch out into the snow, kill bears,
// grab meat and resources, rescue survivors, spend it all on better gear.
// Pure ECS on the Vapor engine — see survival/systems.hpp for the frame order.
// ============================================================================

namespace {

    entt::entity activeCamera(entt::registry& reg) {
        auto view = reg.view<Vapor::VirtualCameraComponent>();
        for (auto e : view)
            if (view.get<Vapor::VirtualCameraComponent>(e).isActive) return e;
        return entt::null;
    }

    void drawHUD(entt::registry& reg) {
        using namespace Survival;

        entt::entity player = findPlayer(reg);
        const InventoryComponent*  inv    = nullptr;
        const HealthComponent*     health = nullptr;
        const ExperienceComponent* xp     = nullptr;
        const CombatStatsComponent* combat = nullptr;
        const EquipmentComponent*  equip  = nullptr;
        if (player != entt::null) {
            inv    = reg.try_get<InventoryComponent>(player);
            health = reg.try_get<HealthComponent>(player);
            xp     = reg.try_get<ExperienceComponent>(player);
            combat = reg.try_get<CombatStatsComponent>(player);
            equip  = reg.try_get<EquipmentComponent>(player);
        }

        const GameStateComponent* gs = nullptr;
        for (auto e : reg.view<GameStateComponent>()) {
            gs = &reg.get<GameStateComponent>(e);
            break;
        }

        // --- Stats panel (top-left) ---
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("Survival HUD", nullptr, flags)) {
            if (health) {
                float frac = health->maxHp > 0.0f ? health->hp / health->maxHp : 0.0f;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
                ImGui::ProgressBar(frac, ImVec2(220, 18),
                                   fmt::format("HP {:.0f}/{:.0f}", health->hp, health->maxHp).c_str());
                ImGui::PopStyleColor();
            }
            if (xp) {
                float frac = xp->xpToNext > 0.0f ? xp->xp / xp->xpToNext : 0.0f;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.45f, 0.65f, 0.95f, 1.0f));
                ImGui::ProgressBar(frac, ImVec2(220, 14),
                                   fmt::format("LV {}  ({:.0f}/{:.0f} XP)", xp->level, xp->xp, xp->xpToNext).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            if (inv) {
                ImGui::Text("Meat  %d", inv->meat);
                ImGui::Text("Wood  %d", inv->wood);
                ImGui::Text("Stone %d", inv->stone);
                ImGui::Text("Rescued %d", inv->survivors);
            }
            if (combat && equip) {
                ImGui::Separator();
                ImGui::Text("Weapon LV %d  (DMG %.0f)", equip->weaponLevel, combat->attackDamage);
                ImGui::Text("Armor  LV %d", equip->armorLevel);
            }
            if (gs) {
                ImGui::Separator();
                ImGui::Text("Survived %.0fs", gs->survivalTime);
            }
        }
        ImGui::End();

        // --- Controls hint (bottom-left) ---
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(16, io.DisplaySize.y - 16), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.40f);
        if (ImGui::Begin("Controls", nullptr, flags)) {
            ImGui::Text("WASD: move   Space/E: swing torch");
            ImGui::Text("1: upgrade weapon (wood+stone)");
            ImGui::Text("2: upgrade armor (meat+stone)");
        }
        ImGui::End();

        // --- Game over banner (center) ---
        if (gs && gs->gameOver) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.75f);
            if (ImGui::Begin("GameOver", nullptr, flags)) {
                ImGui::Text("THE COLD TOOK YOU");
                if (gs) ImGui::Text("Survived %.0f seconds", gs->survivalTime);
                if (inv) ImGui::Text("Rescued %d survivors", inv->survivors);
                ImGui::Text("Press ESC to quit");
            }
            ImGui::End();
        }
    }

}// namespace

int main(int argc, char* args[]) {
    args::ArgumentParser parser{ "Whiteout Survival — ad minigame (Project Vapor)." };
    args::Group windowGroup(parser, "Window:");
    args::ValueFlag<Uint32> width(windowGroup, "number", "Window width", { 'w', "width" }, 1280);
    args::ValueFlag<Uint32> height(windowGroup, "number", "Window height", { 'h', "height" }, 720);
    args::Group graphicsGroup(parser, "Graphics:", args::Group::Validators::Xor);
    args::Flag useMetal(graphicsGroup, "Metal", "Use Metal backend", { "metal" });
    args::Flag useVulkan(graphicsGroup, "Vulkan", "Use Vulkan backend", { "vulkan" });
    args::Group helpGroup(parser, "Help:");
    args::HelpFlag help(helpGroup, "help", "Display help menu", { "help" });
    if (argc > 1) {
        try {
            parser.ParseCLI(argc, args);
        } catch (args::Help) {
            std::cout << parser;
            return 0;
        } catch (args::ParseError& e) {
            std::cerr << e.what() << std::endl << parser;
            return 1;
        } catch (args::ValidationError& e) {
            std::cerr << e.what() << std::endl << parser;
            return 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print("SDL could not initialize! Error: {}\n", SDL_GetError());
        return 1;
    }

    const char* winTitle;
    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    GraphicsBackend gfxBackend;
#if defined(__APPLE__)
    if (useVulkan) {
        winTitle = "Whiteout Survival (Vulkan)";
        winFlags |= SDL_WINDOW_VULKAN;
        gfxBackend = GraphicsBackend::Vulkan;
    } else {
        winTitle = "Whiteout Survival (Metal)";
        winFlags |= SDL_WINDOW_METAL;
        gfxBackend = GraphicsBackend::Metal;
    }
#else
    winTitle = "Whiteout Survival (Vulkan)";
    winFlags |= SDL_WINDOW_VULKAN;
    gfxBackend = GraphicsBackend::Vulkan;
#endif

    auto window = SDL_CreateWindow(winTitle, width.Get(), height.Get(), winFlags);
    if (!window) {
        fmt::print(stderr, "Failed to create SDL_Window: {}\n", SDL_GetError());
        return 1;
    }
    int windowWidth, windowHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    Vapor::RNG rng(1337);

    auto renderer = createRenderer(gfxBackend);
    renderer->init(window);

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler(), renderer->getDebugDraw());

    fmt::print("Engine initialized\n");

    entt::registry registry;
    physics->attach(registry);// wires CharacterBody cleanup on entity destroy

    auto scene = std::make_shared<Scene>("survival");

    Survival::GameWorld world = Survival::buildWorld(registry, scene, *physics, windowWidth, windowHeight, rng);

    // Gather lights into the scene so the renderer knows how many to allocate buffers for.
    Survival::LightGatherSystem::update(registry, scene.get());

    // Stage everything once. After staging assigns material/instance IDs, clear
    // stagedMeshes so the ECS draw path is the single source of truth (otherwise
    // each mesh would be drawn twice). The geometry stays in the scene buffers.
    renderer->stage(scene);
    scene->stagedMeshes.clear();
    scene->stagedMeshTransforms.clear();

    renderer->setImGuiCallback([&]() { drawHUD(registry); });

    fmt::print("World built: torch up, the snow is waiting.\n");

    float time = SDL_GetTicks() / 1000.0f;
    bool quit = false;
    auto& inputManager = engineCore->getInputManager();

    while (!quit) {
        float currTime = SDL_GetTicks() / 1000.0f;
        float deltaTime = currTime - time;
        time = currTime;
        if (deltaTime > 0.1f) deltaTime = 0.1f;// clamp after stalls

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
                if (e.key.scancode == SDL_SCANCODE_F3) physics->setDebugEnabled(!physics->isDebugEnabled());
                break;
            case SDL_EVENT_WINDOW_RESIZED: {
                windowWidth = e.window.data1;
                windowHeight = e.window.data2;
                break;
            }
            default:
                break;
            }
        }

        const auto& input = inputManager.getInputState();
        float aspect = (float)windowWidth / (float)windowHeight;

        bool gameOver = false;
        for (auto se : registry.view<Survival::GameStateComponent>())
            gameOver = registry.get<Survival::GameStateComponent>(se).gameOver;

        // --- Gameplay (frozen once the player dies) ---
        if (!gameOver) {
            Survival::PlayerInputSystem::update(registry, input);
            Survival::PlayerMovementSystem::update(registry);
            Survival::PlayerAttackSystem::update(registry, deltaTime);
            Survival::EnemyAISystem::update(registry, deltaTime, rng);
            Survival::DamageSystem::update(registry, deltaTime);
            Survival::DeathLootSystem::update(registry, rng);
            Survival::PickupSystem::update(registry);
            Survival::SurvivorRescueSystem::update(registry);
            Survival::ProgressionSystem::update(registry);
            Survival::EquipmentUpgradeSystem::update(registry);
            Survival::SpawnerSystem::update(registry, deltaTime, rng);
            Survival::BobSystem::update(registry, deltaTime);
            Survival::HitFlashSystem::update(registry, deltaTime);
            Survival::TorchFlickerSystem::update(registry, deltaTime);
            Survival::GameOverSystem::update(registry, deltaTime);

            physics->process(registry, deltaTime);
            Survival::RecycleSystem::update(registry, physics.get());
        }

        // --- Camera, transforms, lights (always, so a dead player still renders) ---
        Survival::IsometricCameraSystem::update(registry, deltaTime, aspect);
        engineCore->update(deltaTime);
        Vapor::TransformSystem::update(registry);
        Survival::LightGatherSystem::update(registry, scene.get());

        // --- Render ---
        entt::entity camEntity = activeCamera(registry);
        if (camEntity != entt::null) {
            auto& cam = registry.get<Vapor::VirtualCameraComponent>(camEntity);
            Camera tempCamera;
            tempCamera.setEye(cam.position);
            tempCamera.setViewMatrix(cam.viewMatrix);
            tempCamera.setProjectionMatrix(cam.projectionMatrix);
            renderer->draw(registry, scene, tempCamera);
        }
    }

    physics->deinit();
    engineCore->shutdown();
    renderer->deinit();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
