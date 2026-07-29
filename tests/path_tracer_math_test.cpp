// Energy and consistency tests for the photo-mode integrator's sampling math.
//
// The GPU kernel (3d_path_trace.metal) cannot run in CI — Metal only, and CI
// forces raytracing off — but its math is pure, so it is TRANSLITERATED here
// and tested statistically on the CPU. Three properties carry the ground-truth
// claim, none of which a screenshot can show:
//
//   1. BSDF parity: evalDisneyBRDF * N.L must equal the raster path's
//      CookTorranceBRDF (3d_pbr_lib.metal) on random inputs. This is the
//      literal statement of "the photo integrates the same PBR model the game
//      shades with" — if it drifts, photo mode is a different renderer.
//   2. The importance sampler and the evaluator agree: E[f·cos/pdf] equals
//      the numerically integrated reflectance across the parameter space,
//      including anisotropy, sheen, clearcoat and subsurface. If this drifts,
//      renders come out too bright or too dark with no crash and no cause.
//   3. Energy bounds: the sampler cannot manufacture energy the model does
//      not contain. (The Disney model itself is allowed to exceed 1 slightly
//      — sheen and clearcoat are additive by design; the photo reproduces the
//      model, so the bound documents the model, not ideal physics.)
//
// KEEP IN SYNC with 3d_path_trace.metal (evalDisneyBRDF, disneyLobeWeights,
// disneyPdf, sampleDisney, sampleGGXVNDFAniso, light helpers), 3d_pbr_lib.metal
// (rasterCookTorranceBRDF and the GTR/Smith/Fresnel primitives) and
// 3d_common.metal (randomNext, sampleCosineWeightedHemisphere). The
// transliteration is only a guard while it matches the MSL line for line.
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <cstdint>

namespace ptmath {

using glm::vec2;
using glm::vec3;

constexpr float PI = 3.1415927f;

inline float saturate(float x) { return glm::clamp(x, 0.0f, 1.0f); }

// 3d_common.metal randomNext — PCG hash, advancing state.
inline float randomNext(uint32_t& state) {
    state = state * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float(word >> 8u) * (1.0f / 16777216.0f);
}

// 3d_common.metal sampleCosineWeightedHemisphere.
inline vec3 sampleCosineWeightedHemisphere(vec2 s, vec3 normal) {
    float r = std::sqrt(s.x);
    float theta = 2.0f * PI * s.y;
    float x = r * std::cos(theta);
    float y = r * std::sin(theta);
    float z = std::sqrt(1.0f - s.x);
    vec3 axis = std::abs(normal.z) > 0.999f ? vec3(1, 0, 0) : vec3(0, 0, 1);
    vec3 tangent = glm::normalize(glm::cross(axis, normal));
    vec3 bitangent = glm::cross(normal, tangent);
    return tangent * x + bitangent * y + normal * z;
}

inline void buildBasis(vec3 n, vec3& t, vec3& b) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = vec3(c, sign + n.y * n.y * a, -n.y);
}

// ── 3d_pbr_lib.metal primitives ─────────────────────────────────────────────

// The PBR lib's own luma weights (0.3/0.6/0.1 — deliberately not Rec.709).
inline float luminanceD(vec3 color) {
    return glm::dot(color, vec3(0.3f, 0.6f, 0.1f));
}

inline float FresnelApprox(float u) {
    return std::pow(1.0f + 0.0001f - u, 5.0f);
}

inline float GTR1(float nh, float a) {
    if (a >= 1.0f) return 1.0f / PI;
    float a2 = a * a;
    float t = 1.0f + (a2 - 1.0f) * nh * nh;
    return (a2 - 1.0f) / (PI * std::log(a2) * t);
}

inline float GTR2_aniso(float nh, float hx, float hy, float ax, float ay) {
    float t = (hx * hx) / (ax * ax) + (hy * hy) / (ay * ay) + nh * nh;
    return 1.0f / (PI * ax * ay * t * t);
}

inline float SmithGGX(float u, float r) {
    float a = r * r;
    float b = u * u;
    return 1.0f / (u + std::sqrt(a + b - a * b));
}

inline float SmithGGX_aniso(float u, float vx, float vy, float ax, float ay) {
    float t = vx * vx * ax * ax + vy * vy * ay * ay + u * u;
    return 1.0f / (u + std::sqrt(t));
}

// 3d_pbr_lib.metal Surface (shading-relevant fields).
struct Surface {
    vec3 color = vec3(0.8f);
    float roughness = 0.5f;
    float metallic = 0.0f;
    float subsurface = 0.0f;
    float specular = 0.5f;
    float specular_tint = 0.0f;
    float anisotropic = 0.0f;
    float sheen = 0.0f;
    float sheen_tint = 0.0f;
    float clearcoat = 0.0f;
    float clearcoat_gloss = 0.0f;
};

// ── The raster BRDF, verbatim: 3d_pbr_lib.metal CookTorranceBRDF ───────────
// (returns f * nl, exactly like the fragment shader's call site expects).
inline vec3 rasterCookTorranceBRDF(vec3 norm, vec3 tangent, vec3 bitangent,
                                   vec3 lightDir, vec3 viewDir, Surface surf) {
    vec3 halfway = glm::normalize(lightDir + viewDir);
    float nv = std::max(glm::dot(norm, viewDir), 0.0f);
    float nl = std::max(glm::dot(norm, lightDir), 0.0f);
    float nh = std::max(glm::dot(norm, halfway), 0.0f);
    float lh = std::max(glm::dot(lightDir, halfway), 0.0f);
    float lum = luminanceD(surf.color);
    vec3 tint = lum > 0.0f ? surf.color / lum : vec3(1.0f);
    vec3 spec0 = glm::mix(surf.specular * 0.08f * glm::mix(vec3(1.0f), tint, surf.specular_tint),
                          surf.color, surf.metallic);
    float fh = FresnelApprox(lh);
    float fl = FresnelApprox(nl);
    float fv = FresnelApprox(nv);
    float fss90 = lh * lh * surf.roughness;
    float fd90 = 0.5f + 2.0f * fss90;
    float kd = glm::mix(1.0f, fd90, fl) * glm::mix(1.0f, fd90, fv);
    float fss = glm::mix(1.0f, fss90, fl) * glm::mix(1.0f, fss90, fv);
    float ss = 1.25f * (fss * (1.0f / (nl + nv + 0.0001f) - 0.5f) + 0.5f);
    float aspect = std::sqrt(1.0f - surf.anisotropic * 0.9f);
    float ax = std::max(0.001f, surf.roughness * surf.roughness / aspect);
    float ay = std::max(0.001f, surf.roughness * surf.roughness * aspect);
    float hx = glm::dot(halfway, tangent);
    float hy = glm::dot(halfway, bitangent);
    float lx = glm::dot(lightDir, tangent);
    float ly = glm::dot(lightDir, bitangent);
    float vx = glm::dot(viewDir, tangent);
    float vy = glm::dot(viewDir, bitangent);
    float D = GTR2_aniso(nh, hx, hy, ax, ay);
    float G = SmithGGX_aniso(nl, lx, ly, ax, ay) * SmithGGX_aniso(nv, vx, vy, ax, ay);
    vec3 F = glm::mix(spec0, vec3(1.0f), fh);
    vec3 specular = D * G * F;
    vec3 sheen = fh * surf.sheen * glm::mix(vec3(1.0f), tint, surf.sheen_tint);
    float Dr = GTR1(nh, glm::mix(0.1f, 0.001f, surf.clearcoat_gloss));
    float Fr = glm::mix(0.04f, 1.0f, fh);
    float Gr = SmithGGX(nl, 0.25f) * SmithGGX(nv, 0.25f);
    vec3 clearcoat = 0.25f * vec3(surf.clearcoat) * Dr * Fr * Gr;
    return ((glm::mix(kd, ss, surf.subsurface) * surf.color / PI + sheen) * (1.0f - surf.metallic)
            + specular + clearcoat) * nl;
}

// ── The photo-mode BRDF: 3d_path_trace.metal evalDisneyBRDF (no nl) ────────

inline vec3 evalDisneyBRDF(Surface surf, vec3 N, vec3 T, vec3 B, vec3 V, vec3 L) {
    float nl = glm::dot(N, L);
    float nv = glm::dot(N, V);
    if (nl <= 0.0f || nv <= 0.0f) return vec3(0.0f);

    vec3 H = glm::normalize(L + V);
    float nh = std::max(glm::dot(N, H), 0.0f);
    float lh = std::max(glm::dot(L, H), 0.0f);

    float lum = luminanceD(surf.color);
    vec3 tint = lum > 0.0f ? surf.color / lum : vec3(1.0f);
    vec3 spec0 = glm::mix(surf.specular * 0.08f * glm::mix(vec3(1.0f), tint, surf.specular_tint),
                          surf.color, surf.metallic);
    float fh = FresnelApprox(lh);
    float fl = FresnelApprox(nl);
    float fv = FresnelApprox(nv);
    float fss90 = lh * lh * surf.roughness;
    float fd90 = 0.5f + 2.0f * fss90;
    float kd = glm::mix(1.0f, fd90, fl) * glm::mix(1.0f, fd90, fv);
    float fss = glm::mix(1.0f, fss90, fl) * glm::mix(1.0f, fss90, fv);
    float ss = 1.25f * (fss * (1.0f / (nl + nv + 0.0001f) - 0.5f) + 0.5f);
    float aspect = std::sqrt(1.0f - surf.anisotropic * 0.9f);
    float ax = std::max(0.001f, surf.roughness * surf.roughness / aspect);
    float ay = std::max(0.001f, surf.roughness * surf.roughness * aspect);
    float D = GTR2_aniso(nh, glm::dot(H, T), glm::dot(H, B), ax, ay);
    float G = SmithGGX_aniso(nl, glm::dot(L, T), glm::dot(L, B), ax, ay)
            * SmithGGX_aniso(nv, glm::dot(V, T), glm::dot(V, B), ax, ay);
    vec3 F = glm::mix(spec0, vec3(1.0f), fh);
    vec3 specular = D * G * F;
    vec3 sheen = fh * surf.sheen * glm::mix(vec3(1.0f), tint, surf.sheen_tint);
    float Dr = GTR1(nh, glm::mix(0.1f, 0.001f, surf.clearcoat_gloss));
    float Fr = glm::mix(0.04f, 1.0f, fh);
    float Gr = SmithGGX(nl, 0.25f) * SmithGGX(nv, 0.25f);
    vec3 clearcoat = 0.25f * vec3(surf.clearcoat) * Dr * Fr * Gr;

    return (glm::mix(kd, ss, surf.subsurface) * surf.color / PI + sheen) * (1.0f - surf.metallic)
         + specular + clearcoat;
}

// ── The photo-mode sampler: mixture of cosine, aniso VNDF and GTR1 ─────────

struct LobeWeights {
    float pDiffuse;
    float pSpec;
    float pClearcoat;
};

inline LobeWeights disneyLobeWeights(Surface surf) {
    float lum = luminanceD(surf.color);
    vec3 tint = lum > 0.0f ? surf.color / lum : vec3(1.0f);
    vec3 spec0 = glm::mix(surf.specular * 0.08f * glm::mix(vec3(1.0f), tint, surf.specular_tint),
                          surf.color, surf.metallic);
    float wD = (1.0f - surf.metallic) * std::max(lum, 0.05f);
    float wS = std::max(luminanceD(spec0), 0.05f);
    float wC = 0.25f * surf.clearcoat;
    float total = wD + wS + wC;
    return { wD / total, wS / total, wC / total };
}

inline vec3 sampleGGXVNDFAniso(vec3 Ve, float ax, float ay, vec2 u) {
    vec3 Vh = glm::normalize(vec3(ax * Ve.x, ay * Ve.y, Ve.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0f ? vec3(-Vh.y, Vh.x, 0.0f) * (1.0f / std::sqrt(lensq)) : vec3(1, 0, 0);
    vec3 T2 = glm::cross(Vh, T1);
    float r = std::sqrt(u.x);
    float phi = 2.0f * PI * u.y;
    float t1 = r * std::cos(phi);
    float t2 = r * std::sin(phi);
    float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * std::sqrt(saturate(1.0f - t1 * t1)) + s * t2;
    vec3 Nh = t1 * T1 + t2 * T2 + std::sqrt(saturate(1.0f - t1 * t1 - t2 * t2)) * Vh;
    return glm::normalize(vec3(ax * Nh.x, ay * Nh.y, std::max(Nh.z, 0.0f)));
}

inline float disneyPdf(Surface surf, LobeWeights w, vec3 N, vec3 T, vec3 B, vec3 V, vec3 L) {
    float nl = glm::dot(N, L);
    float nv = glm::dot(N, V);
    if (nl <= 0.0f || nv <= 0.0f) return 0.0f;
    vec3 H = glm::normalize(V + L);
    float nh = std::max(glm::dot(N, H), 0.0f);
    float vh = std::max(glm::dot(V, H), 1e-6f);

    float aspect = std::sqrt(1.0f - surf.anisotropic * 0.9f);
    float ax = std::max(0.001f, surf.roughness * surf.roughness / aspect);
    float ay = std::max(0.001f, surf.roughness * surf.roughness * aspect);

    float pdfDiffuse = nl / PI;
    float D = GTR2_aniso(nh, glm::dot(H, T), glm::dot(H, B), ax, ay);
    float pdfSpec = D * SmithGGX_aniso(nv, glm::dot(V, T), glm::dot(V, B), ax, ay) * 0.5f;
    float aCC = glm::mix(0.1f, 0.001f, surf.clearcoat_gloss);
    float pdfClearcoat = GTR1(nh, aCC) * nh / (4.0f * vh);

    return w.pDiffuse * pdfDiffuse + w.pSpec * pdfSpec + w.pClearcoat * pdfClearcoat;
}

inline bool sampleDisney(Surface surf, vec3 N, vec3 T, vec3 B, vec3 V,
                         uint32_t& rngState, vec3& outDir, vec3& outWeight) {
    if (glm::dot(N, V) <= 0.0f) return false;
    LobeWeights w = disneyLobeWeights(surf);

    float pick = randomNext(rngState);
    vec2 u = vec2(randomNext(rngState), randomNext(rngState));

    float aspect = std::sqrt(1.0f - surf.anisotropic * 0.9f);
    float ax = std::max(0.001f, surf.roughness * surf.roughness / aspect);
    float ay = std::max(0.001f, surf.roughness * surf.roughness * aspect);

    vec3 L;
    if (pick < w.pDiffuse) {
        L = sampleCosineWeightedHemisphere(u, N);
    } else if (pick < w.pDiffuse + w.pSpec) {
        vec3 Vt = vec3(glm::dot(V, T), glm::dot(V, B), glm::dot(V, N));
        vec3 Ht = sampleGGXVNDFAniso(Vt, ax, ay, u);
        vec3 H = Ht.x * T + Ht.y * B + Ht.z * N;
        L = glm::reflect(-V, H);
    } else {
        float a = glm::mix(0.1f, 0.001f, surf.clearcoat_gloss);
        float a2 = a * a;
        float cosT2 = (1.0f - std::pow(a2, 1.0f - u.x)) / (1.0f - a2);
        float cosT = std::sqrt(saturate(cosT2));
        float sinT = std::sqrt(saturate(1.0f - cosT2));
        float phi = 2.0f * PI * u.y;
        vec3 H = T * (sinT * std::cos(phi)) + B * (sinT * std::sin(phi)) + N * cosT;
        L = glm::reflect(-V, H);
    }

    if (glm::dot(N, L) <= 0.0f) return false;
    float pdf = disneyPdf(surf, w, N, T, B, V, L);
    if (pdf < 1e-7f) return false;

    outDir = L;
    outWeight = evalDisneyBRDF(surf, N, T, B, V, L) * glm::dot(N, L) / pdf;
    return true;
}

// ── Analytic light transliterations (3d_path_trace.metal light NEE) ────────

inline float smoothstepf(float e0, float e1, float x) {
    float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// punctualAttenuation: the raster path's point/spot falloff — inverse square,
// windowed to exactly zero at the light's radius.
inline float punctualAttenuation(float dist, float radius) {
    float atten = 1.0f / std::max(dist * dist, 1e-6f);
    return atten * (1.0f - smoothstepf(radius * 0.8f, radius, dist));
}

// spotConeFactor: squared linear ramp between outer and inner cosines.
inline float spotConeFactor(vec3 L, vec3 spotDirection, float cosInner, float cosOuter) {
    float cosAngle = glm::dot(-L, spotDirection);
    float cone = glm::clamp((cosAngle - cosOuter) / std::max(cosInner - cosOuter, 1e-4f), 0.0f, 1.0f);
    return cone * cone;
}

struct RectLightGeom {
    vec3 position;
    vec3 right;       // normalized
    vec3 up;          // normalized
    float halfWidth;
    float halfHeight;
};

// Transliterated from 3d_pbr_lib.metal EvalRectLightDiffuse — the raster
// path's EXACT polygon solid-angle formula (Baum et al.). Returns
// dot(vectorIrradiance-ish sum, N) / 2π, so ∫cosθ dω over the quad = π * this.
inline float rectDiffuseGeo(vec3 N, vec3 fragPos, const RectLightGeom& light) {
    vec3 corners[4] = {
        light.position + light.right * light.halfWidth + light.up * light.halfHeight,
        light.position - light.right * light.halfWidth + light.up * light.halfHeight,
        light.position - light.right * light.halfWidth - light.up * light.halfHeight,
        light.position + light.right * light.halfWidth - light.up * light.halfHeight,
    };
    vec3 sum(0.0f);
    for (int i = 0; i < 4; i++) {
        vec3 v0 = glm::normalize(corners[i] - fragPos);
        vec3 v1 = glm::normalize(corners[(i + 1) % 4] - fragPos);
        vec3 c = glm::cross(v0, v1);
        float len = glm::length(c);
        if (len < 1e-6f) continue;
        float theta = std::atan2(len, glm::dot(v0, v1));
        sum += (theta / len) * c;
    }
    return std::max(0.0f, glm::dot(sum, N)) / (2.0f * PI);
}

// The photo-mode rect NEE geometry estimator: E over uniform quad samples of
// cosθ_surf * area * |cosθ_light| / d² — this IS ∫cosθ dω, estimated the way
// the kernel estimates it (same sampling, same double-sidedness).
inline float rectGeoViaAreaSampling(vec3 N, vec3 fragPos, const RectLightGeom& light,
                                    int samples, uint32_t seed) {
    uint32_t rng = seed;
    vec3 lightN = glm::normalize(glm::cross(light.right, light.up));
    float area = 4.0f * light.halfWidth * light.halfHeight;
    double sum = 0.0;
    for (int i = 0; i < samples; i++) {
        float u = 2.0f * randomNext(rng) - 1.0f;
        float v = 2.0f * randomNext(rng) - 1.0f;
        vec3 q = light.position + light.right * (light.halfWidth * u)
                                + light.up * (light.halfHeight * v);
        vec3 toQ = q - fragPos;
        float distSq = std::max(glm::dot(toQ, toQ), 1e-6f);
        vec3 L = toQ / std::sqrt(distSq);
        float NoL = glm::dot(N, L);
        if (NoL <= 0.0f) continue;
        float cosLight = std::abs(glm::dot(lightN, L));
        sum += double(NoL) * double(area * cosLight / distSq);
    }
    return float(sum / samples);
}

// Directional-hemispherical reflectance ρ(V), two independent estimators.

// A: what the integrator actually does — importance-sample the BSDF and
// average the returned weights (failed samples count as zero).
inline vec3 reflectanceViaSampler(Surface surf, vec3 N, vec3 T, vec3 B, vec3 V,
                                  int samples, uint32_t seed) {
    uint32_t rng = seed;
    vec3 sum(0.0f);
    for (int i = 0; i < samples; i++) {
        vec3 dir, w;
        if (sampleDisney(surf, N, T, B, V, rng, dir, w)) sum += w;
    }
    return sum / float(samples);
}

// B: brute force — uniform hemisphere Monte Carlo of ∫ f·cos dω, sharing no
// code path with the sampler beyond evalDisneyBRDF itself.
inline vec3 reflectanceViaUniformMC(Surface surf, vec3 N, vec3 T, vec3 B, vec3 V,
                                    int samples, uint32_t seed) {
    uint32_t rng = seed;
    vec3 sum(0.0f);
    for (int i = 0; i < samples; i++) {
        float u1 = randomNext(rng);
        float u2 = randomNext(rng);
        float z = u1;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float phi = 2.0f * PI * u2;
        vec3 L = T * (r * std::cos(phi)) + B * (r * std::sin(phi)) + N * z;
        sum += evalDisneyBRDF(surf, N, T, B, V, L) * glm::dot(N, L);
    }
    return sum * (2.0f * PI / float(samples));
}

} // namespace ptmath

using namespace ptmath;

// ============================================================
// RNG 與取樣分佈
// ============================================================

TEST_CASE("PT math: randomNext 均勻分佈於 [0,1)", "[ptmath]") {
    uint32_t rng = 12345u;
    double sum = 0.0;
    float lo = 1.0f, hi = 0.0f;
    const int N = 200000;
    for (int i = 0; i < N; i++) {
        float x = randomNext(rng);
        sum += x;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    REQUIRE(lo >= 0.0f);
    REQUIRE(hi < 1.0f);
    REQUIRE(std::abs(sum / N - 0.5) < 0.01);
}

TEST_CASE("PT math: cosine hemisphere 樣本在半球內且 E[cosθ] = 2/3", "[ptmath]") {
    // 傾斜的 normal，不能只測 (0,0,1) —— basis 建構的錯誤在軸對齊時看不出來
    const vec3 N = glm::normalize(vec3(0.3f, -0.5f, 0.8f));
    uint32_t rng = 777u;
    double cosSum = 0.0;
    const int count = 100000;
    for (int i = 0; i < count; i++) {
        vec2 s(randomNext(rng), randomNext(rng));
        vec3 d = sampleCosineWeightedHemisphere(s, N);
        float c = glm::dot(d, N);
        REQUIRE(c >= 0.0f);
        REQUIRE(std::abs(glm::length(d) - 1.0f) < 1e-3f);
        cosSum += c;
    }
    // cosine-weighted pdf 下 E[cosθ] = ∫cos²/π dω = 2/3
    REQUIRE(std::abs(cosSum / count - 2.0 / 3.0) < 0.005);
}

// ============================================================
// BSDF parity：photo mode 的 BRDF ≡ raster 的 CookTorranceBRDF
// ============================================================

TEST_CASE("PT math: evalDisneyBRDF × N.L ≡ raster CookTorranceBRDF", "[ptmath]") {
    // 這是 ground-truth 宣稱的字面語句：photo mode 積分的就是 raster shade
    // 的那個模型。隨機掃參數空間 + 隨機方向，兩個轉寫必須逐點一致。
    uint32_t rng = 20240729u;
    int tested = 0;
    for (int i = 0; i < 20000 && tested < 4000; i++) {
        Surface surf;
        surf.color = vec3(0.05f + 0.95f * randomNext(rng),
                          0.05f + 0.95f * randomNext(rng),
                          0.05f + 0.95f * randomNext(rng));
        surf.roughness = 0.02f + 0.98f * randomNext(rng);
        surf.metallic = randomNext(rng);
        surf.subsurface = randomNext(rng);
        surf.specular = randomNext(rng);
        surf.specular_tint = randomNext(rng);
        surf.anisotropic = randomNext(rng);
        surf.sheen = randomNext(rng);
        surf.sheen_tint = randomNext(rng);
        surf.clearcoat = randomNext(rng);
        surf.clearcoat_gloss = randomNext(rng);

        const vec3 N = glm::normalize(vec3(randomNext(rng) - 0.5f, randomNext(rng) - 0.5f,
                                           0.2f + randomNext(rng)));
        vec3 T, B;
        buildBasis(N, T, B);
        vec3 V = sampleCosineWeightedHemisphere(vec2(randomNext(rng), randomNext(rng)), N);
        vec3 L = sampleCosineWeightedHemisphere(vec2(randomNext(rng), randomNext(rng)), N);
        // 掠射邊界的 max(0)/early-out 差異不屬於模型本身，避開它
        if (glm::dot(N, V) < 1e-3f || glm::dot(N, L) < 1e-3f) continue;
        tested++;

        vec3 mine = evalDisneyBRDF(surf, N, T, B, V, L) * glm::dot(N, L);
        vec3 raster = rasterCookTorranceBRDF(N, T, B, L, V, surf);
        for (int c = 0; c < 3; c++) {
            INFO("case " << i << " channel " << c << " mine=" << mine[c] << " raster=" << raster[c]);
            REQUIRE(std::abs(mine[c] - raster[c]) <= 1e-4f * std::max(1.0f, std::abs(raster[c])));
        }
    }
    REQUIRE(tested >= 4000);
}

// ============================================================
// Sampler ↔ evaluator 一致性（積分器亮度正確性的核心）
// ============================================================

namespace {

void requireSamplerMatchesIntegral(Surface surf, const char* label,
                                   uint32_t seedA, uint32_t seedB) {
    const vec3 N(0.0f, 0.0f, 1.0f);
    vec3 T, B;
    buildBasis(N, T, B);
    // 45 度視角 —— 正對時 VNDF/aniso 的方向性看不出來
    const vec3 V = glm::normalize(vec3(0.0f, 0.7f, 0.7f));

    vec3 a = reflectanceViaSampler(surf, N, T, B, V, 400000, seedA);
    vec3 b = reflectanceViaUniformMC(surf, N, T, B, V, 1200000, seedB);
    for (int c = 0; c < 3; c++) {
        const float tolerance = 0.06f * std::max(a[c], b[c]) + 0.005f;
        INFO(label << " channel=" << c << " sampler=" << a[c] << " reference=" << b[c]);
        REQUIRE(std::abs(a[c] - b[c]) < tolerance);
    }
}

Surface makeSurface(float roughness, float metallic) {
    Surface s;
    s.color = vec3(0.8f, 0.6f, 0.4f);
    s.roughness = roughness;
    s.metallic = metallic;
    return s;
}

} // namespace

TEST_CASE("PT math: Disney sampler 期望值 = 數值積分（基本網格）", "[ptmath]") {
    // roughness 下限 0.3：更光滑的 lobe 用均勻半球參考積分的變異數太大，
    // 測不出有意義的界限（sampler 本身沒有這個限制；能量測試蓋 sharp 端）
    uint32_t seed = 1u;
    for (float roughness : { 0.3f, 0.6f, 1.0f }) {
        for (float metallic : { 0.0f, 0.5f, 1.0f }) {
            const uint32_t seedA = (seed += 17u);
            const uint32_t seedB = (seed += 31u);
            requireSamplerMatchesIntegral(makeSurface(roughness, metallic), "base", seedA, seedB);
        }
    }
}

TEST_CASE("PT math: Disney sampler 一致性 — 各向異性", "[ptmath]") {
    Surface s = makeSurface(0.5f, 1.0f);
    s.anisotropic = 0.8f;
    requireSamplerMatchesIntegral(s, "anisotropic", 211u, 223u);
}

TEST_CASE("PT math: Disney sampler 一致性 — sheen", "[ptmath]") {
    Surface s = makeSurface(0.6f, 0.0f);
    s.sheen = 1.0f;
    s.sheen_tint = 0.5f;
    requireSamplerMatchesIntegral(s, "sheen", 307u, 311u);
}

TEST_CASE("PT math: Disney sampler 一致性 — clearcoat", "[ptmath]") {
    // gloss 0 → α = 0.1：GTR1 最寬的一端，均勻參考積分還算得動；
    // 更 glossy 的端點由能量測試涵蓋
    Surface s = makeSurface(0.6f, 0.0f);
    s.clearcoat = 1.0f;
    s.clearcoat_gloss = 0.0f;
    requireSamplerMatchesIntegral(s, "clearcoat", 401u, 409u);
}

TEST_CASE("PT math: Disney sampler 一致性 — subsurface", "[ptmath]") {
    Surface s = makeSurface(0.6f, 0.0f);
    s.subsurface = 1.0f;
    requireSamplerMatchesIntegral(s, "subsurface", 503u, 509u);
}

TEST_CASE("PT math: Disney sampler 一致性 — specular/specularTint", "[ptmath]") {
    Surface s = makeSurface(0.4f, 0.0f);
    s.specular = 1.0f;
    s.specular_tint = 1.0f;
    requireSamplerMatchesIntegral(s, "spectint", 601u, 607u);

    Surface z = makeSurface(0.4f, 0.0f);
    z.specular = 0.0f;  // F0 = 0 電介質：spec lobe 為零但 pdf 仍覆蓋
    requireSamplerMatchesIntegral(z, "nospec", 701u, 709u);
}

TEST_CASE("PT math: Disney sampler 掠射角下仍一致", "[ptmath]") {
    const vec3 N(0.0f, 0.0f, 1.0f);
    vec3 T, B;
    buildBasis(N, T, B);
    const vec3 V = glm::normalize(vec3(0.0f, 0.98f, 0.2f));  // ~78 度
    Surface s;
    s.color = vec3(1.0f);
    s.roughness = 0.5f;

    vec3 a = reflectanceViaSampler(s, N, T, B, V, 400000, 99u);
    vec3 b = reflectanceViaUniformMC(s, N, T, B, V, 1200000, 101u);
    for (int c = 0; c < 3; c++) {
        REQUIRE(std::abs(a[c] - b[c]) < 0.06f * std::max(a[c], b[c]) + 0.005f);
    }
}

// ============================================================
// 能量界限（白爐測試的取樣端）
// ============================================================

TEST_CASE("PT math: 白色基本材質在任何粗糙度/金屬度下反射率 <= 1", "[ptmath]") {
    const vec3 N(0.0f, 0.0f, 1.0f);
    vec3 T, B;
    buildBasis(N, T, B);
    const vec3 V = glm::normalize(vec3(0.0f, 0.6f, 0.8f));

    // 這裡涵蓋 sharp lobe（consistency 測試蓋不到的區域）：能量上限
    // 不需要參考積分，只需要期望值本身
    uint32_t seed = 3u;
    for (float roughness : { 0.05f, 0.15f, 0.3f, 0.6f, 1.0f }) {
        for (float metallic : { 0.0f, 0.5f, 1.0f }) {
            Surface s;
            s.color = vec3(1.0f);
            s.roughness = roughness;
            s.metallic = metallic;
            vec3 rho = reflectanceViaSampler(s, N, T, B, V, 300000, seed += 13u);
            for (int c = 0; c < 3; c++) {
                INFO("roughness=" << roughness << " metallic=" << metallic << " rho=" << rho[c]);
                // 1.05：Disney 的 retro-reflective diffuse 在高粗糙度本來就
                // 略超 1（模型自身的性質），加上 MC 誤差；抓的是「明顯創造
                // 能量」這類 bug
                REQUIRE(rho[c] <= 1.05f);
                REQUIRE(rho[c] >= 0.0f);
            }
        }
    }
}

TEST_CASE("PT math: sheen/clearcoat 疊加的能量超額有界（模型自身的非守恆）", "[ptmath]") {
    // raster 的 Disney 模型把 sheen 和 clearcoat 疊加在基底之上 —— 模型本身
    // 就不守恆。photo mode 重現這個模型，所以這裡鎖的是「超額有上界」而不是
    // 「不超過 1」：上界失守代表 sampler 在創造模型沒有的能量。
    const vec3 N(0.0f, 0.0f, 1.0f);
    vec3 T, B;
    buildBasis(N, T, B);
    const vec3 V = glm::normalize(vec3(0.0f, 0.6f, 0.8f));

    Surface s;
    s.color = vec3(1.0f);
    s.roughness = 0.6f;
    s.sheen = 1.0f;
    s.clearcoat = 1.0f;
    s.clearcoat_gloss = 1.0f;  // α=.001：最 glossy 的 clearcoat 端點
    s.subsurface = 1.0f;
    vec3 rho = reflectanceViaSampler(s, N, T, B, V, 300000, 883u);
    for (int c = 0; c < 3; c++) {
        INFO("rho=" << rho[c]);
        REQUIRE(rho[c] >= 0.0f);
        REQUIRE(rho[c] <= 1.35f);
    }
}

// ============================================================
// 分析光源：photo mode 的 NEE 必須跟 raster 的定義吻合
// ============================================================

TEST_CASE("PT math: rect light 面積採樣 = Baum 立體角解析公式", "[ptmath]") {
    // 兩條完全獨立的數學路徑算同一個 ∫cosθ dω：
    //   MC 面積採樣（kernel 的做法，pdf 轉換 area·cosθ_l/d²）
    //   vs Baum 多邊形公式（raster 的做法，解析）
    // 一致才能宣稱 photo mode 的 rect diffuse 期望值 = raster 的精確項，
    // 也是 L_e = color·intensity/π 這個輻射度約定成立的前提。
    const vec3 N(0.0f, 0.0f, 1.0f);
    const vec3 fragPos(0.0f);

    RectLightGeom light;
    light.position = vec3(0.6f, 0.4f, 1.2f);
    light.right = glm::normalize(vec3(1.0f, 0.2f, -0.1f));
    light.up = glm::normalize(glm::cross(vec3(0.3f, -0.4f, 0.9f), light.right));
    light.halfWidth = 0.35f;
    light.halfHeight = 0.25f;

    const float analytic = PI * rectDiffuseGeo(N, fragPos, light);
    const float sampled = rectGeoViaAreaSampling(N, fragPos, light, 500000, 2024u);
    INFO("analytic=" << analytic << " sampled=" << sampled);
    REQUIRE(analytic > 0.01f);  // 這個佈局必須真的照到，否則測試空轉
    REQUIRE(std::abs(sampled - analytic) < 0.02f * analytic + 1e-4f);
}

TEST_CASE("PT math: rect light 從背面照樣發光（雙面，與 raster 一致）", "[ptmath]") {
    // raster 的 Baum 公式從背面看 winding 反轉、cross 全部變號，貢獻仍為正
    // —— rect light 是雙面光源。kernel 用 |cosθ_l| 對應這個行為。
    const vec3 N(0.0f, 0.0f, 1.0f);
    const vec3 fragPos(0.0f);

    RectLightGeom light;
    light.position = vec3(0.2f, -0.1f, 1.0f);
    // right × up 的法線朝 +z —— 面向「遠離」受光面的方向，受光面在背面
    light.right = vec3(1.0f, 0.0f, 0.0f);
    light.up = vec3(0.0f, 1.0f, 0.0f);
    light.halfWidth = 0.4f;
    light.halfHeight = 0.3f;

    const float analytic = PI * rectDiffuseGeo(N, fragPos, light);
    const float sampled = rectGeoViaAreaSampling(N, fragPos, light, 500000, 7u);
    INFO("analytic=" << analytic << " sampled=" << sampled);
    REQUIRE(analytic > 0.01f);
    REQUIRE(std::abs(sampled - analytic) < 0.02f * analytic + 1e-4f);
}

TEST_CASE("PT math: point light 衰減 — 反平方 + radius 窗函數", "[ptmath]") {
    const float radius = 10.0f;
    // 窗內（d < 0.8r）：純反平方
    REQUIRE(std::abs(punctualAttenuation(2.0f, radius) - 1.0f / 4.0f) < 1e-6f);
    REQUIRE(std::abs(punctualAttenuation(5.0f, radius) - 1.0f / 25.0f) < 1e-6f);
    // 窗中（0.8r < d < r）：小於純反平方
    REQUIRE(punctualAttenuation(9.0f, radius) < 1.0f / 81.0f);
    REQUIRE(punctualAttenuation(9.0f, radius) > 0.0f);
    // 邊界與界外：恰好為零 —— kernel 以 d >= radius 跳過是精確的，不是截斷
    REQUIRE(punctualAttenuation(10.0f, radius) == 0.0f);
    REQUIRE(punctualAttenuation(12.0f, radius) == 0.0f);
}

TEST_CASE("PT math: spot cone — 內全亮、外全暗、之間單調", "[ptmath]") {
    const vec3 spotDir(0.0f, 0.0f, -1.0f);  // 光朝 -z 照
    const float cosInner = std::cos(glm::radians(15.0f));
    const float cosOuter = std::cos(glm::radians(30.0f));

    // 正中央（表面在光正下方，L 指回光 = +z，-L = spotDir）
    REQUIRE(spotConeFactor(vec3(0, 0, 1), spotDir, cosInner, cosOuter) == 1.0f);
    // 外錐之外（表面在軸外 45 度）
    const float out = glm::radians(45.0f);
    vec3 Lout = glm::normalize(vec3(std::sin(out), 0.0f, std::cos(out)));
    REQUIRE(spotConeFactor(Lout, spotDir, cosInner, cosOuter) == 0.0f);

    // 內外之間：沿角度掃描單調遞減，且值域 (0,1)
    float prev = 1.0f;
    for (int deg = 16; deg < 30; deg++) {
        const float a = glm::radians(float(deg));
        vec3 L = glm::normalize(vec3(std::sin(a), 0.0f, std::cos(a)));
        float cone = spotConeFactor(L, spotDir, cosInner, cosOuter);
        REQUIRE(cone > 0.0f);
        REQUIRE(cone < 1.0f);
        REQUIRE(cone <= prev);
        prev = cone;
    }
}

TEST_CASE("PT math: Disney 取樣方向永遠在正半球、權重非負", "[ptmath]") {
    const vec3 N = glm::normalize(vec3(0.2f, 0.3f, 0.9f));
    vec3 T, B;
    buildBasis(N, T, B);
    const vec3 V = glm::normalize(vec3(-0.3f, 0.5f, 0.8f));
    Surface surf;
    surf.color = vec3(0.5f, 0.7f, 0.9f);
    surf.roughness = 0.25f;
    surf.metallic = 0.3f;
    surf.clearcoat = 0.5f;
    surf.sheen = 0.5f;
    surf.anisotropic = 0.4f;

    uint32_t rng = 42u;
    for (int i = 0; i < 50000; i++) {
        vec3 dir, w;
        if (!sampleDisney(surf, N, T, B, V, rng, dir, w)) continue;
        REQUIRE(glm::dot(dir, N) > 0.0f);
        REQUIRE(w.x >= 0.0f);
        REQUIRE(w.y >= 0.0f);
        REQUIRE(w.z >= 0.0f);
    }
}
