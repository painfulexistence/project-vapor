// [FIXTURE: legacy]
// Each issue is tagged [ISSUE:LEG-XXX]. Run: /cpp-review legacy --dir .claude/commands/cpp-review-fixtures
// Expected: all LEG-xxx found; the "new-only" code sections must NOT be flagged.

#include <memory>
#include <string>

// ── Two APIs for the same operation coexist ───────────────────────────────────

// Old API: static, synchronous, returns raw pointer
struct OldAssetManager {
    static int* loadImage(const std::string& path) {
        return nullptr;
    }
    static int* loadMesh(const std::string& path) {
        return nullptr;
    }
};

// New API: instance-based, supports async loading
struct ResourceManager {
    int loadImage(const std::string& path, bool async = false) { return 0; }
    int loadMesh(const std::string& path,  bool async = false) { return 0; }
};

void setupScene(ResourceManager& rm) {
    // [ISSUE:LEG-001a] Old API still called in active code path
    auto* img  = OldAssetManager::loadImage("textures/albedo.png");
    auto* mesh = OldAssetManager::loadMesh("models/player.obj");

    // New API — correct path
    auto imgHandle  = rm.loadImage("textures/albedo.png", true);
    auto meshHandle = rm.loadMesh("models/player.obj", true);

    (void)img; (void)mesh; (void)imgHandle; (void)meshHandle;
}

// ── Comments that admit incomplete migration ──────────────────────────────────

void processEntities() {
    // TODO: migrate this loop to use the new ECS view pattern         [ISSUE:LEG-002a]
    // FIXME: stagedMeshes accumulation should be removed after migration is done  [ISSUE:LEG-002b]
    // HACK: temporarily duplicating logic here until the old Node system is gone  [ISSUE:LEG-002c]
}

// ── Legacy type still referenced in non-legacy code ──────────────────────────

// Legacy CPU particle (kept for compatibility with older systems)  [ISSUE:LEG-003]
struct Particle {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// New GPU particle — the canonical type going forward
struct GPUParticle {
    float position[3] = {};
    float velocity[3] = {};
};

void spawnParticle() {
    Particle p;   // [ISSUE:LEG-003] active new code using the "legacy" struct
    p.x = 1.0f;

    GPUParticle g;  // OK — new type
    g.position[0] = 1.0f;
}

// ── FlyCameraComponent defined here AND in duplicates_issues.hpp ─────────────
// [ISSUE:LEG-004] / [ISSUE:DUP-001] — same struct in two fixture files to test
//                 cross-file duplicate detection.
struct FlyCameraComponent {  // also defined in duplicates_issues.hpp
    float moveSpeed   = 5.0f;
    float rotateSpeed = 90.0f;
    float yaw         = -90.0f;
    float pitch       = 0.0f;
};

// ── Two different Camera abstractions coexist ─────────────────────────────────

// Old direct camera object
struct Camera {
    float fov    = 60.0f;
    float aspect = 16.0f / 9.0f;
    float near   = 0.1f;
    float far    = 1000.0f;
    Camera(float fov, float aspect, float near, float far)
        : fov(fov), aspect(aspect), near(near), far(far) {}
};

// New ECS-component camera
struct VirtualCameraComponent {
    float fov     = 60.0f;
    bool  isActive = false;
    void setFov(float f) { fov = f; }
    float getFov() const { return fov; }
};

void renderFrame() {
    // [ISSUE:LEG-005a] Old-style camera still constructed and used
    Camera oldCam(60.0f, 1.778f, 0.1f, 1000.0f);

    // New style — correct
    VirtualCameraComponent newCam;
    newCam.setFov(60.0f);

    (void)oldCam; (void)newCam;
}
