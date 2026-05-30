#pragma once
// [FIXTURE: duplicates]
// Each issue is tagged [ISSUE:DUP-XXX]. Run: /cpp-review duplicates --dir .claude/commands/cpp-review-fixtures
// Expected: all DUP-xxx found; the unique/non-duplicated code must NOT be flagged.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ── Near-identical struct defined in two places ───────────────────────────────

// [ISSUE:DUP-001] This component is also defined (identically) in legacy_issues.hpp.
//                 Both definitions should be consolidated into one canonical location.
struct FlyCameraComponent {
    float moveSpeed   = 5.0f;
    float rotateSpeed = 90.0f;
    float yaw         = -90.0f;
    float pitch       = 0.0f;
};

// [ISSUE:DUP-002] Near-duplicate with a different default value — silent divergence
//                 The engine version has maxPickupRange = 20.0f; this has 5.0f.
struct GrabberComponent {
    std::uint32_t heldEntity    = UINT32_MAX;
    float         maxPickupRange = 5.0f;  // engine has 20.0f — which is canonical?
};

// ── Copy-pasted index generation loops ───────────────────────────────────────

// [ISSUE:DUP-003] These two functions are byte-for-byte identical.
//                 Should be one function with an `offset` parameter.
void generateTopCapIndices(
    std::vector<std::uint32_t>& indices,
    std::uint32_t offset,
    std::uint32_t segments)
{
    for (std::uint32_t seg = 0; seg < segments; ++seg) {
        std::uint32_t cur  = offset + seg;
        std::uint32_t next = cur + segments + 1;
        indices.push_back(cur);
        indices.push_back(cur + 1);
        indices.push_back(next);
        indices.push_back(cur + 1);
        indices.push_back(next + 1);
        indices.push_back(next);
    }
}

void generateBottomCapIndices(  // [ISSUE:DUP-003] identical body
    std::vector<std::uint32_t>& indices,
    std::uint32_t offset,
    std::uint32_t segments)
{
    for (std::uint32_t seg = 0; seg < segments; ++seg) {
        std::uint32_t cur  = offset + seg;
        std::uint32_t next = cur + segments + 1;
        indices.push_back(cur);
        indices.push_back(cur + 1);
        indices.push_back(next);
        indices.push_back(cur + 1);
        indices.push_back(next + 1);
        indices.push_back(next);
    }
}

// OK — similar structure but different enough to be intentional
void generateFanIndices(
    std::vector<std::uint32_t>& indices,
    std::uint32_t center,
    std::uint32_t segments)
{
    for (std::uint32_t seg = 0; seg < segments; ++seg) {
        indices.push_back(center);
        indices.push_back(center + 1 + seg);
        indices.push_back(center + 1 + (seg + 1) % segments);
    }
}

// ── Stub / empty implementation ───────────────────────────────────────────────

class MeshBuilder {
public:
    // [ISSUE:DUP-004a] Stub: creates empty mesh and immediately returns
    static std::shared_ptr<int> buildCone(float radius, float height, int segments) {
        auto mesh = std::make_shared<int>(0);
        return mesh;  // not implemented
    }

    // [ISSUE:DUP-004b] Stub with TODO comment
    static std::shared_ptr<int> buildTerrain(int resolution, float scale) {
        // TODO: implement heightmap-based terrain generation
        return nullptr;
    }

    // OK — complete implementation
    static std::shared_ptr<int> buildCube(float size) {
        auto mesh = std::make_shared<int>(1);
        (void)size;
        // ... real vertex/index generation ...
        return mesh;
    }
};

// ── Parallel system implementations ──────────────────────────────────────────

// [ISSUE:DUP-005] TransformSystem implemented twice with incompatible signatures.
//                 The engine layer (Vapor/include/Vapor/systems.hpp) uses World&,
//                 this game-layer version uses a raw registry reference.
//                 The logic is equivalent but the two cannot be composed.
struct FakeWorld {};   // stand-in for the engine's World abstraction

class TransformSystem {
public:
    // Engine layer signature:
    static void update(FakeWorld& world, float dt) {}

    // Game layer re-implementation with different parameter type:
    // (In the real codebase this is a separate class in Vaporware/src/systems.hpp)
    // static void update(entt::registry& registry, float dt) {}
};
