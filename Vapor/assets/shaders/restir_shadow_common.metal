#ifndef RESTIR_SHADOW_COMMON_METAL
#define RESTIR_SHADOW_COMMON_METAL

#include "Res/shaders/3d_common.metal"

// Shared types + helpers for the ReSTIR stochastic-shadow passes
// (3d_restir_shadow_temporal.metal / 3d_restir_shadow_resolve.metal).
//
// Per pixel, one weighted-reservoir per analytic light domain (point / rect
// area / spot — the R/G/B channels of the shadow target). The reservoir's
// target function is the light's UNSHADOWED contribution estimate, so the
// resolve pass's single traced ray per domain lands on the light (and, for
// rect lights, the point on the quad) that actually dominates the pixel.
// Visibility deliberately stays OUT of the target function: the resolved
// output is the winner's binary visibility, whose expectation under a
// p̂-proportional selection is the contribution-weighted visibility — exactly
// the factor the PBR shader multiplies each domain's analytic sum by. The
// existing point-shadow temporal accumulator remains the final averager.

// Mirrors ShadowReservoirSetCPU in renderer.cpp — 32 bytes per pixel.
struct ShadowReservoirSet {
    uint  pointData;  // [0:16) point light index, [16:32) M
    float pointW;     // RIS weight W = wSum / (M * p̂(y))
    uint  spotData;   // [0:16) spot light index, [16:32) M
    float spotW;
    uint  rectData;   // [0:8) rect index, [8:20) quad u, [20:32) quad v
    float rectW;
    uint  rectM;      // [0:16) M (rectData has no room for it)
    float viewDepth;  // view-space z at write time (temporal validation)
};

// Mirrors RestirShadowParamsCPU in renderer.cpp — 80 bytes.
struct RestirShadowParams {
    float2 screenSize;      // pixels
    uint2  gridDims;        // light-cull tile grid (x, y)
    uint   frameIndex;
    uint   pointCount;      // live light counts (stale history indices are
    uint   rectCount;       //  dropped against these)
    uint   spotCount;
    uint   historyValid;    // 0 on the first frame / after resize or toggle
    uint   pointCandidates; // fresh RIS candidates per frame, per domain
    uint   rectCandidates;
    uint   spotCandidates;
    uint   debugMode;       // 0 visibility, 1 tile heatmap, 2 winner id, 3 M
    uint   spatialTaps;
    float  pointMClamp;     // absolute history M caps (point/spot share one;
    float  rectMClamp;      //  rect stays low so quad points keep refreshing)
    float  spatialRadius;   // px
    float  depthTolerance;  // max relative view-depth difference for reuse
    float  normalTolerance; // min normal dot for spatial reuse
    float  _pad0;
};

constant uint RESTIR_INVALID_LIGHT = 0xFFFFu;
constant uint RESTIR_INVALID_RECT  = 0xFFu;

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

inline uint restirPackIdxM(uint idx, float M) {
    return (idx & 0xFFFFu) | (min(uint(M + 0.5), 0xFFFFu) << 16);
}
inline uint restirUnpackIdx(uint data) { return data & 0xFFFFu; }
inline float restirUnpackM(uint data) { return float(data >> 16); }

// Rect sample = (light index, point on the quad). 12-bit quantized UV keeps
// the whole sample in one word; 1/4096 of the quad is far below penumbra
// scale.
inline uint restirPackRect(uint idx, float2 uv) {
    uint2 q = uint2(clamp(uv, 0.0, 1.0) * 4095.0 + 0.5);
    return (idx & 0xFFu) | (q.x << 8) | (q.y << 20);
}
inline uint restirUnpackRectIdx(uint data) { return data & 0xFFu; }
inline float2 restirUnpackRectUV(uint data) {
    return float2(float((data >> 8) & 0xFFFu), float((data >> 20) & 0xFFFu)) / 4095.0;
}

inline ShadowReservoirSet restirEmptySet(float viewDepth) {
    ShadowReservoirSet s;
    s.pointData = RESTIR_INVALID_LIGHT;
    s.pointW = 0.0;
    s.spotData = RESTIR_INVALID_LIGHT;
    s.spotW = 0.0;
    s.rectData = RESTIR_INVALID_RECT;
    s.rectW = 0.0;
    s.rectM = 0u;
    s.viewDepth = viewDepth;
    return s;
}

// ---------------------------------------------------------------------------
// RNG — same hash family as random() in 3d_common.metal, but stateful so each
// draw advances the sequence instead of re-deriving offsets by hand.
// ---------------------------------------------------------------------------

inline float restirRand(thread uint& state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float(word >> 8u) * (1.0 / 16777216.0); // [0, 1)
}

// ---------------------------------------------------------------------------
// Target functions p̂ — the UNSHADOWED contribution of a light sample at the
// shaded point, matching the PBR shaders' analytic attenuation exactly
// (3d_pbr_normal_mapped.metal) with N·L standing in for the BRDF.
// ---------------------------------------------------------------------------

inline float restirLuminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

inline float restirPointPdf(PointLight light, float3 P, float3 N) {
    float3 toL = light.position - P;
    float d2 = dot(toL, toL);
    float d = sqrt(d2);
    if (d >= light.radius || d < 1e-3) return 0.0;
    float ndotl = dot(N, toL / d);
    if (ndotl <= 0.0) return 0.0;
    float att = (1.0 / d2) * (1.0 - smoothstep(light.radius * 0.8, light.radius, d));
    return restirLuminance(light.color) * light.intensity * att * ndotl;
}

inline float restirSpotPdf(SpotLight light, float3 P, float3 N) {
    float3 toL = light.position - P;
    float d2 = dot(toL, toL);
    float d = sqrt(d2);
    if (d >= light.radius || d < 1e-3) return 0.0;
    float3 dir = toL / d;
    float ndotl = dot(N, dir);
    if (ndotl <= 0.0) return 0.0;
    float cone = clamp((dot(-dir, light.direction) - light.cosOuter) /
                       max(light.cosInner - light.cosOuter, 1e-4), 0.0, 1.0);
    if (cone <= 0.0) return 0.0;
    float att = (1.0 / d2) * (1.0 - smoothstep(light.radius * 0.8, light.radius, d));
    return restirLuminance(light.color) * light.intensity * att * cone * cone * ndotl;
}

inline float3 restirRectPoint(RectLight light, float2 uv) {
    float2 q = uv * 2.0 - 1.0;
    return float3(light.position) + float3(light.right) * (q.x * light.halfWidth)
                                  + float3(light.up) * (q.y * light.halfHeight);
}

inline float restirRectPdf(RectLight light, float2 uv, float3 P, float3 N) {
    float3 toL = restirRectPoint(light, uv) - P;
    float d2 = dot(toL, toL);
    if (d2 < 1e-6) return 0.0;
    float3 dir = toL / sqrt(d2);
    float ndotl = dot(N, dir);
    if (ndotl <= 0.0) return 0.0;
    // Double-sided emitter assumption, matching the trace pass (which shadows
    // the pixel no matter which face of the quad it sees).
    float cosEmit = abs(dot(dir, normalize(cross(float3(light.right), float3(light.up)))));
    return restirLuminance(float3(light.color)) * light.intensity * cosEmit * ndotl / d2;
}

// ---------------------------------------------------------------------------
// Weighted reservoir sampling (Bitterli et al. 2020, Algorithm 2/4).
// ---------------------------------------------------------------------------

struct WRSReservoir {
    uint  candidate; // domain-specific packed payload (index, or index+quad UV)
    float wSum;
    float M;
    float pdf;       // p̂(candidate) at THIS pixel
};

inline WRSReservoir wrsEmpty() {
    WRSReservoir r;
    r.candidate = 0xFFFFFFFFu;
    r.wSum = 0.0;
    r.M = 0.0;
    r.pdf = 0.0;
    return r;
}

// Stream one fresh candidate with resampling weight w = p̂/q.
inline void wrsUpdate(thread WRSReservoir& r, uint candidate, float w, float pdf, float rnd) {
    r.wSum += w;
    r.M += 1.0;
    if (w > 0.0 && rnd * r.wSum < w) {
        r.candidate = candidate;
        r.pdf = pdf;
    }
}

// Merge a stored reservoir (its W/M) whose sample was re-scored at the current
// pixel (pdfHere). The caller clamps M beforehand where history is involved.
inline void wrsMerge(thread WRSReservoir& r, uint candidate, float pdfHere, float W, float M, float rnd) {
    float w = pdfHere * W * M;
    r.wSum += w;
    r.M += M;
    if (w > 0.0 && rnd * r.wSum < w) {
        r.candidate = candidate;
        r.pdf = pdfHere;
    }
}

inline float wrsFinalizeW(WRSReservoir r) {
    return (r.pdf > 0.0 && r.M > 0.0) ? r.wSum / (r.M * r.pdf) : 0.0;
}

#endif
