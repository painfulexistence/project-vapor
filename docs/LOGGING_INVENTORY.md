# Logging / Debug Mechanisms — Inventory & Proposed Macro System

This is a snapshot of every logging / diagnostic mechanism the RHI-split work
introduced or touched, so it can be consolidated into one log-macro system.

## 1. Current state (what exists today)

### 1.1 Environment-variable gates

| Env var | Read at | Effect | Category |
|---|---|---|---|
| `GITHUB_ACTIONS` | `rhi_metal.cpp:96`, `renderer_metal.cpp:2908` | Force raytracing **off** in CI | behaviour switch |
| `VAPOR_DISABLE_RT` | `renderer.cpp:75` | Force raytracing off | behaviour switch |
| `VAPOR_DISABLE_RMLUI` | `renderer.cpp:2648` | Skip RmlUI pass | behaviour switch |
| `VAPOR_METAL_NATIVE` | `renderer.cpp:3970` | Select legacy native-Metal renderer instead of RHI | behaviour switch |
| `VAPOR_METAL_DEBUG` | `rhi_metal.cpp:814,897,919,1622` | `=1` GPU fault report; `=2` per-pass `[pass]` / `[frame committed]` print | **log gate** |
| `VAPOR_RHI_STATS` | `renderer.cpp:652`, `rhi_metal.cpp:789`, `rhi_vulkan.cpp:1567` | Per-120-frame leak-hunt telemetry (`[RSTATS]`/`[MTLSTATS]`/`[VKSTATS]`) | **log gate** |
| `VAPOR_RT_DEBUG` | `renderer.cpp:1484`, `rhi_metal.cpp:1535,1679` | On-change RT structure diagnostics (`[RT]`) | **log gate** |

The first four are *behaviour* switches (not logging) and should stay separate
from the log system. The last three are the actual **log gates** to fold in.

### 1.2 Output functions in use (inconsistent — the core problem)

| Sink | Used by | Notes |
|---|---|---|
| `fmt::print(stdout, ...)` | init/info messages in `renderer.cpp`, `rhi_metal.cpp`, `rhi_vulkan.cpp` | success/lifecycle |
| `fmt::print(stderr, ...)` | telemetry + errors (`[RSTATS]`, `[VKSTATS]`, `[RT]`, pool-exhaustion) | mixed levels |
| `fprintf(stderr, ...)` | `[MTLSTATS]` at `rhi_metal.cpp:792` | raw C `%`-format — the odd one out |
| `SDL_Log` / `SDL_LogError` | `scene.cpp` (15), `gibs_manager.cpp` (8), and most other subsystems (audio, asset, physics, video…) | engine-wide default |

### 1.3 Prefix tags currently in the wild

`[RSTATS]` (renderer maps), `[VKSTATS]` (Vulkan objects), `[MTLSTATS]`
(Metal objects), `[RT]` (accel-structure state), `[pass]` / `[frame committed]`
(Metal per-pass trace).

### 1.4 Frequency patterns (the axis that matters most)

| Pattern | Example | Implementation today |
|---|---|---|
| **once / lifecycle** | "Scene staged with N meshes", pipeline-creation errors | plain `fmt::print` |
| **on-error (always)** | "descriptor pool exhausted" (`rhi_vulkan.cpp:502,2791`) | `fmt::print(stderr)`, ungated |
| **per-N-frame throttle** | all three `*STATS` lines | `frameNumber % 120 == 0` + cached `getenv` bool |
| **on-change only** | `[RT]` — prints only when vis/inst/BLAS counts change | static last-value comparison |
| **per-pass trace** | Metal `[pass]` when `VAPOR_METAL_DEBUG==2` | `strcmp(dbgEnv,"2")` |

### 1.5 Non-print debug facilities (keep as-is, just noting them)

- `pointShadowDebugMode`, `mainDebugFlags` — GPU-side debug (ImGui-driven, pushed to shader).
- `isGpuTimingSupported` / `setGpuTimingEnabled` — GPU Pass Timings panel.

These are *visualization/telemetry surfaces*, not text logs — out of scope for the macro system.

---

## 2. Proposed unified macro system

Two orthogonal axes: **level** (severity, always-on vs opt-in) and **channel**
(subsystem gate). Keep the `getenv`-once-into-static-bool pattern that already works.

### 2.1 Header sketch (`Vapor/include/Vapor/log.hpp`)

```cpp
namespace vapor::log {

// One env var per opt-in channel; resolved once, cached.
enum class Chan { Stats, RT, Metal, Vulkan };
bool enabled(Chan c);          // getenv() cached in a static, per channel

// Frame counter the throttle macros read (renderer bumps it once/frame).
extern uint64_t g_frame;
}

// --- Always-on levels (unconditional) -------------------------------
#define VLOG_ERROR(...) ::fmt::print(stderr, "[ERR] "  __VA_ARGS__)
#define VLOG_WARN(...)  ::fmt::print(stderr, "[WARN] " __VA_ARGS__)
#define VLOG_INFO(...)  ::fmt::print(stdout, "[INFO] " __VA_ARGS__)

// --- Channel-gated debug (opt-in via env) ---------------------------
#define VLOG_DEBUG(chan, ...) \
    do { if (::vapor::log::enabled(::vapor::log::Chan::chan)) \
         ::fmt::print(stderr, "[" #chan "] " __VA_ARGS__); } while (0)

// --- Throttled: gated channel + once every N frames -----------------
#define VLOG_EVERY(chan, n, ...) \
    do { if (::vapor::log::enabled(::vapor::log::Chan::chan) && \
             (::vapor::log::g_frame % (n)) == 0) \
         ::fmt::print(stderr, "[" #chan "] " __VA_ARGS__); } while (0)
```

Channel → env-var table (inside `enabled()`):

| `Chan` | Env var | Replaces |
|---|---|---|
| `Stats` | `VAPOR_RHI_STATS` | `[RSTATS]` / `[MTLSTATS]` / `[VKSTATS]` |
| `RT` | `VAPOR_RT_DEBUG` | `[RT]` |
| `Metal` | `VAPOR_METAL_DEBUG` | `[pass]` / `[frame committed]` / fault report |
| `Vulkan` | `VAPOR_VK_DEBUG` (new) | future Vulkan traces |

### 2.2 Migration examples

```cpp
// before (rhi_metal.cpp:792) — raw fprintf, manual getenv+counter
static const bool rhiStats = std::getenv("VAPOR_RHI_STATS") != nullptr;
static Uint32 statsFrame = 0;
if (rhiStats && (statsFrame++ % 120) == 0)
    fprintf(stderr, "[MTLSTATS] f=%u buf=%zu ...\n", ...);

// after
VLOG_EVERY(Stats, 120, "[MTL] buf={} tex={} ...\n", buffers.size(), ...);

// before (rhi_vulkan.cpp:502) — ungated error
fmt::print(stderr, "flushDescriptors: descriptor pool exhausted\n");
// after
VLOG_ERROR("flushDescriptors: descriptor pool exhausted\n");
```

The **on-change** `[RT]` pattern (static last-value compare) stays as hand-written
code inside a `if (enabled(Chan::RT))` guard — it's too specialized to macro-ize
cleanly, but it should use `VLOG_DEBUG(RT, ...)` for the actual print.

### 2.3 What NOT to fold in

- Behaviour switches (`VAPOR_DISABLE_*`, `VAPOR_METAL_NATIVE`, `GITHUB_ACTIONS`) — not logging.
- `SDL_Log` in the wider engine — a separate, larger migration; do the RHI files
  first (`renderer.cpp`, `rhi_metal.cpp`, `rhi_vulkan.cpp`, `rml_renderer_rhi.cpp`)
  and leave engine-wide `SDL_Log` for a follow-up if desired.
- GPU-side debug (`mainDebugFlags`, `pointShadowDebugMode`, GPU timings) — not text logs.

---

## 3. Suggested rollout order

1. Land `log.hpp` + `enabled()`/`g_frame` definitions; renderer bumps `g_frame` once/frame.
2. Convert the 3 `*STATS` blocks → `VLOG_EVERY(Stats, 120, ...)`. Unifies the tag to `[STAT]`.
3. Convert ungated error prints (`fmt::print(stderr, "...")`) → `VLOG_ERROR`.
4. Convert `[RT]` and Metal `[pass]` sites → `VLOG_DEBUG(RT/Metal, ...)`.
5. (Optional) sweep `SDL_Log` in RHI files.
