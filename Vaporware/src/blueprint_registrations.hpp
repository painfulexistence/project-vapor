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

#include "Vapor/resource_manager.hpp"
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
#include <cmath>
#include <fmt/core.h>

inline void registerAppBlueprintComponents(Vapor::ResourceManager& resourceManager) {
    auto& r = Vapor::BlueprintComponents::instance();

    // Plain aggregates — PFR fills fields by JSON key.
    r.registerComponent<AutoRotateComponent>("autoRotate");
    r.registerComponent<ChapterTitleTriggerComponent>("chapterTitleTrigger");
    r.registerComponent<SceneTransitionComponent>("sceneTransition");
    r.registerComponent<CharacterIntent>("characterIntent");
    r.registerComponent<PersistentTag>("persistentTag");
    r.registerComponent<Vapor::UINavigatorComponent>("uiNavigator");
    r.registerComponent<FpsTextComponent>("fpsText");

    // sprite: the atlas is authored by NAME and resolved against the app's
    // ResourceManager (AtlasHandle itself is not authorable). The atlas must
    // be registered before instantiate() — registration stays code-side.
    // NOTE: captures resourceManager by pointer; it outlives every
    // instantiate() call (both live for the whole of main()).
    r.registerApplier("sprite", [rm = &resourceManager](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
        Vapor::SpriteComponent sprite;
        const std::string atlasName = j.value("atlas", "");
        sprite.atlas = rm->getAtlasHandle(atlasName);
        if (!sprite.atlas.valid()) {
            fmt::print(stderr, "blueprint: sprite atlas '{}' not registered — sprite hidden\n", atlasName);
        }
        sprite.frameIndex = j.value("frameIndex", sprite.frameIndex);
        const auto readV2 = [&](const char* key, glm::vec2 fallback) {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_array() || it->size() < 2) return fallback;
            return glm::vec2{ (*it)[0].get<float>(), (*it)[1].get<float>() };
        };
        sprite.size = readV2("size", sprite.size);
        sprite.pivot = readV2("pivot", sprite.pivot);
        if (const auto it = j.find("tint"); it != j.end() && it->is_array() && it->size() >= 4) {
            sprite.tint = { (*it)[0].get<float>(), (*it)[1].get<float>(),
                            (*it)[2].get<float>(), (*it)[3].get<float>() };
        }
        sprite.sortingLayer = j.value("sortingLayer", sprite.sortingLayer);
        sprite.orderInLayer = j.value("orderInLayer", sprite.orderInLayer);
        sprite.flipX = j.value("flipX", sprite.flipX);
        sprite.flipY = j.value("flipY", sprite.flipY);
        sprite.visible = j.value("visible", sprite.visible);
        reg.emplace_or_replace<Vapor::SpriteComponent>(e, sprite);
    });

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
    r.registerApplier("pointLightField", [](entt::registry& reg, entt::entity field, const nlohmann::json& j) {
        const int count = j.value("count", 128);
        Vapor::RNG rng(j.value("seed", 1u));
        const glm::vec3 areaMin(j.value("areaMinX", -10.0f), j.value("areaMinY", 0.5f), j.value("areaMinZ", -10.0f));
        const glm::vec3 areaMax(j.value("areaMaxX", 10.0f), j.value("areaMaxY", 4.0f), j.value("areaMaxZ", 10.0f));
        for (int i = 0; i < count; ++i) {
            const auto e = reg.create();
            reg.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{ fmt::format("Point Light {}", i) });
            auto& tc = reg.emplace<Vapor::TransformComponent>(e);
            // Parent each spawned light under the field entity itself: the field
            // is the scene-graph owner of its emitted lights, so the inspector
            // shows "Point Light Field > Point Light N" instead of N orphans at
            // the root. The field's transform is identity, so the world-space
            // positions authored below are unchanged by the reparent.
            tc.parent = field;
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

    // Declarative rainbow quad grid (cols x rows Shape2D children under the
    // grid entity, hue cycling across cells) — replaces the immediate-mode
    // canvas demo loop. Cell positions are LOCAL; place the grid entity to
    // move the whole pattern.
    r.registerApplier("rainbowGrid", [](entt::registry& reg, entt::entity grid, const nlohmann::json& j) {
        const int cols = j.value("cols", 10);
        const int rows = j.value("rows", 5);
        const float spacing = j.value("spacing", 25.0f);
        const float size = j.value("size", 20.0f);
        const float alpha = j.value("alpha", 0.8f);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                const auto e = reg.create();
                reg.emplace<Vapor::NameComponent>(
                    e, Vapor::NameComponent{ fmt::format("Rainbow Quad {}", y * cols + x) });
                auto& tc = reg.emplace<Vapor::TransformComponent>(e);
                tc.parent = grid;
                tc.position = glm::vec3(x * spacing, y * spacing, 0.0f);
                tc.isDirty = true;
                const float hue = static_cast<float>(x + y * cols) / static_cast<float>(cols * rows);
                auto& shape = reg.emplace<Vapor::Shape2DComponent>(e);
                shape.kind = Vapor::Shape2DComponent::Kind::Quad;
                shape.size = { size, size };
                shape.color = glm::vec4(
                    0.5f + 0.5f * std::sin(hue * 6.28f),
                    0.5f + 0.5f * std::sin(hue * 6.28f + 2.09f),
                    0.5f + 0.5f * std::sin(hue * 6.28f + 4.18f),
                    alpha
                );
            }
        }
    });
}
