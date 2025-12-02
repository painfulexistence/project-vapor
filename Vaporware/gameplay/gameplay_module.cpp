#include "../src/hot_reload/game_memory.hpp"
#include "../src/camera_manager.hpp"
#include <Vapor/scene.hpp>
#include <Vapor/physics_3d.hpp>
#include <Vapor/input_manager.hpp>
#include <Vapor/mesh_builder.hpp>
#include <Vapor/rng.hpp>
#include <Vapor/engine_core.hpp>
#include <fmt/core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"

// Platform-specific export macro
#ifdef _WIN32
    #define GAME_API __declspec(dllexport)
#else
    #define GAME_API __attribute__((visibility("default")))
#endif

namespace {

// Global game memory pointer (set by game_init, used by game_memory)
Game::GameMemory* g_memory = nullptr;

// Local state that gets rebuilt on hot reload
struct LocalState {
    // Camera manager
    Vapor::CameraManager cameraManager;

    // Node references (need to rebind after hot reload)
    Node* cube1 = nullptr;
    Node* cube2 = nullptr;

    // Frame counter
    uint32_t frameCount = 0;
    float totalTime = 0.0f;

    bool initialized = false;
};

LocalState g_local;

// Helper to rebind node pointers after hot reload
void rebindNodes() {
    if (!g_memory || !g_memory->scene) return;

    g_local.cube1 = g_memory->scene->findNode("Cube 1").get();
    g_local.cube2 = g_memory->scene->findNode("Cube 2").get();

    fmt::print("[Gameplay] Nodes rebound: cube1={}, cube2={}\n",
               g_local.cube1 ? "found" : "null",
               g_local.cube2 ? "found" : "null");
}

void setupCameras() {
    if (!g_memory || !g_memory->window) return;

    int windowWidth, windowHeight;
    SDL_GetWindowSize(g_memory->window, &windowWidth, &windowHeight);
    float aspectRatio = (float)windowWidth / (float)windowHeight;

    // Add FlyCam (free-flying camera)
    auto flyCam = std::make_unique<Vapor::FlyCam>(
        glm::vec3(0.0f, 2.0f, 8.0f),     // Eye position
        glm::vec3(0.0f, 0.0f, 0.0f),     // Look at center
        glm::vec3(0.0f, 1.0f, 0.0f),     // Up vector
        glm::radians(60.0f),              // FOV
        aspectRatio,
        0.05f,                            // Near plane
        500.0f,                           // Far plane
        5.0f,                             // Move speed
        1.5f                              // Rotate speed
    );
    g_local.cameraManager.addCamera("fly", std::move(flyCam));

    // Add FollowCam (follows Cube 1)
    if (g_local.cube1) {
        auto followCam = std::make_unique<Vapor::FollowCam>(
            g_local.cube1,
            glm::vec3(0.0f, 1.0f, 2.0f),     // Offset
            glm::radians(60.0f),
            aspectRatio,
            0.05f,
            500.0f,
            0.1f,                             // Smooth factor
            0.1f                              // Deadzone
        );
        g_local.cameraManager.addCamera("follow", std::move(followCam));
    }

    g_local.cameraManager.switchCamera("fly");

    fmt::print("Camera controls:\n");
    fmt::print("  Press '1' - Switch to Fly Camera\n");
    fmt::print("  Press '2' - Switch to Follow Camera\n");
}

void loadScene() {
    if (!g_memory || !g_memory->engine) return;

    auto& resourceManager = g_memory->engine->getResourceManager();

    // Load scene
    fmt::print("[Gameplay] Loading scene...\n");
    auto sceneResource = resourceManager.loadScene(
        std::string("assets/models/Sponza/Sponza.gltf"),
        true,  // optimized
        Vapor::LoadMode::Async,
        [](std::shared_ptr<Scene> loadedScene) {
            fmt::print("[Gameplay] Scene loaded with {} nodes\n", loadedScene->nodes.size());
        }
    );

    g_memory->scene = sceneResource->get();
    fmt::print("[Gameplay] Scene ready\n");

    // Add lights
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

    // Load textures
    fmt::print("[Gameplay] Loading textures...\n");
    auto albedoResource = resourceManager.loadImage(
        std::string("assets/textures/american_walnut_albedo.png"),
        Vapor::LoadMode::Async
    );
    auto normalResource = resourceManager.loadImage(
        std::string("assets/textures/american_walnut_normal.png"),
        Vapor::LoadMode::Async
    );
    auto roughnessResource = resourceManager.loadImage(
        std::string("assets/textures/american_walnut_roughness.png"),
        Vapor::LoadMode::Async
    );
    resourceManager.waitForAll();

    auto material = std::make_shared<Material>(Material {
        .albedoMap = albedoResource->get(),
        .normalMap = normalResource->get(),
        .roughnessMap = roughnessResource->get(),
    });
    fmt::print("[Gameplay] Textures loaded\n");

    // Create entities
    auto entity1 = g_memory->scene->createNode("Cube 1");
    g_memory->scene->addMeshToNode(entity1, MeshBuilder::buildCube(1.0f, material));
    entity1->setPosition(glm::vec3(-2.0f, 10.5f, 0.0f));
    entity1->body = g_memory->physics->createBoxBody(
        glm::vec3(.5f, .5f, .5f),
        glm::vec3(-2.0f, 0.5f, 0.0f),
        glm::identity<glm::quat>(),
        BodyMotionType::Dynamic
    );
    g_memory->physics->addBody(entity1->body, true);

    auto entity2 = g_memory->scene->createNode("Cube 2");
    g_memory->scene->addMeshToNode(entity2, MeshBuilder::buildCube(1.0f, material));
    entity2->setPosition(glm::vec3(2.0f, 0.5f, 0.0f));
    entity2->body = g_memory->physics->createBoxBody(
        glm::vec3(.5f, .5f, .5f),
        glm::vec3(2.0f, 0.5f, 0.0f),
        glm::identity<glm::quat>(),
        BodyMotionType::Dynamic
    );
    g_memory->physics->addBody(entity2->body, true);

    auto entity3 = g_memory->scene->createNode("Floor");
    entity3->setPosition(glm::vec3(0.0f, -0.5f, 0.0f));
    entity3->body = g_memory->physics->createBoxBody(
        glm::vec3(50.0f, .5f, 50.0f),
        glm::vec3(0.0f, -.5f, 0.0f),
        glm::identity<glm::quat>(),
        BodyMotionType::Static
    );
    g_memory->physics->addBody(entity3->body, false);

    // Stage scene for rendering
    g_memory->renderer->stage(g_memory->scene);

    // Rebind local node pointers
    rebindNodes();
}

void updateLights(float time) {
    if (!g_memory || !g_memory->scene) return;

    float speed = 0.5f;

    if (!g_memory->scene->directionalLights.empty()) {
        g_memory->scene->directionalLights[0].direction = glm::vec3(0.5, -1.0, 0.05 * sin(time * speed));
    }

    for (size_t i = 0; i < g_memory->scene->pointLights.size(); i++) {
        auto& l = g_memory->scene->pointLights[i];
        switch (i % 4) {
        case 0:  // circular motion
            l.position.x = 3.0f * cos(time * speed + i * 0.1f);
            l.position.z = 3.0f * sin(time * speed + i * 0.1f);
            l.position.y = 1.5f + 0.5f * sin(time * speed * 0.5f + i * 0.2f);
            break;

        case 1:  // figure-8 motion
            l.position.x = 4.0f * sin(time * speed * 0.7f + i * 0.15f);
            l.position.z = 4.0f * sin(time * speed * 0.7f + i * 0.15f) * cos(time * speed * 0.7f + i * 0.15f);
            l.position.y = 1.0f + 1.0f * cos(time * speed * 0.3f + i * 0.1f);
            break;

        case 2:  // linear motion
            l.position.x = 4.0f * sin(time * speed * 0.6f + i * 0.12f);
            l.position.z = 2.0f * cos(time * speed * 0.8f + i * 0.18f);
            l.position.y = 0.5f + 2.0f * abs(sin(time * speed * 0.4f + i * 0.14f));
            break;

        case 3: { // spiral motion
            float spiralRadius = 2.0f + 1.0f * sin(time * speed * 0.2f + i * 0.05f);
            l.position.x = spiralRadius * cos(time * speed * 0.5f + i * 0.08f);
            l.position.z = spiralRadius * sin(time * speed * 0.5f + i * 0.08f);
            l.position.y = 0.5f + 2.5f * (1.0f - cos(time * speed * 0.3f + i * 0.06f));
            break;
        }
        }
        l.intensity = 3.0f + 2.0f * (0.5f + 0.5f * sin(time * 0.3f + i * 0.1f));
    }
}

} // anonymous namespace

extern "C" {

GAME_API uint32_t game_get_version() {
    return Game::GAME_MODULE_API_VERSION;
}

GAME_API Game::GameMemory* game_memory() {
    return g_memory;
}

GAME_API bool game_init(Game::GameMemory* memory) {
    g_memory = memory;

    fmt::print("[Gameplay] game_init called, is_initialized={}\n", memory->is_initialized);

    if (!memory->is_initialized) {
        // First time initialization - load everything
        loadScene();
        setupCameras();

        memory->is_initialized = true;
        g_local.initialized = true;
        fmt::print("[Gameplay] First-time initialization complete\n");
    } else {
        // Hot reload - rebind pointers and rebuild local state
        fmt::print("[Gameplay] Hot reload detected\n");
        rebindNodes();
        setupCameras();
        g_local.initialized = true;
    }

    return true;
}

GAME_API bool game_update(Game::GameMemory* memory, const Game::FrameInput* input) {
    if (!memory || !input) return false;
    if (!g_local.initialized) return true;

    float dt = input->deltaTime;
    g_local.totalTime = input->totalTime;

    // Handle camera switching
    if (input->inputState) {
        const auto& inputState = *input->inputState;

        if (inputState.isPressed(Vapor::InputAction::Hotkey1)) {
            g_local.cameraManager.switchCamera("fly");
            fmt::print("[Gameplay] Switched to Fly Camera\n");
        }
        if (inputState.isPressed(Vapor::InputAction::Hotkey2)) {
            g_local.cameraManager.switchCamera("follow");
            fmt::print("[Gameplay] Switched to Follow Camera\n");
        }

        // Update camera
        g_local.cameraManager.update(dt, inputState);
    }

    // Update engine
    memory->engine->update(dt);

    // Rotate cube
    if (g_local.cube1) {
        g_local.cube1->rotate(glm::vec3(0.0f, 1.0f, -1.0f), 1.5f * dt);
    }

    // Update lights
    updateLights(g_local.totalTime);

    // Update scene and physics
    memory->scene->update(dt);
    memory->physics->process(memory->scene, dt);

    // Debug panel
    auto& resourceManager = memory->engine->getResourceManager();
    if (ImGui::Begin("Debug")) {
        if (ImGui::CollapsingHeader("Hot Reload", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Module: Loaded");
            ImGui::Text("Time: %.2f", g_local.totalTime);
            ImGui::Text("Frame: %u", g_local.frameCount);
        }
        if (ImGui::CollapsingHeader("Resources")) {
            ImGui::Text("Images: %zu", resourceManager.getImageCacheSize());
            ImGui::Text("Scenes: %zu", resourceManager.getSceneCacheSize());
            ImGui::Text("Meshes: %zu", resourceManager.getMeshCacheSize());
            if (ImGui::Button("Clear All Caches")) {
                resourceManager.clearAllCaches();
            }
        }
        if (ImGui::CollapsingHeader("Stats")) {
            ImGui::Text("Delta: %.3f ms", dt * 1000.0f);
            ImGui::Text("FPS: %.1f", 1.0f / dt);
        }
    }
    ImGui::End();

    // Render
    auto* currentCam = g_local.cameraManager.getCurrentCamera();
    if (currentCam && memory->renderer) {
        memory->renderer->draw(memory->scene, currentCam->getCamera());
    }

    g_local.frameCount++;
    return true;  // Return false to quit
}

GAME_API void game_shutdown(Game::GameMemory* memory) {
    fmt::print("[Gameplay] game_shutdown called\n");

    // Clear local state (will be rebuilt on reload)
    g_local = LocalState{};

    // Note: Don't clear g_memory or set is_initialized to false
    // The memory should persist for hot reload
}

} // extern "C"
