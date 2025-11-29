# World + SceneManager Architecture Design

## Overview

This document describes the proposed architecture for entity management and scene loading in Vapor. The design separates concerns into two main components:

- **World**: Runtime entity management, querying, and future ECS integration
- **SceneManager**: Scene loading, unloading, and lifecycle management

Both components are engine-side responsibilities. Application code interacts through clean APIs without needing to understand internal implementation details.

---

## Architecture Diagram

```
+---------------------------------------------------------------+
|                         Engine (Vapor)                        |
|                                                               |
|  +---------------------------+   +-------------------------+  |
|  |          World            |<--|     SceneManager        |  |
|  |                           |   |                         |  |
|  | - entities (flat list)    |   | - activeScenes          |  |
|  | - query API               |   | - load / unload         |  |
|  | - future: ECS systems     |   | - async loading         |  |
|  +---------------------------+   +-------------------------+  |
|              ^                             ^                  |
+--------------|-----------------------------|-----------------+
               |                             |
               |   Application interacts via these APIs
               |                             |
       world.find("Player")     sceneManager.load("level1.gltf")
```

---

## Design Goals

### 1. Flat Entity List for Efficient Operations

**Problem with hierarchy-only storage:**
- Unloading a scene requires recursive tree traversal
- Querying all entities requires recursive iteration
- Multi-scene management is complex

**Solution:**
- World maintains a flat list of ALL entities (including children)
- Each entity is tagged with its SceneID
- Hierarchy (children/parent) is preserved separately for transform inheritance

```
Hierarchy Tree (preserved)          Flat List (new)

    Root                            entities: [
    +-- Child1                        Root*,
    |   +-- Grandchild                Child1*,
    +-- Child2                        Grandchild*,
                                      Child2*
                                    ]
```

### 2. Separation of Concerns

| Responsibility | World | SceneManager |
|----------------|-------|--------------|
| Manage entity flat list | Yes | |
| Provide query API | Yes | |
| Manage ECS systems (future) | Yes | |
| Load/unload scenes | | Yes |
| Track active scenes | | Yes |
| Async loading & progress | | Yes |
| Scene transitions | | Yes |

### 3. Scene as Metadata (Not Container)

Previously, Scene owned all nodes. After this refactor:

```cpp
// Before: Scene contains nodes
struct Scene {
    std::vector<std::shared_ptr<Node>> nodes;  // owns nodes
};

// After: Scene is lightweight metadata
struct SceneHandle {
    SceneID id;
    std::string name;
    std::string sourceFile;               // "Sponza.gltf"
    std::vector<DirectionalLight> lights; // scene-level data
    // entities are NOT stored here - they live in World's flat list
};
```

### 4. LoadGLTF Returns Node* (Not Scene)

```cpp
// Before
std::shared_ptr<Scene> loadGLTF(const std::string& filename);

// After
std::shared_ptr<Node> loadGLTF(const std::string& filename);
// Or for multiple root nodes:
std::vector<std::shared_ptr<Node>> loadGLTF(const std::string& filename);
```

**Benefits:**
- Can attach loaded model anywhere in existing hierarchy
- Can instantiate same GLTF multiple times
- More flexible scene composition

---

## Benefits

### For Scene Unloading

```cpp
void World::unloadScene(SceneID id) {
    // O(n) single pass - no recursive tree traversal
    entities.erase(
        std::remove_if(entities.begin(), entities.end(),
            [id](Entity* e) {
                if (e->sceneId == id) {
                    delete e;
                    return true;
                }
                return false;
            }),
        entities.end()
    );
    activeScenes.erase(id);
}
```

### For Entity Queries

```cpp
class World {
    std::vector<Entity*> entities;
    std::unordered_map<std::string, Entity*> entityByName;  // O(1) lookup

public:
    Entity* find(const std::string& name);
    Entity* find(EntityID id);
    std::vector<Entity*> findByTag(const std::string& tag);
    std::vector<Entity*> findByScene(SceneID sceneId);
};
```

### For Multi-Scene Support

Multiple scenes can coexist in the same World:

```cpp
auto scene1 = sceneManager.load("level1.gltf");  // SceneID = 1
auto scene2 = sceneManager.load("characters.gltf");  // SceneID = 2

// All entities are in World's flat list, tagged with their SceneID
// Can unload independently:
sceneManager.unload(scene1);  // Only removes scene1's entities
```

### For Future ECS Integration

World encapsulates ECS registry internally:

```cpp
class World {
    // Internal: can be entt::registry, flecs::world, or custom
    // Application code doesn't know or care

public:
    // Simple API that hides ECS complexity
    Entity* find(const std::string& name);

    template<typename... Components>
    void forEach(std::function<void(Components&...)> callback);

    // Engine can optimize internally:
    // - Multi-threaded iteration
    // - Cache-friendly data layout (SoA)
    // - SIMD operations
};
```

**User code remains unchanged** even when engine internals evolve:

```cpp
// This code works whether World uses vector, entt, or flecs
auto player = world.find("Player");
auto& transform = world.getComponent<Transform>(player);

world.forEach<Transform, Velocity>([dt](Transform& t, Velocity& v) {
    t.position += v.value * dt;
});
```

---

## Hierarchy Handling

### Current Approach (Children Vector)

The existing children-based hierarchy is preserved:

```cpp
struct Entity {
    SceneID sceneId;
    std::vector<std::shared_ptr<Entity>> children;  // keep this
    glm::mat4 localTransform;
    glm::mat4 worldTransform;
};
```

Transform update still walks from root to leaves:

```cpp
void updateTransforms(Entity* entity, const glm::mat4& parentWorld) {
    entity->worldTransform = parentWorld * entity->localTransform;
    for (auto& child : entity->children) {
        updateTransforms(child.get(), entity->worldTransform);
    }
}
```

### Future Enhancement (Parent Pointer)

Parent pointer can be added later for:
- Child-to-root traversal (e.g., accumulating world transform from any node)
- Notifying parent when child is removed

```cpp
struct Entity {
    Entity* parent = nullptr;  // optional, add later
    std::vector<std::shared_ptr<Entity>> children;
    // ...
};
```

This is NOT required for the initial World + SceneManager implementation.

---

## Proposed API

### World

```cpp
namespace Vapor {

class World {
public:
    // Entity queries
    Entity* find(const std::string& name);
    Entity* find(EntityID id);
    std::vector<Entity*> findByTag(const std::string& tag);
    std::vector<Entity*> findByScene(SceneID sceneId);

    // Entity lifecycle (called by SceneManager internally)
    void registerEntity(Entity* entity, SceneID sceneId);
    void unregisterEntity(Entity* entity);
    void unregisterScene(SceneID sceneId);

    // Future: ECS-style iteration
    template<typename... Components>
    void forEach(std::function<void(Components&...)> callback);

private:
    std::vector<Entity*> entities;
    std::unordered_map<std::string, Entity*> entityByName;
    std::unordered_map<EntityID, Entity*> entityById;
};

}
```

### SceneManager

```cpp
namespace Vapor {

class SceneManager {
public:
    explicit SceneManager(World* world);

    // Scene loading
    SceneID load(const std::string& path,
                 LoadMode mode = LoadMode::Async,
                 std::function<void(SceneID)> onComplete = nullptr);

    // Scene unloading
    void unload(SceneID id);
    void unloadAll();

    // Scene queries
    bool isLoaded(SceneID id) const;
    const SceneHandle* getScene(SceneID id) const;
    std::vector<SceneID> getActiveScenes() const;

private:
    World* world;
    std::unordered_map<SceneID, SceneHandle> activeScenes;
    SceneID nextSceneId = 1;
};

}
```

---

## Migration Path

### Phase 1: Add World + SceneManager (No Breaking Changes)

1. Create World class with flat entity list
2. Create SceneManager class
3. Modify scene loading to register entities with World
4. Keep existing Node/Scene structures

### Phase 2: Refactor LoadGLTF Return Type

1. Change `loadGLTF` to return `Node*` instead of `Scene*`
2. Update SceneManager to handle new return type
3. Scene becomes lightweight metadata

### Phase 3: Add Parent Pointer (Optional)

1. Add `Entity* parent` field
2. Update hierarchy manipulation code
3. Enable child-to-root queries

### Phase 4: ECS Integration (Future)

1. Replace internal storage with ECS registry (entt/flecs)
2. Keep public API unchanged
3. Add component-based queries

---

## Open Questions

1. **Entity ID generation**: UUID vs incremental ID vs pointer-as-ID?
2. **Entity pooling**: Pre-allocate entity pool for performance?
3. **Deferred deletion**: Queue deletions and process end-of-frame?
4. **Event system**: How to notify when entities are added/removed?
