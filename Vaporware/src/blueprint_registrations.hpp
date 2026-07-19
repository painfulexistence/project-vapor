#pragma once
// Registers Vaporware's gameplay components with the engine's blueprint
// applier registry, so scenes/main.json can author them directly. Call
// registerAppBlueprintComponents() once, before the first loadScene().
//
// Split by mechanism:
//   - plain aggregates ride the Boost.PFR auto-applier (fields by name)
//   - container-bearing / string-enum components take hand-written appliers
//   - uiPage resolves a page-name → Page-subclass factory (behavior objects
//     can't live in JSON; their NAME can — same pattern as the registry itself)
//   - pointLightField expands into N light entities (declarative stand-in for
//     the old RNG loop in scene_builder; seeded, so layouts are reproducible)

#include "Vapor/rng.hpp"
#include "Vapor/scene_blueprint.hpp"
#include "components.hpp"
#include "pages/page_system.hpp"// re-exports Vapor::Page/PageID/PageSystem/UI* into the app namespace
#include "pages/chapter_title_page.hpp"
#include "pages/hud_page.hpp"
#include "pages/letterbox_page.hpp"
#include "pages/loading_screen_page.hpp"
#include "pages/main_menu_page.hpp"
#include "pages/pause_menu_page.hpp"
#include "pages/scroll_text_page.hpp"
#include "pages/settings_page.hpp"
#include "pages/subtitle_page.hpp"
#include <SDL3/SDL_events.h>
#include <fmt/core.h>

inline void registerAppBlueprintComponents() {
    auto& r = Vapor::BlueprintComponents::instance();

    // Plain aggregates — PFR fills fields by JSON key.
    r.registerComponent<AutoRotateComponent>("autoRotate");
    r.registerComponent<ChapterTitleTriggerComponent>("chapterTitleTrigger");
    r.registerComponent<SceneTransitionComponent>("sceneTransition");
    r.registerComponent<CharacterIntent>("characterIntent");
    r.registerComponent<PersistentTag>("persistentTag");
    r.registerComponent<Vapor::UINavigatorComponent>("uiNavigator");

    r.registerApplier("lightMovementLogic", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
        LightMovementLogicComponent logic;
        const std::string pattern = j.value("pattern", "circle");
        logic.pattern = pattern == "figure8"  ? MovementPattern::Figure8
                        : pattern == "linear" ? MovementPattern::Linear
                        : pattern == "spiral" ? MovementPattern::Spiral
                                              : MovementPattern::Circle;
        logic.speed = j.value("speed", logic.speed);
        logic.timer = j.value("timer", logic.timer);
        logic.radius = j.value("radius", logic.radius);
        logic.height = j.value("height", logic.height);
        reg.emplace_or_replace<LightMovementLogicComponent>(e, logic);
    });

    r.registerApplier("subtitleQueue", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
        SubtitleQueueComponent sq;
        sq.autoAdvance = j.value("autoAdvance", sq.autoAdvance);
        if (j.contains("queue") && j.at("queue").is_array()) {
            for (const auto& entry : j.at("queue")) {
                if (!entry.is_object()) continue;
                sq.queue.push_back(SubtitleEntry{
                    .speaker = entry.value("speaker", ""),
                    .text = entry.value("text", ""),
                    .duration = entry.value("duration", 3.0f),
                });
            }
        }
        reg.emplace_or_replace<SubtitleQueueComponent>(e, std::move(sq));
    });

    r.registerApplier("scrollTextQueue", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
        ScrollTextQueueComponent stq;
        if (j.contains("lines") && j.at("lines").is_array())
            for (const auto& line : j.at("lines"))
                if (line.is_string()) stq.lines.push_back(line.get<std::string>());
        reg.emplace_or_replace<ScrollTextQueueComponent>(e, std::move(stq));
    });

    // UI page: document path + behavior. The Page subclass comes from a
    // name-keyed factory; the entity is wired into the UINavigatorComponent,
    // which must be declared on an EARLIER entity in the scene (appliers run
    // in entity order).
    r.registerApplier("uiPage", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
        using Vapor::PageID;
        using PageFactory = std::function<std::shared_ptr<Page>(entt::registry&)>;
        struct PageDesc {
            PageID id;
            PageFactory make;
        };
        static const std::unordered_map<std::string, PageDesc> pages = {
            { "HUD", { PageID::HUD, [](entt::registry&) { return std::make_shared<HUDPage>(); } } },
            { "Letterbox", { PageID::Letterbox, [](entt::registry&) { return std::make_shared<LetterboxPage>(); } } },
            { "Subtitle", { PageID::Subtitle, [](entt::registry&) { return std::make_shared<SubtitlePage>(); } } },
            { "ScrollText",
              { PageID::ScrollText, [](entt::registry&) { return std::make_shared<ScrollTextPage>(); } } },
            { "ChapterTitle",
              { PageID::ChapterTitle, [](entt::registry&) { return std::make_shared<ChapterTitlePage>(); } } },
            { "MainMenu",
              { PageID::MainMenu,
                [](entt::registry& reg) {
                    return std::make_shared<MainMenuPage>(
                        [&reg] { Vapor::PageSystem::popAll(reg); },
                        [] {
                            SDL_Event quit{};
                            quit.type = SDL_EVENT_QUIT;
                            SDL_PushEvent(&quit);
                        }
                    );
                } } },
            { "PauseMenu",
              { PageID::PauseMenu,
                [](entt::registry& reg) {
                    return std::make_shared<PauseMenuPage>(
                        [&reg] { Vapor::PageSystem::pop(reg); },
                        [&reg] {
                            Vapor::PageSystem::popAll(reg);
                            Vapor::PageSystem::push(reg, PageID::MainMenu);
                        }
                    );
                } } },
            { "Settings", { PageID::Settings, [](entt::registry&) { return std::make_shared<SettingsPage>(); } } },
            { "LoadingScreen",
              { PageID::LoadingScreen, [](entt::registry&) { return std::make_shared<LoadingScreenPage>(); } } },
        };

        const std::string id = j.value("id", "");
        const auto it = pages.find(id);
        if (it == pages.end()) {
            fmt::print(stderr, "uiPage: unknown page id '{}'\n", id);
            return;
        }
        reg.emplace_or_replace<Vapor::UIDocumentComponent>(
            e, Vapor::UIDocumentComponent{ .path = j.value("path", ""), .lazyLoad = j.value("lazy", false) }
        );
        reg.emplace_or_replace<Vapor::UIPageBehaviorComponent>(
            e, Vapor::UIPageBehaviorComponent{ .page = it->second.make(reg) }
        );
        if (j.value("overlay", false)) reg.emplace_or_replace<Vapor::UIOverlayTag>(e);

        auto navView = reg.view<Vapor::UINavigatorComponent>();
        if (navView.begin() == navView.end()) {
            fmt::print(stderr, "uiPage '{}': no UINavigatorComponent exists yet — declare the navigator first\n", id);
            return;
        }
        navView.get<Vapor::UINavigatorComponent>(*navView.begin()).pages[it->second.id] = e;
    });

    // Declarative light field: N seeded-random point lights with cycling
    // movement patterns (Circle/Figure8/Linear/Spiral), replacing the RNG loop
    // scene_builder used to run. Lights spawn as separate entities.
    r.registerApplier("pointLightField", [](entt::registry& reg, entt::entity, const nlohmann::json& j) {
        const int count = j.value("count", 128);
        Vapor::RNG rng(j.value("seed", 1u));
        const glm::vec3 areaMin(j.value("areaMinX", -10.0f), j.value("areaMinY", 0.5f), j.value("areaMinZ", -10.0f));
        const glm::vec3 areaMax(j.value("areaMaxX", 10.0f), j.value("areaMaxY", 4.0f), j.value("areaMaxZ", 10.0f));
        for (int i = 0; i < count; ++i) {
            const auto e = reg.create();
            reg.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{ fmt::format("Point Light {}", i) });
            auto& tc = reg.emplace<Vapor::TransformComponent>(e);
            tc.position = glm::vec3(
                rng.RandomFloatInRange(areaMin.x, areaMax.x), rng.RandomFloatInRange(areaMin.y, areaMax.y),
                rng.RandomFloatInRange(areaMin.z, areaMax.z)
            );
            tc.isDirty = true;
            auto& pl = reg.emplace<Vapor::PointLightComponent>(e);
            pl.color = glm::vec3(
                0.3f + 0.7f * rng.RandomFloat(), 0.3f + 0.7f * rng.RandomFloat(), 0.3f + 0.7f * rng.RandomFloat()
            );
            pl.intensity = j.value("intensity", 5.0f) * rng.RandomFloat();
            pl.radius = j.value("radius", 0.5f);
            auto& logic = reg.emplace<LightMovementLogicComponent>(e);
            logic.speed = j.value("speed", 0.5f);
            logic.timer = i * 0.1f;
            switch (i % 4) {
            case 0:
                logic.pattern = MovementPattern::Circle;
                logic.radius = 3.0f;
                logic.height = 1.5f;
                break;
            case 1:
                logic.pattern = MovementPattern::Figure8;
                logic.radius = 3.0f;
                break;
            case 2:
                logic.pattern = MovementPattern::Linear;
                logic.radius = 3.0f;
                break;
            case 3: logic.pattern = MovementPattern::Spiral; break;
            }
        }
    });
}
