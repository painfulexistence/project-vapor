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
    std::string base = SDL_GetBasePath();
#ifndef NDEBUG
    addSearchPath(base + "ResHR",    0);
#endif
    addSearchPath(base + "ResPatch", 1);
    addSearchPath(base + "Res",      2);
    addSearchPath(base + "ResDLC",   3);
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
