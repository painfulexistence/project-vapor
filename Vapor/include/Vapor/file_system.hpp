#pragma once
#include <optional>
#include <string>
#include <vector>

namespace Vapor {

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
    [[nodiscard]] std::optional<std::string> resolvePath(const std::string& relativePath) const;

    // Like resolvePath but throws std::runtime_error if not found.
    [[nodiscard]] std::string resolvePathOrThrow(const std::string& relativePath) const;

private:
    struct SearchEntry {
        std::string absolutePath;
        int priority;
    };
    std::vector<SearchEntry> m_paths; // sorted ascending by priority (lower = searched first)
    bool m_initialized = false;

    // Called automatically by resolvePath if initialize() was never called.
    void lazyInitialize();
};

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
