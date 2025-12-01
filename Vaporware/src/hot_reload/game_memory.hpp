#pragma once

#include <Vapor/scene.hpp>
#include <Vapor/physics_3d.hpp>
#include <Vapor/renderer.hpp>
#include <Vapor/input_manager.hpp>
#include <Vapor/engine_core.hpp>
#include <cstdint>

namespace Game {

// Game state that persists across hot reloads
// Add your game-specific state here
struct GameState {
    float gameTime = 0.0f;
    int score = 0;
    bool isPaused = false;

    // Add more game state as needed...
};

// Memory block shared between host and gameplay DLL
struct GameMemory {
    // A. Is memory initialized?
    // Used by gameplay DLL to determine if it's a first launch or a hot reload
    bool is_initialized = false;

    // B. Global game state (survives hot reload)
    GameState state;

    // C. Engine services (owned by host, passed to DLL)
    // These pointers remain valid across hot reloads
    Scene* scene = nullptr;
    Physics3D* physics = nullptr;
    Renderer* renderer = nullptr;
    Vapor::InputManager* input = nullptr;
    Vapor::EngineCore* engine = nullptr;

    // D. Temp memory allocator (optional)
    // Linear allocator for per-frame allocations, avoids malloc/free
    size_t temp_storage_size = 0;
    void* temp_storage_buffer = nullptr;
};

// Input state passed each frame
struct FrameInput {
    float deltaTime;
    const Vapor::InputState* inputState;
};

// Module API version for compatibility checking
constexpr uint32_t GAME_MODULE_API_VERSION = 1;

// Functions that the gameplay DLL must export
extern "C" {
    // Called once when module is first loaded
    // Return false if initialization fails
    using GameInitFunc = bool (*)(GameMemory* memory);

    // Called every frame
    using GameUpdateFunc = void (*)(GameMemory* memory, const FrameInput* input);

    // Called when module is about to be unloaded (before hot reload)
    using GameShutdownFunc = void (*)(GameMemory* memory);

    // Return the API version this module was compiled with
    using GameGetVersionFunc = uint32_t (*)();
}

// Export function names (for dlsym/GetProcAddress)
#define GAME_INIT_FUNC_NAME "game_init"
#define GAME_UPDATE_FUNC_NAME "game_update"
#define GAME_SHUTDOWN_FUNC_NAME "game_shutdown"
#define GAME_GET_VERSION_FUNC_NAME "game_get_version"

} // namespace Game
