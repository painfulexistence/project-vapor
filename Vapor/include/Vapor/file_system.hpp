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

    // Enumerate files under a relative directory across ALL search paths,
    // returned as sorted, de-duplicated relative paths (e.g.
    // "models/Sponza/Sponza.gltf") ready to hand straight back to resolvePath /
    // AssetManager::loadModel. `extensions` filters by a ';'-separated,
    // case-insensitive list of extensions WITHOUT dots (e.g. "usd;usda"); ""
    // lists every file. `maxDepth` bounds recursion: 0 = the directory itself
    // (flat); 1 also includes files one sub-directory down (a model folder's
    // entry file) without descending into that folder's own sub-trees; N
    // descends N levels; a negative value is unlimited. Shallow keeps a picker
    // from being flooded by a self-contained asset package's referenced parts.
    [[nodiscard]] std::vector<std::string>
        list(const std::string& relativeDir, const std::string& extensions = "", int maxDepth = 0) const;

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
