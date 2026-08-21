// ReSTIR stochastic-shadow shared types + helpers — GLSL twin of
// restir_shadow_common.metal (see that file for the full design rationale:
// per-domain weighted reservoirs whose target function is the light's
// UNSHADOWED contribution; visibility stays out of the target and is traced
// only for the winner). Requires include/rt_common.glsl first (CameraData,
// light structs, octEncode/octDecode).
#ifndef RESTIR_COMMON_GLSL
#define RESTIR_COMMON_GLSL

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
    vec2  screenSize;      // pixels
    uvec2 gridDims;        // light-cull tile grid (x, y)
    uint  frameIndex;
    uint  pointCount;
    uint  rectCount;
    uint  spotCount;
    uint  historyValid;    // 0 on the first frame / after any skipped frame
    uint  pointCandidates; // fresh RIS candidates per frame, per domain
    uint  rectCandidates;
    uint  spotCandidates;
    uint  debugMode;       // 0 visibility, 1 tile heatmap, 2 winner id, 3 M
    uint  spatialTaps;
    float pointMClamp;     // absolute history M caps, per domain
    float rectMClamp;
    float spatialRadius;   // px
    float depthTolerance;  // max relative view-depth difference for reuse
    float normalTolerance; // min normal dot for temporal/spatial reuse
    float spotMClamp;
};

const uint RESTIR_INVALID_LIGHT = 0xFFFFu;

float luminance709(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

uint restirPackIdxM(uint idx, float M) {
    return (idx & 0xFFFFu) | (min(uint(M + 0.5), 0xFFFFu) << 16);
}
uint restirUnpackIdx(uint data) { return data & 0xFFFFu; }
float restirUnpackM(uint data) { return float(data >> 16); }

// The surface normal rides in the reservoir set (8+8-bit octahedral).
uint restirPackNormal(vec3 n) {
    vec2 e = octEncode(n) * 0.5 + 0.5;
    uvec2 q = uvec2(e * 255.0 + 0.5);
    return q.x | (q.y << 8);
}
vec3 restirUnpackNormal(uint p) {
    vec2 e = vec2(float(p & 0xFFu), float((p >> 8) & 0xFFu)) / 255.0;
    return octDecode(e * 2.0 - 1.0);
}

ShadowReservoirSet restirEmptySet(float viewDepth) {
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
// Surface reconstruction — matches the stochastic kernel's convention.
// ---------------------------------------------------------------------------

struct RestirSurface {
    vec3 worldPos;
    float viewDepth;
};

RestirSurface restirReconstructSurface(uvec2 tid, uint w, uint h, float depth,
                                       CameraData camera) {
    vec2 uv = vec2(tid) / vec2(w, h);
    vec2 ndcXY = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    vec4 viewPos = camera.invProj * vec4(ndcXY, depth, 1.0);
    viewPos /= viewPos.w;
    RestirSurface s;
    s.worldPos = (camera.invView * viewPos).xyz;
    s.viewDepth = viewPos.z;
    return s;
}

// Tile-cluster index, matching the PBR/stochastic kernels' 2D convention.
uint restirClusterIndex(vec2 uv, uvec2 gridDims) {
    uint tileX = min(uint(uv.x * float(gridDims.x)), gridDims.x - 1u);
    uint tileY = min(uint((1.0 - uv.y) * float(gridDims.y)), gridDims.y - 1u);
    return tileX + tileY * gridDims.x;
}

// ---------------------------------------------------------------------------
// Target functions p̂ — the UNSHADOWED contribution of a light sample.
// ---------------------------------------------------------------------------

float restirPointPdf(PointLight light, vec3 P, vec3 N) {
    vec3 toL = light.position - P;
    float d2 = dot(toL, toL);
    float d = sqrt(d2);
    if (d >= light.radius || d < 1e-3) return 0.0;
    float ndotl = dot(N, toL / d);
    if (ndotl <= 0.0) return 0.0;
    float att = (1.0 / d2) * (1.0 - smoothstep(light.radius * 0.8, light.radius, d));
    return luminance709(light.color) * light.intensity * att * ndotl;
}

float restirSpotPdf(SpotLight light, vec3 P, vec3 N) {
    vec3 toL = light.position - P;
    float d2 = dot(toL, toL);
    float d = sqrt(d2);
    if (d >= light.radius || d < 1e-3) return 0.0;
    vec3 dir = toL / d;
    float ndotl = dot(N, dir);
    if (ndotl <= 0.0) return 0.0;
    float cone = clamp((dot(-dir, light.direction) - light.cosOuter) /
                       max(light.cosInner - light.cosOuter, 1e-4), 0.0, 1.0);
    if (cone <= 0.0) return 0.0;
    float att = (1.0 / d2) * (1.0 - smoothstep(light.radius * 0.8, light.radius, d));
    return luminance709(light.color) * light.intensity * att * cone * cone * ndotl;
}

vec3 restirRectPoint(RectLight light, vec2 uv) {
    vec2 q = uv * 2.0 - 1.0;
    return light.position + light.rright * (q.x * light.halfWidth)
                          + light.up * (q.y * light.halfHeight);
}

float restirRectPdf(RectLight light, vec2 uv, vec3 P, vec3 N) {
    vec3 toL = restirRectPoint(light, uv) - P;
    float d2 = dot(toL, toL);
    if (d2 < 1e-6) return 0.0;
    vec3 dir = toL / sqrt(d2);
    float ndotl = dot(N, dir);
    if (ndotl <= 0.0) return 0.0;
    // Double-sided emitter assumption, matching the trace pass.
    float cosEmit = abs(dot(dir, normalize(cross(light.rright, light.up))));
    // Quad area converts per-point radiance into the light's contribution.
    float area = 4.0 * light.halfWidth * light.halfHeight;
    return luminance709(light.color) * light.intensity * area * cosEmit * ndotl / d2;
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

WRSReservoir wrsEmpty() {
    WRSReservoir r;
    r.candidate = 0xFFFFFFFFu;
    r.wSum = 0.0;
    r.M = 0.0;
    r.pdf = 0.0;
    return r;
}

// Stream one fresh candidate with resampling weight w = p̂/q.
void wrsUpdate(inout WRSReservoir r, uint candidate, float w, float pdf, float rnd) {
    r.wSum += w;
    r.M += 1.0;
    if (w > 0.0 && rnd * r.wSum < w) {
        r.candidate = candidate;
        r.pdf = pdf;
    }
}

// Merge a stored reservoir (its W/M) whose sample was re-scored at the current
// pixel (pdfHere). The caller clamps M beforehand where history is involved.
void wrsMerge(inout WRSReservoir r, uint candidate, float pdfHere, float W, float M, float rnd) {
    float w = pdfHere * W * M;
    r.wSum += w;
    r.M += M;
    if (w > 0.0 && rnd * r.wSum < w) {
        r.candidate = candidate;
        r.pdf = pdfHere;
    }
}

float wrsFinalizeW(WRSReservoir r) {
    return (r.pdf > 0.0 && r.M > 0.0) ? r.wSum / (r.M * r.pdf) : 0.0;
}

// Pack the three domain reservoirs (+ the reuse-validation surface data).
ShadowReservoirSet restirPackSet(WRSReservoir rPoint, WRSReservoir rRect, WRSReservoir rSpot,
                                 float viewDepth, vec3 normal) {
    ShadowReservoirSet o;
    o.pointData = restirPackIdxM(rPoint.pdf > 0.0 ? rPoint.candidate : RESTIR_INVALID_LIGHT, rPoint.M);
    o.pointW = wrsFinalizeW(rPoint);
    o.spotData = restirPackIdxM(rSpot.pdf > 0.0 ? rSpot.candidate : RESTIR_INVALID_LIGHT, rSpot.M);
    o.spotW = wrsFinalizeW(rSpot);
    o.rectData = restirPackIdxM(rRect.pdf > 0.0 ? rRect.candidate : RESTIR_INVALID_LIGHT, rRect.M);
    o.rectW = wrsFinalizeW(rRect);
    o.packedNormal = restirPackNormal(normal);
    o.viewDepth = viewDepth;
    return o;
}

#endif // RESTIR_COMMON_GLSL
