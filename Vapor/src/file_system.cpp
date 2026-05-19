#include "Vapor/file_system.hpp"

#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <filesystem>
#include <fmt/core.h>
#include <stdexcept>

FileSystem& FileSystem::instance() {
    static FileSystem fs;
    return fs;
}

void FileSystem::initialize() {
    if (m_initialized) return;
    const char* baseCStr = SDL_GetBasePath();
    if (!baseCStr) {
        fmt::print(stderr, "[FileSystem] SDL_GetBasePath() returned null; no search paths configured.\n");
        m_initialized = true;
        return;
    }
    std::string base = baseCStr;
#ifndef NDEBUG
    addSearchPath(base + "ResHR",    0);
#endif
    addSearchPath(base + "ResPatch", 1);
    addSearchPath(base + "Res",      2);
    addSearchPath(base + "ResDLC",   3);
    m_initialized = true;
}

void FileSystem::lazyInitialize() {
    if (!m_initialized) initialize();
}

void FileSystem::addSearchPath(std::string absolutePath, int priority) {
    m_paths.push_back({ std::move(absolutePath), priority });
    std::stable_sort(m_paths.begin(), m_paths.end(), [](const SearchEntry& a, const SearchEntry& b) {
        return a.priority < b.priority;
    });
}

void FileSystem::removeSearchPath(const std::string& absolutePath) {
    m_paths.erase(
        std::remove_if(m_paths.begin(), m_paths.end(),
            [&](const SearchEntry& e) { return e.absolutePath == absolutePath; }),
        m_paths.end()
    );
}

std::optional<std::string> FileSystem::resolvePath(const std::string& relativePath) const {
    const_cast<FileSystem*>(this)->lazyInitialize();
    for (const auto& entry : m_paths) {
        std::filesystem::path full = std::filesystem::path(entry.absolutePath) / relativePath;
        if (std::filesystem::exists(full)) {
            return full.string();
        }
    }
    return std::nullopt;
}

std::string FileSystem::resolvePathOrThrow(const std::string& relativePath) const {
    auto result = resolvePath(relativePath);
    if (!result) {
        throw std::runtime_error(fmt::format("Asset not found in any search path: {}", relativePath));
    }
    return *result;
}
