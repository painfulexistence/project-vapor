#pragma once
#include <optional>
#include <string>
#include <vector>

class FileSystem {
public:
    static FileSystem& instance();

    // Initialize default search paths from executable directory.
    // Order (highest priority first): ResHR (debug only), ResPatch, Res, ResDLC.
    void initialize();

    void addSearchPath(std::string absolutePath, int priority);
    void removeSearchPath(const std::string& absolutePath);

    // Returns the first resolved absolute path where relativePath exists,
    // or std::nullopt if not found in any search path.
    std::optional<std::string> resolvePath(const std::string& relativePath) const;

    // Like resolvePath but throws std::runtime_error if not found.
    std::string resolvePathOrThrow(const std::string& relativePath) const;

private:
    struct SearchEntry {
        std::string absolutePath;
        int priority;
    };
    std::vector<SearchEntry> m_paths; // sorted ascending by priority (lower = searched first)
};
