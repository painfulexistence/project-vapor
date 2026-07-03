#pragma once
// [FIXTURE: cpp]
// Each issue is tagged [ISSUE:CPP-XXX]. Run: /cpp-review cpp --dir .claude/commands/cpp-review-fixtures
// Expected: all CPP-xxx found; the "// OK" examples must NOT be flagged.

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ── Missing qualifiers ────────────────────────────────────────────────────────

class ResourceCache {
public:
    // [ISSUE:CPP-001] Missing [[nodiscard]] on getters / query methods
    int         getId()    const { return m_id; }       // CPP-001a
    bool        isLoaded() const { return m_loaded; }   // CPP-001b
    bool        hasError() const { return m_error; }    // CPP-001c
    std::string getPath()  const { return m_path; }     // CPP-001d

    // [ISSUE:CPP-002] Missing noexcept on trivial const methods that cannot throw
    int getCount() const { return m_count; }    // CPP-002

    // [ISSUE:CPP-003] Missing const on getter — does not modify any member
    int getVersion() { return m_version; }      // CPP-003

    // OK — already correct
    [[nodiscard]] bool tryAcquire() const noexcept { return m_loaded; }

    // ── Uninitialized members ─────────────────────────────────────────────────
    // [ISSUE:CPP-004] Uninitialized POD members (UB if read before first write)
    int   m_id;      // CPP-004a
    float m_scale;   // CPP-004b
    bool  m_error;   // CPP-004c

    // OK — properly initialized
    bool        m_loaded  = false;
    int         m_count   = 0;
    int         m_version = 1;
    std::string m_path;

    // [ISSUE:CPP-005] NULL instead of nullptr
    void* m_legacyPtr = NULL;   // CPP-005

    // [ISSUE:CPP-006] std::function as a per-instance member in what could be
    //                 a high-cardinality ECS component (heap alloc per instance)
    std::function<void(int)> m_onComplete;   // CPP-006
};

// ── Raw owning pointer ────────────────────────────────────────────────────────

// [ISSUE:CPP-007] Raw owning pointer — manual new/delete, no RAII
class TextureManager {
public:
    TextureManager()  : m_data(new uint8_t[1024]) {}
    ~TextureManager() { delete[] m_data; }

    uint8_t* m_data;  // CPP-007 — owning raw pointer
};

// OK — non-owning observer pointer (should NOT be flagged)
class MeshRenderer {
public:
    explicit MeshRenderer(TextureManager* tex) : m_tex(tex) {}  // observer
private:
    TextureManager* m_tex = nullptr;  // does not own, does not delete
};

// ── C-style patterns ──────────────────────────────────────────────────────────

// [ISSUE:CPP-008] M_PI — POSIX macro, not standard C++
float circleArea(float r) {
    return static_cast<float>(M_PI) * r * r;  // CPP-008
}

// [ISSUE:CPP-009] Raw C array where std::array<T, N> is appropriate
void processVerts() {
    float positions[3] = { 0.0f, 1.0f, 0.0f };  // CPP-009
    (void)positions;
}

// OK — std::array used correctly
void processVertsOk() {
    std::array<float, 3> positions = { 0.0f, 1.0f, 0.0f };
    (void)positions;
}

// [ISSUE:CPP-010] strncpy — error-prone C API
void copyName(char* dst, const char* src, std::size_t n) {
    strncpy(dst, src, n);  // CPP-010
}

// [ISSUE:CPP-011] C-style cast instead of static_cast
float computeRatio(int a, int b) {
    return (float)a / (float)b;  // CPP-011
}

// ── Signed/unsigned comparison ────────────────────────────────────────────────

// [ISSUE:CPP-012] Comparing signed int with size_t from .size()
void checkBounds(int index, const std::vector<int>& v) {
    if (index < v.size()) {   // CPP-012 — signed < unsigned (size_t)
        (void)v[index];
    }
}

// OK — correct guard: check non-negative first, then cast
void checkBoundsOk(int index, const std::vector<int>& v) {
    if (index >= 0 && static_cast<std::size_t>(index) < v.size()) {
        (void)v[index];
    }
}

// ── Mutable lazy-init without mutable keyword ─────────────────────────────────

// [ISSUE:CPP-013] Lazy-init getter cannot be const because it modifies members,
//                 but the members should be mutable
class Camera {
public:
    // CPP-013 — logically const, but modifies _viewMatrix; should use mutable
    float* getViewMatrix() {
        if (_isDirty) {
            _viewMatrix[0] = 1.0f;  // recompute
            _isDirty = false;
        }
        return _viewMatrix;
    }

private:
    float _viewMatrix[16];  // CPP-004 (uninitialized) + CPP-013
    bool  _isDirty = true;
};

// OK — correct mutable pattern
class CameraOk {
public:
    [[nodiscard]] const float* getViewMatrix() const noexcept {
        if (m_isDirty) {
            m_viewMatrix[0] = 1.0f;
            m_isDirty = false;
        }
        return m_viewMatrix.data();
    }
private:
    mutable std::array<float, 16> m_viewMatrix = {};
    mutable bool m_isDirty = true;
};
