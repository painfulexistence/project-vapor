#pragma once
// [FIXTURE: naming]
// Each issue is tagged [ISSUE:NAM-XXX]. Run: /cpp-review naming --dir .claude/commands/cpp-review-fixtures
// Expected: all NAM-xxx issues found, no false positives on the "// OK" lines.

#include <string>
#include <vector>

// ── Enum value convention ─────────────────────────────────────────────────────

// [ISSUE:NAM-001] Enum values mix PascalCase and UPPER_SNAKE_CASE in the same enum
enum class RenderMode { Forward, DEFERRED, TileBasedDeferred };

// [ISSUE:NAM-002] Two enums in the same file use different value conventions
enum class BufferType { VERTEX_BUFFER, INDEX_BUFFER, UNIFORM };  // UPPER_SNAKE_CASE
enum class CameraMode { Perspective, Orthographic };             // PascalCase — inconsistent with BufferType

// OK: internally consistent enums (should NOT be flagged)
enum class GraphicsBackend { Metal, Vulkan, OpenGL };
enum class LoadMode { SYNC, ASYNC, STREAMING };

// ── Class naming ─────────────────────────────────────────────────────────────

// [ISSUE:NAM-003] Class uses snake_case instead of PascalCase
class render_pipeline {
public:
    // [ISSUE:NAM-004] Mixed member prefixes within the same class (m_, _, and no prefix)
    int   m_width  = 0;
    int   _height  = 0;  // different prefix style from m_width
    float depth    = 0;  // no prefix at all

    // [ISSUE:NAM-005] Boolean naming: `is` prefix used inconsistently
    bool  isActive  = false;  // has is-prefix
    bool  visible   = false;  // [ISSUE:NAM-005a] no is-prefix
    bool  enabled   = false;  // [ISSUE:NAM-005b] no is-prefix

    // [ISSUE:NAM-006] Single-word / short abbreviations with unclear meaning
    int   rid   = 0;   // resourceId?
    bool  hdr   = false;   // isHDR? hdrEnabled?
    int   gfxIdx = 0;  // graphicsIndex? graphicsFamilyIndex?

    // [ISSUE:NAM-007] Numeric suffixes with no semantic meaning
    std::vector<int> set0s;
    std::vector<int> set1s;
    std::vector<int> set2s;

    void process(int i, int j) {}  // OK — i, j are standard loop counters
};

// OK: correctly named class with consistent conventions (should NOT be flagged)
class RenderPipeline {
public:
    [[nodiscard]] bool isReady() const { return m_ready; }
    [[nodiscard]] int  getWidth() const { return m_width; }
private:
    bool m_ready = false;
    int  m_width = 0;
};

// ── Method naming ─────────────────────────────────────────────────────────────

// [ISSUE:NAM-008] Method suffix inconsistency: Idx vs Index used in same scope
class QueueFamilyInfo {
public:
    int graphicsFamilyIdx  = -1;  // Idx
    int presentFamilyIndex = -1;  // Index — inconsistent suffix
};
