#include "game.hpp"
#include "camera_manager.hpp"
#include <Vapor/mesh_builder.hpp>
#include <Vapor/rng.hpp>
#include <fmt/core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL.h>
#include "imgui.h"

// Game state that survives hot reload (stored in Game instance)
namespace {
    GameMemory* g_memory = nullptr;
    Vapor::CameraManager g_cameraManager;
    Node* g_cube1 = nullptr;
    Node* g_cube2 = nullptr;
    uint32_t g_frameCount = 0;
    float g_totalTime = 0.0f;
}

void Game::rebindNodes() {
    if (!g_memory || !g_memory->scene) return;

    g_cube1 = g_memory->scene->findNode("Cube 1").get();
    g_cube2 = g_memory->scene->findNode("Cube 2").get();

    fmt::print("[Game] Nodes rebound: cube1={}, cube2={}\n",
               g_cube1 ? "found" : "null",
               g_cube2 ? "found" : "null");
}

void Game::setupCameras() {
    if (!g_memory || !g_memory->window) return;

    g_cameraManager = Vapor::CameraManager{};  // Reset

    int windowWidth, windowHeight;
    SDL_GetWindowSize(g_memory->window, &windowWidth, &windowHeight);
    float aspectRatio = (float)windowWidth / (float)windowHeight;

    auto flyCam = std::make_unique<Vapor::FlyCam>(
        glm::vec3(0.0f, 2.0f, 8.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::radians(60.0f),
        aspectRatio,
        0.05f, 500.0f,
        5.0f, 1.5f
    );
    g_cameraManager.addCamera("fly", std::move(flyCam));

    if (g_cube1) {
        auto followCam = std::make_unique<Vapor::FollowCam>(
            g_cube1,
            glm::vec3(0.0f, 1.0f, 2.0f),
            glm::radians(60.0f),
            aspectRatio,
            0.05f, 500.0f,
            0.1f, 0.1f
        );
        g_cameraManager.addCamera("follow", std::move(followCam));
    }

    g_cameraManager.switchCamera("fly");
    fmt::print("[Game] Cameras ready. Press 1=Fly, 2=Follow\n");
}

void Game::loadScene() {
    if (!g_memory || !g_memory->engine) return;

    auto& resourceManager = g_memory->engine->getResourceManager();

    fmt::print("[Game] Loading scene...\n");
    auto sceneResource = resourceManager.loadScene(
        std::string("assets/models/Sponza/Sponza.gltf"),
        true, Vapor::LoadMode::Async,
        [](std::shared_ptr<Scene> s) {
            fmt::print("[Game] Scene loaded: {} nodes\n", s->nodes.size());
        }
    );

    g_memory->scene = sceneResource->get();

    // Lights
    RNG rng;
    g_memory->scene->directionalLights.push_back({
        .direction = glm::vec3(0.5, -1.0, 0.0),
        .color = glm::vec3(1.0, 1.0, 1.0),
        .intensity = 10.0,
    });
    for (int i = 0; i < 8; i++) {
        g_memory->scene->pointLights.push_back({
            .position = glm::vec3(rng.RandomFloatInRange(-5.0f, 5.0f), rng.RandomFloatInRange(0.0f, 5.0f), rng.RandomFloatInRange(-5.0f, 5.0f)),
            .color = glm::vec3(rng.RandomFloat(), rng.RandomFloat(), rng.RandomFloat()),
            .intensity = 5.0f * rng.RandomFloat(),
            .radius = 0.5f
        });
    }

    // Textures
    fmt::print("[Game] Loading textures...\n");
    auto albedo = resourceManager.loadImage("assets/textures/american_walnut_albedo.png", Vapor::LoadMode::Async);
    auto normal = resourceManager.loadImage("assets/textures/american_walnut_normal.png", Vapor::LoadMode::Async);
    auto roughness = resourceManager.loadImage("assets/textures/american_walnut_roughness.png", Vapor::LoadMode::Async);
    resourceManager.waitForAll();

    auto material = std::make_shared<Material>(Material{
        .albedoMap = albedo->get(),
        .normalMap = normal->get(),
        .roughnessMap = roughness->get(),
    });

    // Entities
    auto entity1 = g_memory->scene->createNode("Cube 1");
    g_memory->scene->addMeshToNode(entity1, MeshBuilder::buildCube(1.0f, material));
    entity1->setPosition(glm::vec3(-2.0f, 10.5f, 0.0f));
    entity1->body = g_memory->physics->createBoxBody(glm::vec3(.5f), glm::vec3(-2.0f, 0.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Dynamic);
    g_memory->physics->addBody(entity1->body, true);

    auto entity2 = g_memory->scene->createNode("Cube 2");
    g_memory->scene->addMeshToNode(entity2, MeshBuilder::buildCube(1.0f, material));
    entity2->setPosition(glm::vec3(2.0f, 0.5f, 0.0f));
    entity2->body = g_memory->physics->createBoxBody(glm::vec3(.5f), glm::vec3(2.0f, 0.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Dynamic);
    g_memory->physics->addBody(entity2->body, true);

    auto floor = g_memory->scene->createNode("Floor");
    floor->setPosition(glm::vec3(0.0f, -0.5f, 0.0f));
    floor->body = g_memory->physics->createBoxBody(glm::vec3(50.0f, .5f, 50.0f), glm::vec3(0.0f, -.5f, 0.0f), glm::identity<glm::quat>(), BodyMotionType::Static);
    g_memory->physics->addBody(floor->body, false);

    g_memory->renderer->stage(g_memory->scene);
    rebindNodes();
    fmt::print("[Game] Scene ready\n");
}

void Game::updateLights(float time) {
    if (!g_memory || !g_memory->scene) return;

    float speed = 0.5f;
    if (!g_memory->scene->directionalLights.empty()) {
        g_memory->scene->directionalLights[0].direction = glm::vec3(0.5, -1.0, 0.05 * sin(time * speed));
    }

    for (size_t i = 0; i < g_memory->scene->pointLights.size(); i++) {
        auto& l = g_memory->scene->pointLights[i];
        switch (i % 4) {
        case 0:
            l.position.x = 3.0f * cos(time * speed + i * 0.1f);
            l.position.z = 3.0f * sin(time * speed + i * 0.1f);
            l.position.y = 1.5f + 0.5f * sin(time * speed * 0.5f + i * 0.2f);
            break;
        case 1:
            l.position.x = 4.0f * sin(time * speed * 0.7f + i * 0.15f);
            l.position.z = 4.0f * sin(time * speed * 0.7f + i * 0.15f) * cos(time * speed * 0.7f + i * 0.15f);
            l.position.y = 1.0f + 1.0f * cos(time * speed * 0.3f + i * 0.1f);
            break;
        case 2:
            l.position.x = 4.0f * sin(time * speed * 0.6f + i * 0.12f);
            l.position.z = 2.0f * cos(time * speed * 0.8f + i * 0.18f);
            l.position.y = 0.5f + 2.0f * abs(sin(time * speed * 0.4f + i * 0.14f));
            break;
        case 3: {
            float r = 2.0f + 1.0f * sin(time * speed * 0.2f + i * 0.05f);
            l.position.x = r * cos(time * speed * 0.5f + i * 0.08f);
            l.position.z = r * sin(time * speed * 0.5f + i * 0.08f);
            l.position.y = 0.5f + 2.5f * (1.0f - cos(time * speed * 0.3f + i * 0.06f));
            break;
        }
        }
        l.intensity = 3.0f + 2.0f * (0.5f + 0.5f * sin(time * 0.3f + i * 0.1f));
    }
}

bool Game::init(GameMemory* memory) {
    g_memory = memory;
    fmt::print("[Game] init, is_initialized={}\n", memory->is_initialized);

    if (!memory->is_initialized) {
        loadScene();
        setupCameras();
        memory->is_initialized = true;
        fmt::print("[Game] First-time init complete\n");
    } else {
        // Hot reload
        fmt::print("[Game] Hot reload - rebinding\n");
        rebindNodes();
        setupCameras();
    }
    return true;
}

bool Game::update(GameMemory* memory, const FrameInput* input) {
    if (!memory || !input) return false;

    float dt = input->deltaTime;
    g_totalTime = input->totalTime;

    if (input->inputState) {
        const auto& is = *input->inputState;
        if (is.isPressed(Vapor::InputAction::Hotkey1)) {
            g_cameraManager.switchCamera("fly");
            fmt::print("[Game] Fly camera\n");
        }
        if (is.isPressed(Vapor::InputAction::Hotkey2)) {
            g_cameraManager.switchCamera("follow");
            fmt::print("[Game] Follow camera\n");
        }
        g_cameraManager.update(dt, is);
    }

    memory->engine->update(dt);

    if (g_cube1) {
        g_cube1->rotate(glm::vec3(0.0f, 1.0f, -1.0f), 1.5f * dt);
    }

    updateLights(g_totalTime);
    memory->scene->update(dt);
    memory->physics->process(memory->scene, dt);

    // ImGui
    auto& rm = memory->engine->getResourceManager();
    if (ImGui::Begin("Debug")) {
        if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Frame: %u", g_frameCount);
            ImGui::Text("Time: %.2f", g_totalTime);
            ImGui::Text("FPS: %.1f", 1.0f / dt);
        }
        if (ImGui::CollapsingHeader("Resources")) {
            ImGui::Text("Images: %zu", rm.getImageCacheSize());
            ImGui::Text("Scenes: %zu", rm.getSceneCacheSize());
            ImGui::Text("Meshes: %zu", rm.getMeshCacheSize());
        }
    }
    ImGui::End();

    // Render
    auto* cam = g_cameraManager.getCurrentCamera();
    if (cam && memory->renderer) {
        memory->renderer->draw(memory->scene, cam->getCamera());
    }

    g_frameCount++;
    return true;
}

void Game::shutdown(GameMemory* memory) {
    fmt::print("[Game] shutdown\n");
    g_cube1 = nullptr;
    g_cube2 = nullptr;
    g_cameraManager = Vapor::CameraManager{};
}
