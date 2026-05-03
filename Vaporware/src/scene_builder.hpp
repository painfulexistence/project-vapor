#pragma once
#include <SDL3/SDL.h>
#include <fmt/core.h>
#include "Vapor/mesh_builder.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/rng.hpp"
#include "Vapor/scene.hpp"
#include "components.hpp"
#include "pages/page_system.hpp"
#include "pages/hud_page.hpp"
#include "pages/letterbox_page.hpp"
#include "pages/subtitle_page.hpp"
#include "pages/scroll_text_page.hpp"
#include "pages/chapter_title_page.hpp"
#include "pages/main_menu_page.hpp"
#include "pages/pause_menu_page.hpp"
#include "pages/settings_page.hpp"
#include "pages/loading_screen_page.hpp"
#include <entt/entt.hpp>
#include <glm/vec3.hpp>
#include <memory>

struct SceneResources {
    std::shared_ptr<Scene> scene;
    std::shared_ptr<Material> material;
    entt::entity cube1 = entt::null;
    entt::entity global = entt::null;
};

inline SceneResources buildScene(
    entt::registry& registry,
    Physics3D& physics,
    std::shared_ptr<Scene> scene,
    std::shared_ptr<Material> material,
    int windowWidth,
    int windowHeight,
    Vapor::RNG& rng)
{
    SceneResources res;
    res.scene = scene;
    res.material = material;

    auto cube1 = registry.create();
    {
        auto& transform = registry.emplace<Vapor::TransformComponent>(cube1);
        transform.position = glm::vec3(-2.0f, 0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(cube1);
        col.halfSize = glm::vec3(0.5f, 0.5f, 0.5f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(cube1);
        rb.motionType = BodyMotionType::Dynamic;

        auto node = scene->createNode("Cube 1");
        scene->addMeshToNode(node, MeshBuilder::buildCube(1.0f, material));
        node->setPosition(transform.position);
        node->body = physics.createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics.addBody(node->body, true);

        auto& nodeRef = registry.emplace<SceneNodeReferenceComponent>(cube1);
        nodeRef.node = node;

        auto& rotateComp = registry.emplace<AutoRotateComponent>(cube1);
        rotateComp.axis = glm::vec3(0.0f, 1.0f, -1.0f);
        rotateComp.speed = 1.5f;
    }
    res.cube1 = cube1;

    auto cube2 = registry.create();
    {
        auto& transform = registry.emplace<Vapor::TransformComponent>(cube2);
        transform.position = glm::vec3(2.0f, 0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(cube2);
        col.halfSize = glm::vec3(0.5f, 0.5f, 0.5f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(cube2);
        rb.motionType = BodyMotionType::Dynamic;

        auto node = scene->createNode("Cube 2");
        scene->addMeshToNode(node, MeshBuilder::buildCube(1.0f, material));
        node->setPosition(transform.position);
        node->body = physics.createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics.addBody(node->body, true);

        auto& nodeRef = registry.emplace<SceneNodeReferenceComponent>(cube2);
        nodeRef.node = node;
    }

    auto floor = registry.create();
    {
        auto& transform = registry.emplace<Vapor::TransformComponent>(floor);
        transform.position = glm::vec3(0.0f, -0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(floor);
        col.halfSize = glm::vec3(50.0f, 0.5f, 50.0f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(floor);
        rb.motionType = BodyMotionType::Static;

        auto node = scene->createNode("Floor");
        node->setPosition(transform.position);
        node->body = physics.createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics.addBody(node->body, false);

        auto& nodeRef = registry.emplace<SceneNodeReferenceComponent>(floor);
        nodeRef.node = node;
    }

    scene->directionalLights.push_back({
        .direction = glm::vec3(0.5, -1.0, 0.0),
        .color = glm::vec3(1.0, 1.0, 1.0),
        .intensity = 10.0,
    });
    auto sunLight = registry.create();
    {
        auto& ref = registry.emplace<SceneDirectionalLightReferenceComponent>(sunLight);
        ref.lightIndex = 0;

        auto& logic = registry.emplace<DirectionalLightLogicComponent>(sunLight);
        logic.baseDirection = glm::vec3(0.5, -1.0, 0.0);
        logic.speed = 0.5f;
        logic.magnitude = 0.05f;
    }

    for (int i = 0; i < 8; i++) {
        scene->pointLights.push_back({ .position = glm::vec3(
                                           rng.RandomFloatInRange(-5.0f, 5.0f),
                                           rng.RandomFloatInRange(0.0f, 5.0f),
                                           rng.RandomFloatInRange(-5.0f, 5.0f)
                                       ),
                                       .color = glm::vec3(rng.RandomFloat(), rng.RandomFloat(), rng.RandomFloat()),
                                       .intensity = 5.0f * rng.RandomFloat(),
                                       .radius = 0.5f });
    }
    for (int i = 0; i < (int)scene->pointLights.size(); ++i) {
        auto e = registry.create();

        auto& ref = registry.emplace<ScenePointLightReferenceComponent>(e);
        ref.lightIndex = i;

        auto& logic = registry.emplace<LightMovementLogicComponent>(e);
        logic.speed = 0.5f;
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
        case 3:
            logic.pattern = MovementPattern::Spiral;
            break;
        }
    }

    auto flyCam = registry.create();
    {
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(flyCam);
        cam.isActive = true;
        cam.fov = glm::radians(60.0f);
        cam.aspect = (float)windowWidth / (float)windowHeight;
        cam.position = glm::vec3(0.0f, 0.0f, 3.0f);

        auto& fly = registry.emplace<FlyCameraComponent>(flyCam);
        fly.moveSpeed = 5.0f;

        registry.emplace<CharacterIntent>(flyCam);
    }

    auto followCam = registry.create();
    {
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(followCam);
        cam.isActive = false;
        cam.aspect = (float)windowWidth / (float)windowHeight;
        cam.position = glm::vec3(0.0f, 2.0f, 5.0f);

        auto& follow = registry.emplace<FollowCameraComponent>(followCam);
        follow.target = cube1;
        follow.offset = glm::vec3(0.0f, 2.0f, 5.0f);
    }

    // --- UI (one persistent entity owns all pages + trigger components) ---
    auto uiEntity = registry.create();
    registry.emplace<PersistentTag>(uiEntity);
    {
        auto& ui = registry.emplace<UIStateComponent>(uiEntity);

        // Overlay pages — eager-loaded, always resident
        ui.pages[PageID::HUD]          = { "assets/ui/hud.rml",          std::make_unique<HUDPage>() };
        ui.pages[PageID::Letterbox]    = { "assets/ui/letterbox.rml",    std::make_unique<LetterboxPage>() };
        ui.pages[PageID::Subtitle]     = { "assets/ui/subtitle.rml",     std::make_unique<SubtitlePage>() };
        ui.pages[PageID::ScrollText]   = { "assets/ui/scroll_text.rml",  std::make_unique<ScrollTextPage>() };
        ui.pages[PageID::ChapterTitle] = { "assets/ui/chapter_title.rml",std::make_unique<ChapterTitlePage>() };

        // Menu pages — lazy-loaded (document created only when first shown)
        ui.pages[PageID::MainMenu] = { "assets/ui/menus/main_menu.rml", std::make_unique<MainMenuPage>(
            [&registry] { PageSystem::popAll(registry); /* game start logic goes here */ },
            [] { SDL_Event e{}; e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e); }
        ), false, true };
        ui.pages[PageID::PauseMenu] = { "assets/ui/menus/pause_menu.rml", std::make_unique<PauseMenuPage>(
            [&registry] { PageSystem::pop(registry); },
            [&registry] { PageSystem::popAll(registry); PageSystem::push(registry, PageID::MainMenu); }
        ), false, true };
        ui.pages[PageID::Settings]      = { "assets/ui/menus/settings.rml",      std::make_unique<SettingsPage>(),     false, true };
        ui.pages[PageID::LoadingScreen] = { "assets/ui/menus/loading_screen.rml",std::make_unique<LoadingScreenPage>(),false, true };

        // Show main menu at startup
        PageSystem::push(registry, PageID::MainMenu);
    }

    // Trigger components — content/timing data for cinematic overlays
    {
        auto& sq = registry.emplace<SubtitleQueueComponent>(uiEntity);
        sq.autoAdvance = true;
        sq.queue = {
            { "NARRATOR",  "In a world where code meets creativity...", 3.0f },
            { "NARRATOR",  "One engine dared to dream differently.",    2.5f },
            { "",          "Press F8 to show chapter title.",           2.0f },
            { "DEVELOPER", "Welcome to Project Vapor.",                 2.5f },
            { "",          "(End of subtitle demo)",                    2.0f },
        };
    }
    {
        auto& stq = registry.emplace<ScrollTextQueueComponent>(uiEntity);
        stq.lines = {
            "Welcome to Project Vapor",
            "Press ENTER to advance text",
            "This is a teleprompter-style scroll effect",
            "Each line scrolls up and fades out",
            "While the next line fades in from below",
            "Perfect for cutscenes or tutorials",
            "End of demo - press ENTER to restart",
        };
    }
    {
        auto& ct = registry.emplace<ChapterTitleTriggerComponent>(uiEntity);
        ct.number       = "Chapter I";
        ct.title        = "The Beginning";
        ct.showRequested = true;
    }
    registry.emplace<SceneTransitionComponent>(uiEntity);

    res.global = registry.create();

    return res;
}
