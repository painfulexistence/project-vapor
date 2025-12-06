#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>

#include "scene.hpp"
#include "world.hpp"

namespace Vapor {

enum class LoadMode {
    Replace,   // Replace active scene
    Additive   // Append to active scene
};

class SceneManager {
public:
    explicit SceneManager(World* world);
    ~SceneManager() = default;

    // Non-copyable
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    // Load a scene (from AssetManager::loadGLTF result)
    // Replace: set as new active scene
    // Additive: append to current active scene
    // Returns the scene itself for chaining
    std::shared_ptr<Scene> load(std::shared_ptr<Scene> scene, LoadMode mode = LoadMode::Replace);

    // Load with specific parent node (always additive)
    std::shared_ptr<Scene> load(std::shared_ptr<Scene> scene, std::shared_ptr<Node> parent);

    // Unload a scene by ID
    void unload(SceneID id);

    // Unload all scenes
    void unloadAll();

    // Get scene by ID
    std::shared_ptr<Scene> getScene(SceneID id) const;

    // Check if scene is loaded
    bool isLoaded(SceneID id) const;

    // Get all active scene IDs
    std::vector<SceneID> getActiveSceneIds() const;

    // Get the main/active scene
    std::shared_ptr<Scene> getActiveScene() const;

private:
    SceneID generateSceneId();
    void registerSceneNodes(std::shared_ptr<Scene> scene, SceneID id);

    World* world;
    std::unordered_map<SceneID, std::shared_ptr<Scene>> scenes;
    SceneID nextSceneId = 1;
    SceneID activeSceneId = InvalidSceneID;
};

} // namespace Vapor
