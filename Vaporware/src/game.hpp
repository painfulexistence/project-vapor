#pragma once

#include <Vapor/scene.hpp>
#include <Vapor/physics_3d.hpp>
#include <Vapor/renderer.hpp>
#include <Vapor/input_manager.hpp>
#include <Vapor/engine_core.hpp>
#include <cstdint>

struct SDL_Window;

// Memory shared between host and game
struct GameMemory {
    bool is_initialized = false;

    // Engine services (owned by host)
    SDL_Window* window = nullptr;
    Renderer* renderer = nullptr;
    Physics3D* physics = nullptr;
    Vapor::EngineCore* engine = nullptr;

    // Game-owned state
    std::shared_ptr<Scene> scene;
};

// Per-frame input
struct FrameInput {
    float deltaTime;
    float totalTime;
    const Vapor::InputState* inputState;
};

// Game class - all gameplay logic lives here
class Game {
public:
    bool init(GameMemory* memory);
    bool update(GameMemory* memory, const FrameInput* input);
    void shutdown(GameMemory* memory);

private:
    void loadScene();
    void setupCameras();
    void rebindNodes();
    void updateLights(float time);
};

// API version for hot reload compatibility check
constexpr uint32_t GAME_API_VERSION = 1;

// C-style function signatures (for DLL export)
extern "C" {
    using GameInitFunc = bool (*)(GameMemory*);
    using GameUpdateFunc = bool (*)(GameMemory*, const FrameInput*);
    using GameShutdownFunc = void (*)(GameMemory*);
    using GameGetVersionFunc = uint32_t (*)();
    using GameMemoryPtrFunc = GameMemory* (*)();
}
