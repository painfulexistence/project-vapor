// Thin wrapper for C-style DLL exports
// Only compiled when building the game as a shared library (HOT_RELOAD mode)

#include "game.hpp"

#ifdef _WIN32
    #define EXPORT __declspec(dllexport)
#else
    #define EXPORT __attribute__((visibility("default")))
#endif

namespace {
    Game g_game;
    GameMemory* g_memoryPtr = nullptr;
}

extern "C" {

EXPORT uint32_t game_get_version() {
    return GAME_API_VERSION;
}

EXPORT GameMemory* game_memory() {
    return g_memoryPtr;
}

EXPORT bool game_init(GameMemory* memory) {
    g_memoryPtr = memory;
    return g_game.init(memory);
}

EXPORT bool game_update(GameMemory* memory, const FrameInput* input) {
    return g_game.update(memory, input);
}

EXPORT void game_shutdown(GameMemory* memory) {
    g_game.shutdown(memory);
}

} // extern "C"
