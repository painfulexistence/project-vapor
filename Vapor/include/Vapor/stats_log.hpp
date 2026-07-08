#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace Vapor {

// ============================================================================
// StatsLog - unified per-frame telemetry
// ----------------------------------------------------------------------------
// Replaces the hand-rolled [VKSTATS]/[MTLSTATS]/[RSTATS]/[CULL]/[RT] blocks that
// each carried their own frame counter, env-var gate, and fprintf. A subsystem
// registers ONE source with a short tag and a fill callback; when the log is
// enabled (via the --stats CLI flag) each source emits a single grouped line
//
//     [VK] f=120 buf=337 tex=125 descSets=39936 stagingHW=6496KB
//
// every `interval` frames, both to stderr (live) and to a file (scroll-proof;
// tail -f while running, diff after). Backends register under their own tag, so
// exactly the instantiated backend's line appears — no `if (backend==...)`.
// ============================================================================

// One grouped record. A subsystem appends key=value fields; the tag and frame
// prefix are added by StatsLog at emit time.
class StatLine {
public:
    // Note: on LP64 std::size_t == std::uint64_t, so no separate size_t overload
    // (it would collide). Container .size() results bind to the uint64_t overload.
    void add(const char* key, std::uint64_t value);
    void add(const char* key, std::int64_t value);
    void add(const char* key, std::uint32_t value) { add(key, static_cast<std::uint64_t>(value)); }
    void add(const char* key, int value)            { add(key, static_cast<std::int64_t>(value)); }
    void add(const char* key, double value);
    void add(const char* key, const char* value);

    const std::string& str() const { return buffer_; }
    bool empty() const { return buffer_.empty(); }

private:
    std::string buffer_;
};

class StatsLog {
public:
    enum class Mode {
        Periodic,  // emit every `interval` frames
        OnChange,  // fill every frame; emit only when the line text changes
    };

    static StatsLog& get();

    // Master switch, wired to the --stats CLI flag. Off => tick() is a no-op and
    // no fill callback ever runs (zero hot-path cost when disabled).
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // In-program configuration (deliberately not env vars). Tweak in code.
    void setInterval(std::uint32_t frames) { interval_ = frames; }
    void setFile(std::string path) { filePath_ = std::move(path); }

    // Register a telemetry source. `fill` runs at most once per emit for
    // Periodic sources (the natural place to read-and-reset interval counters),
    // and once per frame for OnChange sources. Call during setup, not per frame.
    void addSource(const char* tag, std::function<void(StatLine&)> fill,
                   Mode mode = Mode::Periodic);

    // Drop all sources (call before the objects their callbacks capture die).
    void clearSources() { sources_.clear(); }

    // Call exactly once per frame from the render loop.
    void tick(std::uint64_t frame);

private:
    StatsLog() = default;
    ~StatsLog();
    StatsLog(const StatsLog&) = delete;
    StatsLog& operator=(const StatsLog&) = delete;

    void ensureFileOpen();

    struct Source {
        std::string tag;
        std::function<void(StatLine&)> fill;
        Mode mode;
        std::string last;  // OnChange dedup
    };

    bool enabled_ = false;
    std::uint32_t interval_ = 120;
    std::string filePath_ = "vapor_stats.log";
    std::FILE* file_ = nullptr;
    std::vector<Source> sources_;
};

} // namespace Vapor
