#include "Vapor/scene_manager.hpp"

namespace Vapor {

SceneManager::SceneManager(World* world) : world(world) {}

std::shared_ptr<Scene> SceneManager::load(std::shared_ptr<Scene> scene, LoadMode mode) {
    if (!scene) {
        return nullptr;
    }

    SceneID id = generateSceneId();
    scene->sceneId = id;

    if (mode == LoadMode::Replace) {
        // Register nodes to World
        registerSceneNodes(scene, id);

        // Track this scene
        scenes[id] = scene;
        activeSceneId = id;
    } else {
        // Additive: append to active scene
        auto activeScene = getActiveScene();
        if (activeScene) {
            // Register nodes to World with the NEW scene's ID
            registerSceneNodes(scene, id);

            // Append content to active scene
            activeScene->append(scene);

            // Track the appended scene (for unloading later)
            scenes[id] = scene;
        } else {
            // No active scene, treat as Replace
            registerSceneNodes(scene, id);
            scenes[id] = scene;
            activeSceneId = id;
        }
    }

    return scene;
}

std::shared_ptr<Scene> SceneManager::load(std::shared_ptr<Scene> scene, std::shared_ptr<Node> parent) {
    if (!scene || !parent) {
        return nullptr;
    }

    SceneID id = generateSceneId();
    scene->sceneId = id;

    // Register nodes to World
    registerSceneNodes(scene, id);

    // Append to active scene under the specified parent
    auto activeScene = getActiveScene();
    if (activeScene) {
        activeScene->append(scene, parent);
    }

    // Track the appended scene
    scenes[id] = scene;

    return scene;
}

void SceneManager::unload(SceneID id) {
    auto it = scenes.find(id);
    if (it == scenes.end()) {
        return;
    }

    // Unregister nodes from World
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

SceneID SceneManager::generateSceneId() {
    return nextSceneId++;
}

void SceneManager::registerSceneNodes(std::shared_ptr<Scene> scene, SceneID id) {
    for (auto& node : scene->nodes) {
        world->registerNodeRecursive(node.get(), id);
    }
}

} // namespace Vapor
