#pragma once

#include "game_memory.hpp"
#include <string>
#include <filesystem>
#include <fmt/core.h>

#ifdef _WIN32
    #include <windows.h>
    using ModuleHandle = HMODULE;
    #define MODULE_EXTENSION ".dll"
#else
    #include <dlfcn.h>
    using ModuleHandle = void*;
    #ifdef __APPLE__
        #define MODULE_EXTENSION ".dylib"
    #else
        #define MODULE_EXTENSION ".so"
    #endif
#endif

namespace Game {

class ModuleLoader {
public:
    ModuleLoader() = default;
    ~ModuleLoader() { unload(); }

    // Load the gameplay module from the given path
    // Path should be without extension (e.g., "./gameplay/libGameplay")
    bool load(const std::string& basePath) {
        std::string fullPath = basePath + MODULE_EXTENSION;

        // Copy to temp file to avoid file locking issues (Windows)
        std::string loadPath = copyToTemp(fullPath);
        if (loadPath.empty()) {
            m_lastError = "Failed to copy module to temp location";
            return false;
        }

        // Load the library
        #ifdef _WIN32
            m_handle = LoadLibraryA(loadPath.c_str());
        #else
            m_handle = dlopen(loadPath.c_str(), RTLD_NOW);
        #endif

        if (!m_handle) {
            m_lastError = getSystemError();
            return false;
        }

        // Get function pointers
        m_initFunc = getSymbol<GameInitFunc>(GAME_INIT_FUNC_NAME);
        m_updateFunc = getSymbol<GameUpdateFunc>(GAME_UPDATE_FUNC_NAME);
        m_shutdownFunc = getSymbol<GameShutdownFunc>(GAME_SHUTDOWN_FUNC_NAME);
        m_getVersionFunc = getSymbol<GameGetVersionFunc>(GAME_GET_VERSION_FUNC_NAME);

        if (!m_initFunc || !m_updateFunc || !m_shutdownFunc || !m_getVersionFunc) {
            m_lastError = "Missing required exports: " + getMissingExports();
            unloadLibrary();
            return false;
        }

        // Version check
        uint32_t moduleVersion = m_getVersionFunc();
        if (moduleVersion != GAME_MODULE_API_VERSION) {
            m_lastError = fmt::format("API version mismatch: host={}, module={}",
                                      GAME_MODULE_API_VERSION, moduleVersion);
            unloadLibrary();
            return false;
        }

        m_modulePath = fullPath;
        updateModifyTime();
        m_isLoaded = true;

        fmt::print("[ModuleLoader] Loaded: {}\n", fullPath);
        return true;
    }

    void unload() {
        if (m_handle) {
            unloadLibrary();
            m_handle = nullptr;
            m_initFunc = nullptr;
            m_updateFunc = nullptr;
            m_shutdownFunc = nullptr;
            m_getVersionFunc = nullptr;
            m_isLoaded = false;
            fmt::print("[ModuleLoader] Unloaded module\n");
        }
    }

    // Check if the module file has been modified since last load
    bool hasFileChanged() const {
        if (m_modulePath.empty()) return false;

        try {
            auto currentTime = std::filesystem::last_write_time(m_modulePath);
            return currentTime > m_lastModifyTime;
        } catch (...) {
            return false;
        }
    }

    // Reload the module (call shutdown, unload, load, init)
    bool reload(GameMemory* memory) {
        if (!m_isLoaded) return false;

        std::string path = m_modulePath;
        // Remove extension for reload
        path = path.substr(0, path.length() - strlen(MODULE_EXTENSION));

        // Shutdown current module
        if (m_shutdownFunc && memory) {
            m_shutdownFunc(memory);
        }

        // Unload
        unload();

        // Small delay to ensure file is released (Windows)
        #ifdef _WIN32
            Sleep(100);
        #endif

        // Load new version
        if (!load(path)) {
            return false;
        }

        // Initialize (is_initialized should still be true for hot reload)
        if (m_initFunc && memory) {
            if (!m_initFunc(memory)) {
                m_lastError = "Module init failed after reload";
                return false;
            }
        }

        fmt::print("[ModuleLoader] Hot reload successful\n");
        return true;
    }

    // Accessors
    bool isLoaded() const { return m_isLoaded; }
    const std::string& getLastError() const { return m_lastError; }
    const std::string& getModulePath() const { return m_modulePath; }

    // Function pointers
    GameInitFunc getInitFunc() const { return m_initFunc; }
    GameUpdateFunc getUpdateFunc() const { return m_updateFunc; }
    GameShutdownFunc getShutdownFunc() const { return m_shutdownFunc; }

private:
    ModuleHandle m_handle = nullptr;
    bool m_isLoaded = false;
    std::string m_modulePath;
    std::string m_lastError;
    std::filesystem::file_time_type m_lastModifyTime;

    GameInitFunc m_initFunc = nullptr;
    GameUpdateFunc m_updateFunc = nullptr;
    GameShutdownFunc m_shutdownFunc = nullptr;
    GameGetVersionFunc m_getVersionFunc = nullptr;

    template<typename T>
    T getSymbol(const char* name) {
        #ifdef _WIN32
            return reinterpret_cast<T>(GetProcAddress(m_handle, name));
        #else
            return reinterpret_cast<T>(dlsym(m_handle, name));
        #endif
    }

    void unloadLibrary() {
        if (m_handle) {
            #ifdef _WIN32
                FreeLibrary(m_handle);
            #else
                dlclose(m_handle);
            #endif
        }
    }

    void updateModifyTime() {
        try {
            m_lastModifyTime = std::filesystem::last_write_time(m_modulePath);
        } catch (...) {}
    }

    std::string getSystemError() {
        #ifdef _WIN32
            DWORD error = GetLastError();
            char* msg = nullptr;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                          nullptr, error, 0, (LPSTR)&msg, 0, nullptr);
            std::string result = msg ? msg : "Unknown error";
            LocalFree(msg);
            return result;
        #else
            const char* err = dlerror();
            return err ? err : "Unknown error";
        #endif
    }

    std::string getMissingExports() {
        std::string missing;
        if (!m_initFunc) missing += GAME_INIT_FUNC_NAME " ";
        if (!m_updateFunc) missing += GAME_UPDATE_FUNC_NAME " ";
        if (!m_shutdownFunc) missing += GAME_SHUTDOWN_FUNC_NAME " ";
        if (!m_getVersionFunc) missing += GAME_GET_VERSION_FUNC_NAME " ";
        return missing;
    }

    std::string copyToTemp(const std::string& srcPath) {
        try {
            // Generate unique temp path
            auto tempDir = std::filesystem::temp_directory_path();
            auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            std::string tempPath = (tempDir / fmt::format("gameplay_{}{}", timestamp, MODULE_EXTENSION)).string();

            std::filesystem::copy_file(srcPath, tempPath,
                                       std::filesystem::copy_options::overwrite_existing);
            return tempPath;
        } catch (const std::exception& e) {
            fmt::print("[ModuleLoader] Copy to temp failed: {}\n", e.what());
            return "";
        }
    }
};

} // namespace Game
