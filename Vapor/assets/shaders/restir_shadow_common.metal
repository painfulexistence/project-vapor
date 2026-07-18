#ifndef RESTIR_SHADOW_COMMON_METAL
#define RESTIR_SHADOW_COMMON_METAL

// Requires "Res/shaders/3d_common.metal" to be included FIRST by the kernel.
// No nested #include here: kernels compile from a path-less source string, so
// only top-level quoted includes reliably resolve against the process CWD —
// a quoted include nested inside this (real, on-disk) header would resolve
// against Res/shaders/ instead and fail. Same convention as gibs_common.metal.

// Shared types + helpers for the ReSTIR stochastic-shadow passes
// (3d_restir_shadow_temporal.metal / 3d_restir_shadow_resolve.metal).
//
// Per pixel, one weighted-reservoir per analytic light domain (point / rect
// area / spot — the R/G/B channels of the shadow target). The reservoir's
// target function is the light's UNSHADOWED contribution estimate, so the
// resolve pass's ray budget lands on the light that actually dominates the
// pixel. Visibility deliberately stays OUT of the target function: the
// resolved output is the winner's traced visibility, whose expectation under
// a p̂-proportional selection is the contribution-weighted visibility —
// exactly the factor the PBR shader multiplies each domain's analytic sum by.
// The existing point-shadow temporal accumulator remains the final averager.
//
// The reservoir sample is the LIGHT only, for rect lights too. The point on
// the quad must NOT be temporally reused: a rect penumbra is the fraction of
// the quad that is visible, so the traced point has to be re-jittered every
// frame to keep visibility an independent Bernoulli sample the accumulator
// can average — a reservoir-frozen quad point pins V at 0 or 1 for the whole
// dwell and the EMA has nothing to average (per-pixel-stable, spatially
// random patch noise). p̂ for a rect light is therefore evaluated at a fresh
// random quad point per evaluation — a stochastic estimate of the light's
// integral, which only jitters selection weights, never biases the output.
//
// Known tradeoff: temporal reuse correlates the winner across frames, so where
// two comparable lights disagree on visibility the output dwells on one state
// instead of averaging. The M clamp bounds that dwell (history ≤ clamp×
// candidates, so fresh samples keep ~1/(clamp+1) influence per frame); the
// default keeps winner refresh fast enough that the downstream temporal
// accumulator still averages across the switches.

// Mirrors ShadowReservoirSetCPU in renderer.cpp — 32 bytes per pixel.
struct ShadowReservoirSet {
    uint  pointData;     // [0:16) point light index, [16:32) M
    float pointW;        // RIS weight W = wSum / (M * p̂(y))
    uint  spotData;      // [0:16) spot light index, [16:32) M
    float spotW;
    uint  rectData;      // [0:16) rect light index, [16:32) M
    float rectW;
    uint  packedNormal;  // [0:16) oct-encoded surface normal (reuse gates)
    float viewDepth;     // view-space z at write time (temporal validation)
};

// Mirrors RestirShadowParamsCPU in renderer.cpp — 80 bytes.
struct RestirShadowParams {
    float2 screenSize;      // pixels
    uint2  gridDims;        // light-cull tile grid (x, y)
    uint   frameIndex;
    uint   pointCount;      // live light counts (stale history indices are
    uint   rectCount;       //  dropped against these)
    uint   spotCount;
    uint   historyValid;    // 0 on the first frame / after any skipped frame
    uint   pointCandidates; // fresh RIS candidates per frame, per domain
    uint   rectCandidates;
    uint   spotCandidates;
    uint   debugMode;       // 0 visibility, 1 tile heatmap, 2 winner id, 3 M
    uint   spatialTaps;
    float  pointMClamp;     // absolute history M caps, per domain
    float  rectMClamp;
    float  spatialRadius;   // px
    float  depthTolerance;  // max relative view-depth difference for reuse
    float  normalTolerance; // min normal dot for temporal/spatial reuse
    float  spotMClamp;
};

constant uint RESTIR_INVALID_LIGHT = 0xFFFFu;

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

inline uint restirPackIdxM(uint idx, float M) {
    return (idx & 0xFFFFu) | (min(uint(M + 0.5), 0xFFFFu) << 16);
}
inline uint restirUnpackIdx(uint data) { return data & 0xFFFFu; }
inline float restirUnpackM(uint data) { return float(data >> 16); }

// The surface normal rides in the reservoir set (8+8-bit octahedral, ~1-2°
// error against a 25° reuse threshold) so the temporal/spatial normal gates
// read only the struct they already loaded — the trick the AO chain uses.
inline uint restirPackNormal(float3 n) {
    float2 e = octEncode(n) * 0.5 + 0.5;
    uint2 q = uint2(e * 255.0 + 0.5);
    return q.x | (q.y << 8);
}
inline float3 restirUnpackNormal(uint packed) {
    float2 e = float2(float(packed & 0xFFu), float((packed >> 8) & 0xFFu)) / 255.0;
    return octDecode(e * 2.0 - 1.0);
}

inline ShadowReservoirSet restirEmptySet(float viewDepth) {
    ShadowReservoirSet s;
    s.pointData = RESTIR_INVALID_LIGHT;
    s.pointW = 0.0;
    s.spotData = RESTIR_INVALID_LIGHT;
    s.spotW = 0.0;
    s.rectData = RESTIR_INVALID_LIGHT;
    s.rectW = 0.0;
    s.packedNormal = 0u;
    s.viewDepth = viewDepth;
    return s;
}

// ---------------------------------------------------------------------------
// Surface reconstruction — the convention every reuse guard compares against,
// shared so the two kernels cannot drift (matches the stochastic kernel).
// ---------------------------------------------------------------------------

struct RestirSurface {
    float3 worldPos;
    float viewDepth;
};

inline RestirSurface restirReconstructSurface(
    uint2 tid, uint w, uint h, float depth, constant CameraData& camera
) {
    float2 uv = float2(tid) / float2(w, h);
    float2 ndcXY = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float4 viewPos = camera.invProj * float4(ndcXY, depth, 1.0);
    viewPos /= viewPos.w;
    RestirSurface s;
    s.worldPos = (camera.invView * viewPos).xyz;
    s.viewDepth = viewPos.z;
    return s;
}

// Tile-cluster index, matching the PBR/stochastic kernels' 2D convention.
// The min() keeps the top pixel row (uv.y == 0 → tileY == gridY) inside the
// culled grid instead of reading the never-written row beyond it.
inline uint restirClusterIndex(float2 uv, uint2 gridDims) {
    uint tileX = min(uint(uv.x * float(gridDims.x)), gridDims.x - 1);
    uint tileY = min(uint((1.0 - uv.y) * float(gridDims.y)), gridDims.y - 1);
    return tileX + tileY * gridDims.x;
}

// ---------------------------------------------------------------------------
// Target functions p̂ — the UNSHADOWED contribution of a light sample at the
// shaded point, matching the PBR shaders' analytic attenuation
// (3d_pbr_normal_mapped.metal) with N·L standing in for the BRDF.
// ---------------------------------------------------------------------------

inline float restirPointPdf(PointLight light, float3 P, float3 N) {
    float3 toL = light.position - P;
    float d2 = dot(toL, toL);
    float d = sqrt(d2);
    if (d >= light.radius || d < 1e-3) return 0.0;
    float ndotl = dot(N, toL / d);
    if (ndotl <= 0.0) return 0.0;
    float att = (1.0 / d2) * (1.0 - smoothstep(light.radius * 0.8, light.radius, d));
    return luminance709(light.color) * light.intensity * att * ndotl;
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
    return luminance709(light.color) * light.intensity * att * cone * cone * ndotl;
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
    // Quad area converts per-point radiance into the light's contribution, so
    // selection between rect lights of different sizes weighs correctly (a
    // uniform quad pdf is 1/area — the area must reappear in the target).
    float area = 4.0 * light.halfWidth * light.halfHeight;
    return luminance709(float3(light.color)) * light.intensity * area * cosEmit * ndotl / d2;
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

// Pack the three domain reservoirs (+ the reuse-validation surface data) into
// the stored form. Shared by both kernels so the layout cannot drift.
inline ShadowReservoirSet restirPackSet(
    WRSReservoir rPoint, WRSReservoir rRect, WRSReservoir rSpot,
    float viewDepth, float3 normal
) {
    ShadowReservoirSet out;
    out.pointData = restirPackIdxM(rPoint.pdf > 0.0 ? rPoint.candidate : RESTIR_INVALID_LIGHT, rPoint.M);
    out.pointW = wrsFinalizeW(rPoint);
    out.spotData = restirPackIdxM(rSpot.pdf > 0.0 ? rSpot.candidate : RESTIR_INVALID_LIGHT, rSpot.M);
    out.spotW = wrsFinalizeW(rSpot);
    out.rectData = restirPackIdxM(rRect.pdf > 0.0 ? rRect.candidate : RESTIR_INVALID_LIGHT, rRect.M);
    out.rectW = wrsFinalizeW(rRect);
    out.packedNormal = restirPackNormal(normal);
    out.viewDepth = viewDepth;
    return out;
}

#endif
