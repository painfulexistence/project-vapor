#include "../src/hot_reload/game_memory.hpp"
#include <Vapor/scene.hpp>
#include <Vapor/physics_3d.hpp>
#include <Vapor/input_manager.hpp>
#include <fmt/core.h>
#include <glm/gtc/matrix_transform.hpp>

// Platform-specific export macro
#ifdef _WIN32
    #define GAME_API __declspec(dllexport)
#else
    #define GAME_API __attribute__((visibility("default")))
#endif

namespace {

// Local state that gets rebuilt on hot reload (not persisted)
struct LocalState {
    Node* playerNode = nullptr;
    Node* cube1 = nullptr;
    Node* cube2 = nullptr;
};

LocalState g_local;

// Helper to rebind node pointers after hot reload
void rebindNodes(Game::GameMemory* memory) {
    if (!memory->scene) return;

    g_local.cube1 = memory->scene->findNode("Cube 1").get();
    g_local.cube2 = memory->scene->findNode("Cube 2").get();

    fmt::print("[Gameplay] Nodes rebound: cube1={}, cube2={}\n",
               g_local.cube1 ? "found" : "null",
               g_local.cube2 ? "found" : "null");
}

} // anonymous namespace

extern "C" {

GAME_API uint32_t game_get_version() {
    return Game::GAME_MODULE_API_VERSION;
}

GAME_API bool game_init(Game::GameMemory* memory) {
    fmt::print("[Gameplay] game_init called, is_initialized={}\n", memory->is_initialized);

    if (!memory->is_initialized) {
        // First time initialization
        memory->state.gameTime = 0.0f;
        memory->state.score = 0;
        memory->state.isPaused = false;

        memory->is_initialized = true;
        fmt::print("[Gameplay] First-time initialization complete\n");
    } else {
        // Hot reload - rebind pointers
        fmt::print("[Gameplay] Hot reload detected, rebinding nodes...\n");
    }

    // Always rebind node pointers (they may have changed)
    rebindNodes(memory);

    return true;
}

GAME_API void game_update(Game::GameMemory* memory, const Game::FrameInput* input) {
    if (!memory || !input) return;
    if (memory->state.isPaused) return;

    float dt = input->deltaTime;
    memory->state.gameTime += dt;

    // Example gameplay logic: rotate cube1
    if (g_local.cube1) {
        g_local.cube1->rotate(glm::vec3(0.0f, 1.0f, -1.0f), 1.5f * dt);
    }

    // Example: respond to input
    if (input->inputState) {
        const auto& inputState = *input->inputState;

        // Press Jump to add score
        if (inputState.isPressed(Vapor::InputAction::Jump)) {
            memory->state.score += 10;
            fmt::print("[Gameplay] Score: {}\n", memory->state.score);
        }

        // Press Cancel to toggle pause
        if (inputState.isPressed(Vapor::InputAction::Cancel)) {
            memory->state.isPaused = !memory->state.isPaused;
            fmt::print("[Gameplay] Paused: {}\n", memory->state.isPaused);
        }
    }
}

GAME_API void game_shutdown(Game::GameMemory* memory) {
    fmt::print("[Gameplay] game_shutdown called\n");

    // Clear local state (will be rebuilt on reload)
    g_local = LocalState{};

    // Note: Don't set is_initialized to false here!
    // The state should persist across hot reloads.
}

} // extern "C"
