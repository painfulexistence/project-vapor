#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>

#include "scene.hpp"
#include "world.hpp"

namespace Vapor {

class SceneManager {
public:
    explicit SceneManager(World* world);
    ~SceneManager() = default;

    // Non-copyable
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    // Load a GLTF file and create a scene
    // Returns the SceneID of the loaded scene
    SceneID load(const std::string& path, bool optimized = true);

    // Load a GLTF file asynchronously
    void loadAsync(const std::string& path,
                   std::function<void(SceneID)> onComplete,
                   bool optimized = true);

    // Create an empty scene
    SceneID createScene(const std::string& name = "");

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

    // Get the main/active scene (first loaded scene by default)
    std::shared_ptr<Scene> getActiveScene() const;
    void setActiveScene(SceneID id);

private:
    SceneID generateSceneId();
    void registerSceneNodes(std::shared_ptr<Scene> scene, SceneID id);

    World* world;
    std::unordered_map<SceneID, std::shared_ptr<Scene>> scenes;
    SceneID nextSceneId = 1;
    SceneID activeSceneId = InvalidSceneID;
};

} // namespace Vapor
