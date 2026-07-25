#include "Vapor/file_system.hpp"

#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fmt/core.h>
#include <stdexcept>
#include <unordered_set>

using namespace Vapor;

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

std::vector<std::string>
    FileSystem::list(const std::string& relativeDir, const std::string& extensions, int maxDepth) const {
    namespace fs = std::filesystem;
    const_cast<FileSystem*>(this)->lazyInitialize();

    // Parse the ';'/','-separated extension filter into a lowercase set (no dots).
    std::vector<std::string> exts;
    {
        std::string cur;
        for (char c : extensions) {
            if (c == ';' || c == ',') {
                if (!cur.empty()) exts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (!cur.empty()) exts.push_back(cur);
    }
    auto matches = [&](const std::string& name) {
        if (exts.empty()) return true;
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string e = name.substr(dot + 1);
        for (char& c : e)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return std::find(exts.begin(), exts.end(), e) != exts.end();
    };

    // Merge across every search path (so DLC/patch models list too), de-duped by
    // the relative path — the same key resolvePath / loadModel expect back.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& entry : m_paths) {
        const fs::path rootDir = fs::path(entry.absolutePath) / relativeDir;
        std::error_code ec;
        if (!fs::is_directory(rootDir, ec)) continue;
        fs::recursive_directory_iterator it(rootDir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code dec;
            if (maxDepth >= 0 && it.depth() >= maxDepth && it->is_directory(dec)) it.disable_recursion_pending();
            if (!it->is_regular_file(dec) || dec) continue;
            if (!matches(it->path().filename().string())) continue;
            // Relative to the search-path root: "<relativeDir>/<subpath>",
            // forward-slashed so it round-trips through resolvePath on Windows.
            const std::string rel =
                (fs::path(relativeDir) / it->path().lexically_relative(rootDir)).generic_string();
            if (seen.insert(rel).second) out.push_back(rel);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
