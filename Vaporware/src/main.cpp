#include <SDL3/SDL.h>
#include <fmt/core.h>
#include <args.hxx>
#include <iostream>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"

#include "Vapor/renderer.hpp"
#include "Vapor/graphics.hpp"
#include "Vapor/physics_3d.hpp"
#include "Vapor/engine_core.hpp"

#include "game.hpp"

#ifdef HOT_RELOAD
    #include <filesystem>
    #include <chrono>

    // Platform-specific shared library extension
    #ifdef _WIN32
        #define LIB_EXT ".dll"
    #elif defined(__APPLE__)
        #define LIB_EXT ".dylib"
    #else
        #define LIB_EXT ".so"
    #endif

    class GameModule {
    public:
        bool load(const std::string& path) {
            std::string fullPath = path + LIB_EXT;
            std::string tempPath = copyToTemp(fullPath);
            if (tempPath.empty()) return false;

            m_handle = SDL_LoadObject(tempPath.c_str());
            if (!m_handle) {
                fmt::print("[Hot] Load failed: {}\n", SDL_GetError());
                return false;
            }

            m_init = (GameInitFunc)SDL_LoadFunction(m_handle, "game_init");
            m_update = (GameUpdateFunc)SDL_LoadFunction(m_handle, "game_update");
            m_shutdown = (GameShutdownFunc)SDL_LoadFunction(m_handle, "game_shutdown");
            m_getVersion = (GameGetVersionFunc)SDL_LoadFunction(m_handle, "game_get_version");
            m_getMemory = (GameMemoryPtrFunc)SDL_LoadFunction(m_handle, "game_memory");

            if (!m_init || !m_update || !m_shutdown || !m_getVersion || !m_getMemory) {
                fmt::print("[Hot] Missing exports\n");
                SDL_UnloadObject(m_handle);
                m_handle = nullptr;
                return false;
            }

            if (m_getVersion() != GAME_API_VERSION) {
                fmt::print("[Hot] Version mismatch\n");
                SDL_UnloadObject(m_handle);
                m_handle = nullptr;
                return false;
            }

            m_path = fullPath;
            m_lastModified = std::filesystem::last_write_time(fullPath);
            fmt::print("[Hot] Loaded: {}\n", fullPath);
            return true;
        }

        void unload() {
            if (m_handle) {
                SDL_UnloadObject(m_handle);
                m_handle = nullptr;
            }
        }

        bool reload() {
            GameMemory* mem = m_getMemory ? m_getMemory() : nullptr;
            if (m_shutdown && mem) m_shutdown(mem);

            std::string path = m_path.substr(0, m_path.length() - strlen(LIB_EXT));
            unload();
            SDL_Delay(100);

            if (!load(path)) return false;
            if (m_init && mem) m_init(mem);

            fmt::print("[Hot] Reload successful\n");
            return true;
        }

        bool hasChanged() const {
            if (m_path.empty()) return false;
            try {
                return std::filesystem::last_write_time(m_path) > m_lastModified;
            } catch (...) { return false; }
        }

        GameInitFunc init() const { return m_init; }
        GameUpdateFunc update() const { return m_update; }
        GameShutdownFunc shutdown() const { return m_shutdown; }
        GameMemoryPtrFunc memory() const { return m_getMemory; }
        bool loaded() const { return m_handle != nullptr; }

    private:
        SDL_SharedObject* m_handle = nullptr;
        std::string m_path;
        std::filesystem::file_time_type m_lastModified;
        GameInitFunc m_init = nullptr;
        GameUpdateFunc m_update = nullptr;
        GameShutdownFunc m_shutdown = nullptr;
        GameGetVersionFunc m_getVersion = nullptr;
        GameMemoryPtrFunc m_getMemory = nullptr;

        std::string copyToTemp(const std::string& src) {
            try {
                auto ts = std::chrono::system_clock::now().time_since_epoch().count();
                std::string dst = (std::filesystem::temp_directory_path() / fmt::format("game_{}{}", ts, LIB_EXT)).string();
                std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
                return dst;
            } catch (const std::exception& e) {
                fmt::print("[Hot] Copy failed: {}\n", e.what());
                return "";
            }
        }
    };

    std::string getModulePath() {
        #ifdef _WIN32
            return "./Game";
        #else
            return "./libGame";
        #endif
    }
#else
    // Direct mode - no DLL
    static Game g_game;
#endif

int main(int argc, char* args[]) {
    args::ArgumentParser parser{"Project Vapor"};
    args::ValueFlag<Uint32> width(parser, "w", "Window width", {'w'}, 1280);
    args::ValueFlag<Uint32> height(parser, "h", "Window height", {'h'}, 720);
    args::Flag useMetal(parser, "metal", "Use Metal backend", {"metal"});
    args::Flag useVulkan(parser, "vulkan", "Use Vulkan backend", {"vulkan"});
    args::HelpFlag help(parser, "help", "Help", {"help"});

    if (argc > 1) {
        try { parser.ParseCLI(argc, args); }
        catch (args::Help) { std::cout << parser; return 0; }
        catch (args::Error& e) { std::cerr << e.what() << "\n"; return 1; }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fmt::print("SDL init failed: {}\n", SDL_GetError());
        return 1;
    }

    const char* title;
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    GraphicsBackend backend;

#if defined(__APPLE__)
    if (useVulkan) {
        title = "Project Vapor (Vulkan)";
        flags |= SDL_WINDOW_VULKAN;
        backend = GraphicsBackend::Vulkan;
    } else {
        title = "Project Vapor (Metal)";
        flags |= SDL_WINDOW_METAL;
        backend = GraphicsBackend::Metal;
    }
#else
    title = "Project Vapor (Vulkan)";
    flags |= SDL_WINDOW_VULKAN;
    backend = GraphicsBackend::Vulkan;
#endif

    auto window = SDL_CreateWindow(title, width.Get(), height.Get(), flags);
    if (!window) {
        fmt::print("Window creation failed: {}\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    auto engineCore = std::make_unique<Vapor::EngineCore>();
    engineCore->init();

    auto renderer = createRenderer(backend);
    renderer->init(window);

    auto physics = std::make_unique<Physics3D>();
    physics->init(engineCore->getTaskScheduler());

    // Setup game memory
    GameMemory memory;
    memory.window = window;
    memory.renderer = renderer.get();
    memory.physics = physics.get();
    memory.engine = engineCore.get();

#ifdef HOT_RELOAD
    GameModule module;
    if (!module.load(getModulePath())) {
        fmt::print("Failed to load game module\n");
        return 1;
    }
    if (!module.init()(&memory)) {
        fmt::print("Game init failed\n");
        return 1;
    }
#else
    if (!g_game.init(&memory)) {
        fmt::print("Game init failed\n");
        return 1;
    }
#endif

    auto& inputManager = engineCore->getInputManager();
    float time = SDL_GetTicks() / 1000.0f;
    bool quit = false;

    while (!quit) {
        float now = SDL_GetTicks() / 1000.0f;
        float dt = now - time;
        time = now;

        inputManager.update(dt);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            inputManager.processEvent(e);

            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
#ifdef HOT_RELOAD
                if (e.key.scancode == SDL_SCANCODE_F5) {
                    fmt::print("[Main] F5 - Hot reload\n");
                    module.reload();
                }
#endif
            }
        }

        FrameInput input{dt, time, &inputManager.getInputState()};

#ifdef HOT_RELOAD
        if (!module.update()(&memory, &input)) quit = true;
#else
        if (!g_game.update(&memory, &input)) quit = true;
#endif
    }

#ifdef HOT_RELOAD
    if (module.loaded()) {
        module.shutdown()(module.memory()());
        module.unload();
    }
#else
    g_game.shutdown(&memory);
#endif

    renderer->deinit();
    physics->deinit();
    engineCore->shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
