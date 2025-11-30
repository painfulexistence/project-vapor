#include "Vapor/scene_manager.hpp"
#include "Vapor/asset_manager.hpp"
#include "Vapor/resource_manager.hpp"

namespace Vapor {

SceneManager::SceneManager(World* world) : world(world) {}

SceneID SceneManager::load(const std::string& path, bool optimized) {
    std::shared_ptr<Scene> scene;
    if (optimized) {
        scene = AssetManager::loadGLTFOptimized(path);
    } else {
        scene = AssetManager::loadGLTF(path);
    }

    if (!scene) {
        return InvalidSceneID;
    }

    SceneID id = generateSceneId();
    scene->sceneId = id;

    // Register all nodes to World
    registerSceneNodes(scene, id);

    scenes[id] = scene;

    // Set as active scene if this is the first one
    if (activeSceneId == InvalidSceneID) {
        activeSceneId = id;
    }

    return id;
}

void SceneManager::loadAsync(const std::string& path,
                              std::function<void(SceneID)> onComplete,
                              bool optimized) {
    // For now, just do synchronous loading
    // TODO: Integrate with ResourceManager for true async loading
    SceneID id = load(path, optimized);
    if (onComplete) {
        onComplete(id);
    }
}

SceneID SceneManager::createScene(const std::string& name) {
    auto scene = std::make_shared<Scene>(name);
    SceneID id = generateSceneId();
    scene->sceneId = id;

    scenes[id] = scene;

    if (activeSceneId == InvalidSceneID) {
        activeSceneId = id;
    }

    return id;
}

void SceneManager::unload(SceneID id) {
    auto it = scenes.find(id);
    if (it == scenes.end()) {
        return;
    }

    // Unregister all nodes from World
    world->unregisterScene(id);

    scenes.erase(it);

    // Update active scene if needed
    if (activeSceneId == id) {
        if (!scenes.empty()) {
            activeSceneId = scenes.begin()->first;
        } else {
            activeSceneId = InvalidSceneID;
        }
    }
}

void SceneManager::unloadAll() {
    for (auto& [id, scene] : scenes) {
        world->unregisterScene(id);
    }
    scenes.clear();
    activeSceneId = InvalidSceneID;
}

std::shared_ptr<Scene> SceneManager::getScene(SceneID id) const {
    auto it = scenes.find(id);
    if (it != scenes.end()) {
        return it->second;
    }
    return nullptr;
}

bool SceneManager::isLoaded(SceneID id) const {
    return scenes.find(id) != scenes.end();
}

std::vector<SceneID> SceneManager::getActiveSceneIds() const {
    std::vector<SceneID> ids;
    ids.reserve(scenes.size());
    for (const auto& [id, scene] : scenes) {
        ids.push_back(id);
    }
    return ids;
}

std::shared_ptr<Scene> SceneManager::getActiveScene() const {
    return getScene(activeSceneId);
}

void SceneManager::setActiveScene(SceneID id) {
    if (isLoaded(id)) {
        activeSceneId = id;
    }
}

SceneID SceneManager::generateSceneId() {
    return nextSceneId++;
}

void SceneManager::registerSceneNodes(std::shared_ptr<Scene> scene, SceneID id) {
    for (auto& node : scene->nodes) {
        world->registerNodeRecursive(node.get(), id);
    }
}

} // namespace Vapor
