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
    std::shared_ptr<Vapor::Material> material;
    entt::entity cube1 = entt::null;
    entt::entity global = entt::null;
};

inline SceneResources buildScene(
    entt::registry& registry,
    Physics3D& physics,
    std::shared_ptr<Scene> scene,
    std::shared_ptr<Vapor::Material> material,
    int windowWidth,
    int windowHeight,
    Vapor::RNG& rng
) {
    SceneResources res;
    res.scene = scene;
    res.material = material;

    auto cube1 = registry.create();
    {
        registry.emplace<Vapor::NameComponent>(cube1, Vapor::NameComponent{"Cube 1"});
        auto& transform = registry.emplace<Vapor::TransformComponent>(cube1);
        transform.position = glm::vec3(-2.0f, 0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(cube1);
        col.halfSize = glm::vec3(0.5f, 0.5f, 0.5f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(cube1);
        rb.motionType = BodyMotionType::Dynamic;
        rb.body = physics.createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics.addBody(rb.body, true);

        auto cubeMesh = MeshBuilder::buildCube(1.0f, material);
        scene->addMesh(cubeMesh);
        auto& meshRenderer = registry.emplace<Vapor::MeshRendererComponent>(cube1);
        meshRenderer.meshes.push_back(cubeMesh);

        auto& rotateComp = registry.emplace<AutoRotateComponent>(cube1);
        rotateComp.axis = glm::vec3(0.0f, 1.0f, -1.0f);
        rotateComp.speed = 1.5f;
    }
    res.cube1 = cube1;

    // Iridescent cube: same wood textures as cube1, thin-film iridescence on top
    auto iridescentMaterial = std::make_shared<Vapor::Material>(Vapor::Material{
        .baseColorFactor  = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
        .metallicFactor   = 0.0f,
        .roughnessFactor  = 1.0f,
        .albedoMap        = material->albedoMap,
        .normalMap        = material->normalMap,
        .roughnessMap     = material->roughnessMap,
        .clearcoat        = 0.9f,   // iridescence strength (reused field)
        .clearcoatGloss   = 0.45f,  // film thickness factor → ~480 nm (blue-green dominant)
        .materialType     = Vapor::MaterialType::Iridescent,
        .useIBL           = false,
    });

    auto cube2 = registry.create();
    {
        registry.emplace<Vapor::NameComponent>(cube2, Vapor::NameComponent{"Cube 2 (Iridescent)"});
        auto& transform = registry.emplace<Vapor::TransformComponent>(cube2);
        transform.position = glm::vec3(2.0f, 0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(cube2);
        col.halfSize = glm::vec3(0.5f, 0.5f, 0.5f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(cube2);
        rb.motionType = BodyMotionType::Dynamic;
        rb.body = physics.createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics.addBody(rb.body, true);

        auto cubeMesh2 = MeshBuilder::buildCube(1.0f, iridescentMaterial);
        scene->addMesh(cubeMesh2);
        auto& meshRenderer = registry.emplace<Vapor::MeshRendererComponent>(cube2);
        meshRenderer.meshes.push_back(cubeMesh2);
    }

    auto floor = registry.create();
    {
        registry.emplace<Vapor::NameComponent>(floor, Vapor::NameComponent{"Floor"});
        auto& transform = registry.emplace<Vapor::TransformComponent>(floor);
        transform.position = glm::vec3(0.0f, -0.5f, 0.0f);
        auto& col = registry.emplace<Vapor::BoxColliderComponent>(floor);
        col.halfSize = glm::vec3(50.0f, 0.5f, 50.0f);
        auto& rb = registry.emplace<Vapor::RigidbodyComponent>(floor);
        rb.motionType = BodyMotionType::Static;
        rb.body = physics.createBoxBody(col.halfSize, transform.position, transform.rotation, rb.motionType);
        physics.addBody(rb.body, false);
    }

    {
        auto sunLight = registry.create();
        registry.emplace<Vapor::NameComponent>(sunLight, Vapor::NameComponent{"Sun Light"});
        auto& dl      = registry.emplace<DirectionalLightComponent>(sunLight);
        dl.direction  = glm::normalize(glm::vec3(0.5f, -1.0f, 0.0f));
        dl.color      = glm::vec3(1.0f, 1.0f, 1.0f);
        dl.intensity  = 10.0f;

        auto& logic         = registry.emplace<DirectionalLightLogicComponent>(sunLight);
        logic.baseDirection = glm::vec3(0.5f, -1.0f, 0.0f);
        logic.speed         = 0.5f;
        logic.magnitude     = 0.05f;
    }

    // Enough lights to exercise tiled light culling and to lift the ambient
    // level so screen-space AO is visible (radius 0.5 made them near-invisible)
    for (int i = 0; i < 128; i++) {
        auto e         = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{fmt::format("Point Light {}", i)});
        auto& tc       = registry.emplace<Vapor::TransformComponent>(e);
        tc.position    = glm::vec3(
            rng.RandomFloatInRange(-10.0f, 10.0f),
            rng.RandomFloatInRange(0.5f, 4.0f),
            rng.RandomFloatInRange(-10.0f, 10.0f)
        );
        tc.isDirty     = true;

        auto& pl       = registry.emplace<PointLightComponent>(e);
        pl.color       = glm::vec3(
            0.3f + 0.7f * rng.RandomFloat(),
            0.3f + 0.7f * rng.RandomFloat(),
            0.3f + 0.7f * rng.RandomFloat()
        );
        pl.intensity   = 5.0f * rng.RandomFloat();
        pl.radius      = 0.5f;

        auto& logic    = registry.emplace<LightMovementLogicComponent>(e);
        logic.speed    = 0.5f;
        logic.timer    = i * 0.1f;

        switch (i % 4) {
        case 0:
            logic.pattern = MovementPattern::Circle;
            logic.radius  = 3.0f;
            logic.height  = 1.5f;
            break;
        case 1:
            logic.pattern = MovementPattern::Figure8;
            logic.radius  = 3.0f;
            break;
        case 2:
            logic.pattern = MovementPattern::Linear;
            logic.radius  = 3.0f;
            break;
        case 3:
            logic.pattern = MovementPattern::Spiral;
            break;
        }
    }

    // RT-showcase lights: one spot (cone falloff + RT shadow via the stochastic
    // pass's B channel) and one rect area light (RT-sampled soft penumbra via
    // the G channel).
    {
        auto e      = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{"Spot Light"});
        auto& tc    = registry.emplace<Vapor::TransformComponent>(e);
        tc.position = glm::vec3(0.0f, 6.0f, 0.0f);
        // Beam is the transform's -Z; rotate it to point straight down.
        tc.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        tc.isDirty  = true;
        auto& sl      = registry.emplace<SpotLightComponent>(e);
        sl.color      = glm::vec3(1.0f, 0.95f, 0.8f);
        sl.intensity  = 40.0f;
        sl.radius     = 15.0f;
        sl.innerAngle = 18.0f;
        sl.outerAngle = 28.0f;
    }
    {
        auto e      = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{"Rect Light"});
        auto& tc    = registry.emplace<Vapor::TransformComponent>(e);
        tc.position = glm::vec3(4.0f, 1.8f, -3.0f);  // vertical panel
        tc.isDirty  = true;
        auto& rl     = registry.emplace<Vapor::RectLightComponent>(e);
        rl.size      = glm::vec2(2.0f, 3.0f);
        rl.color     = glm::vec3(0.4f, 0.7f, 1.0f);
        rl.intensity = 8.0f;
    }

    auto flyCam = registry.create();
    {
        registry.emplace<Vapor::NameComponent>(flyCam, Vapor::NameComponent{"Fly Camera"});
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
        registry.emplace<Vapor::NameComponent>(followCam, Vapor::NameComponent{"Follow Camera"});
        auto& cam = registry.emplace<Vapor::VirtualCameraComponent>(followCam);
        cam.isActive = false;
        cam.aspect = (float)windowWidth / (float)windowHeight;
        cam.position = glm::vec3(0.0f, 2.0f, 5.0f);

        auto& follow = registry.emplace<FollowCameraComponent>(followCam);
        follow.target = cube1;
        follow.offset = glm::vec3(0.0f, 2.0f, 5.0f);
    }

    // --- UI navigator (singleton entity: owns PageID→entity map + menu stack) ---
    auto navEntity = registry.create();
    registry.emplace<Vapor::NameComponent>(navEntity, Vapor::NameComponent{"UI Navigator"});
    registry.emplace<PersistentTag>(navEntity);
    auto& nav = registry.emplace<UINavigatorComponent>(navEntity);

    auto addPage = [&](PageID id, std::string path, std::shared_ptr<Page> page,
                       bool lazyLoad = false, bool overlay = false) {
        auto e = registry.create();
        registry.emplace<UIDocumentComponent>(e, UIDocumentComponent{ .path = std::move(path), .lazyLoad = lazyLoad });
        registry.emplace<UIPageBehaviorComponent>(e, UIPageBehaviorComponent{ .page = std::move(page) });
        if (overlay) registry.emplace<UIOverlayTag>(e);
        nav.pages[id] = e;
    };

    // Overlay pages — eager-loaded, always resident
    addPage(PageID::HUD,          "ui/hud.rml",           std::make_shared<HUDPage>(),          false, true);
    addPage(PageID::Letterbox,    "ui/letterbox.rml",     std::make_shared<LetterboxPage>(),    false, true);
    addPage(PageID::Subtitle,     "ui/subtitle.rml",      std::make_shared<SubtitlePage>(),     false, true);
    addPage(PageID::ScrollText,   "ui/scroll_text.rml",   std::make_shared<ScrollTextPage>(),   false, true);
    addPage(PageID::ChapterTitle, "ui/chapter_title.rml", std::make_shared<ChapterTitlePage>(), false, true);

    // Menu pages — lazy-loaded (document created only when first shown)
    addPage(PageID::MainMenu, "ui/menus/main_menu.rml", std::make_shared<MainMenuPage>(
        [&registry] { PageSystem::popAll(registry); },
        [] { SDL_Event e{}; e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e); }
    ), true);
    addPage(PageID::PauseMenu, "ui/menus/pause_menu.rml", std::make_shared<PauseMenuPage>(
        [&registry] { PageSystem::pop(registry); },
        [&registry] { PageSystem::popAll(registry); PageSystem::push(registry, PageID::MainMenu); }
    ), true);
    addPage(PageID::Settings,      "ui/menus/settings.rml",       std::make_shared<SettingsPage>(),      true);
    addPage(PageID::LoadingScreen, "ui/menus/loading_screen.rml",  std::make_shared<LoadingScreenPage>(), true);

    PageSystem::push(registry, PageID::MainMenu);

    // Trigger components — content/timing data for cinematic overlays
    {
        auto& sq = registry.emplace<SubtitleQueueComponent>(navEntity);
        sq.autoAdvance = true;
        sq.queue = {
            { "NARRATOR",  "In a world where code meets creativity...", 3.0f },
            { "NARRATOR",  "One engine dared to dream differently.",    2.5f },
            { "",          "Press F8 to show chapter title.",           2.0f },
            { "DEVELOPER", "Welcome to Project Vapor.",                 2.5f },
            { "",          "(End of subtitle demo)",                    2.0f },
        };
        // FSM components for subtitle state machine (FSMInitSystem auto-initializes)
        registry.emplace<Vapor::FSMDefinition>(navEntity, createSubtitleFSM());
        registry.emplace<Vapor::FSMEventQueue>(navEntity);
    }
    {
        auto& stq = registry.emplace<ScrollTextQueueComponent>(navEntity);
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
        auto& ct = registry.emplace<ChapterTitleTriggerComponent>(navEntity);
        ct.number       = "Chapter I";
        ct.title        = "The Beginning";
        ct.showRequested = true;
    }
    registry.emplace<SceneTransitionComponent>(navEntity);

    // ── Particle sea ──────────────────────────────────────────────────────────
    // Two attractors create a dual-focus gravitational field; curl noise turbulence
    // (set in main) causes the stream to swirl rather than collapse to a point.
    {
        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{"Particle Attractor A"});
        auto& tc = registry.emplace<Vapor::TransformComponent>(e);
        tc.position = glm::vec3(0.0f, 3.0f, 0.0f);
        auto& attr = registry.emplace<Vapor::ParticleAttractorComponent>(e);
        attr.strength = 40.0f;
    }
    {
        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{"Particle Attractor B"});
        auto& tc = registry.emplace<Vapor::TransformComponent>(e);
        tc.position = glm::vec3(6.0f, 1.0f, -4.0f);
        auto& attr = registry.emplace<Vapor::ParticleAttractorComponent>(e);
        attr.strength = 18.0f;
    }
    {
        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{"Wind Field"});
        auto& wf = registry.emplace<Vapor::WindFieldComponent>(e);
        wf.direction  = glm::normalize(glm::vec3(1.0f, 0.1f, 0.4f));
        wf.strength   = 0.6f;
        wf.turbulence = 2.0f;
    }
    // Two one-shot bursts: each fires its whole batch in one frame. Immortal
    // particles (lifetime < 0) never age out, so they persist and bounce at the
    // sim boundary; their slots are held until the emitter entity is destroyed.
    {
        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{"Particle Sea Emitter A"});
        auto& tc = registry.emplace<Vapor::TransformComponent>(e);
        tc.position = glm::vec3(0.0f, 5.0f, 0.0f);
        auto& em = registry.emplace<Vapor::ParticleEmitterComponent>(e);
        em.maxParticles     = 70'000;
        em.oneShot          = true;
        em.particleLifetime = -1.0f; // immortal
        em.speed            = 6.0f;
        em.spread           = 3.14159265f; // full sphere
        em.emitDirection    = glm::vec3(0.0f, 1.0f, 0.0f);
        em.color            = glm::vec4(0.45f, 0.55f, 1.0f, 1.0f); // indigo-blue
        auto& pr = registry.emplace<Vapor::ParticleRendererComponent>(e);
        pr.blendMode = Vapor::ParticleBlendMode::Additive; // glow
        pr.size      = 0.1f;
    }
    {
        auto e = registry.create();
        registry.emplace<Vapor::NameComponent>(e, Vapor::NameComponent{"Particle Sea Emitter B"});
        auto& tc = registry.emplace<Vapor::TransformComponent>(e);
        tc.position = glm::vec3(3.0f, 4.0f, -2.0f);
        auto& em = registry.emplace<Vapor::ParticleEmitterComponent>(e);
        em.maxParticles     = 30'000;
        em.oneShot          = true;
        em.particleLifetime = -1.0f; // immortal
        em.speed            = 6.0f;
        em.spread           = 3.14159265f; // full sphere
        em.emitDirection    = glm::vec3(0.0f, 1.0f, 0.0f);
        em.color            = glm::vec4(1.0f, 0.55f, 0.35f, 1.0f); // warm amber
        auto& pr = registry.emplace<Vapor::ParticleRendererComponent>(e);
        pr.blendMode = Vapor::ParticleBlendMode::AlphaBlend; // smoke-like, contrasts A
        pr.size      = 0.2f;
    }

    res.global = registry.create();
    registry.emplace<Vapor::NameComponent>(res.global, Vapor::NameComponent{"Scene Global"});

    return res;
}
