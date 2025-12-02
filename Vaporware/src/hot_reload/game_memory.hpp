#pragma once

#include <Vapor/scene.hpp>
#include <Vapor/physics_3d.hpp>
#include <Vapor/renderer.hpp>
#include <Vapor/input_manager.hpp>
#include <Vapor/engine_core.hpp>
#include <cstdint>

struct SDL_Window;

namespace Game {

// Memory block shared between host and gameplay DLL
// The gameplay DLL owns all game state; host only provides engine services
struct GameMemory {
    // Is memory initialized? Used to detect hot reload vs first launch
    bool is_initialized = false;

    // Engine services (owned by host, provided to DLL)
    SDL_Window* window = nullptr;
    Renderer* renderer = nullptr;
    Physics3D* physics = nullptr;
    Vapor::EngineCore* engine = nullptr;

    // Game-owned state (created and managed by DLL)
    // These are allocated by game_init and freed by game_shutdown
    std::shared_ptr<Scene> scene;

    // Temp storage for per-frame allocations (optional)
    size_t temp_storage_size = 0;
    void* temp_storage = nullptr;
};

// Input state passed each frame
struct FrameInput {
    float deltaTime;
    float totalTime;
    const Vapor::InputState* inputState;
};

// Module API version for compatibility checking
constexpr uint32_t GAME_MODULE_API_VERSION = 1;

// Functions that the gameplay DLL must export
extern "C" {
    // Called once when module is first loaded, or after hot reload
    // On first load: allocate GameMemory, create scene, entities, etc.
    // On hot reload: rebind pointers, reinitialize local state
    using GameInitFunc = bool (*)(GameMemory* memory);

    // Called every frame - all game logic goes here
    using GameUpdateFunc = bool (*)(GameMemory* memory, const FrameInput* input);

    // Called when module is about to be unloaded
    // On hot reload: just cleanup local state (memory persists)
    // On shutdown: free all game memory
    using GameShutdownFunc = void (*)(GameMemory* memory);

    // Return the API version this module was compiled with
    using GameGetVersionFunc = uint32_t (*)();

    // Return pointer to game memory (for hot reload transfer)
    using GameMemoryFunc = GameMemory* (*)();
}

// Export function names
#define GAME_INIT_FUNC_NAME "game_init"
#define GAME_UPDATE_FUNC_NAME "game_update"
#define GAME_SHUTDOWN_FUNC_NAME "game_shutdown"
#define GAME_GET_VERSION_FUNC_NAME "game_get_version"
#define GAME_MEMORY_FUNC_NAME "game_memory"

} // namespace Game
