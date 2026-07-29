// Energy and consistency tests for the photo-mode integrator's sampling math.
//
// The GPU kernel (3d_path_trace.metal) cannot run in CI — Metal only, and CI
// forces raytracing off — but its math is pure, so it is TRANSLITERATED here
// and tested statistically on the CPU. The two properties under test are the
// ones a screenshot cannot show and a wrong render will not crash on:
//
//   1. The BSDF importance sampler and the BSDF evaluator agree: the sampler's
//      expected weight equals the numerically integrated reflectance. If this
//      drifts, renders come out too bright or too dark by a factor nobody can
//      eyeball back to a cause.
//   2. Energy conservation: a white surface never reflects more than it
//      receives, at any roughness/metallic combination.
//
// KEEP IN SYNC with 3d_path_trace.metal (buildBasis, smithG1, ggxD,
// fresnelSchlick, sampleGGXVNDF, evalBSDF, sampleBSDF) and 3d_common.metal
// (randomNext, sampleCosineWeightedHemisphere). The transliteration is only a
// guard while it matches the MSL line for line.
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

struct HitSurface {
    vec3 albedo;
    float metallic;
    float roughness;
};

inline void buildBasis(vec3 n, vec3& t, vec3& b) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = vec3(c, sign + n.y * n.y * a, -n.y);
}

inline float smithG1(float NoX, float alpha) {
    float a2 = alpha * alpha;
    float denom = NoX + std::sqrt(a2 + (1.0f - a2) * NoX * NoX);
    return denom > 0.0f ? (2.0f * NoX) / denom : 0.0f;
}

inline float ggxD(float NoH, float alpha) {
    float a2 = alpha * alpha;
    float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
    return a2 / std::max(PI * d * d, 1e-9f);
}

inline vec3 fresnelSchlick(float VoH, vec3 F0) {
    float f = std::pow(saturate(1.0f - VoH), 5.0f);
    return F0 + (vec3(1.0f) - F0) * f;
}

inline vec3 sampleGGXVNDF(vec3 Ve, float alpha, vec2 u) {
    vec3 Vh = glm::normalize(vec3(alpha * Ve.x, alpha * Ve.y, Ve.z));
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
    return glm::normalize(vec3(alpha * Nh.x, alpha * Nh.y, std::max(Nh.z, 0.0f)));
}

inline vec3 evalBSDF(HitSurface surf, vec3 N, vec3 V, vec3 L) {
    float NoL = glm::dot(N, L);
    float NoV = glm::dot(N, V);
    if (NoL <= 0.0f || NoV <= 0.0f) return vec3(0.0f);

    vec3 H = glm::normalize(V + L);
    float NoH = saturate(glm::dot(N, H));
    float VoH = saturate(glm::dot(V, H));

    vec3 F0 = glm::mix(vec3(0.04f), surf.albedo, surf.metallic);
    vec3 F = fresnelSchlick(VoH, F0);
    float alpha = std::max(surf.roughness * surf.roughness, 1e-3f);

    vec3 spec = F * (ggxD(NoH, alpha) * smithG1(NoV, alpha) * smithG1(NoL, alpha))
              / std::max(4.0f * NoV * NoL, 1e-6f);
    vec3 diffuse = surf.albedo * (1.0f - surf.metallic) * (vec3(1.0f) - F) / PI;
    return diffuse + spec;
}

inline bool sampleBSDF(HitSurface surf, vec3 N, vec3 V, uint32_t& rngState,
                       vec3& outDir, vec3& weight) {
    float NoV = glm::dot(N, V);
    if (NoV <= 0.0f) return false;

    vec3 F0 = glm::mix(vec3(0.04f), surf.albedo, surf.metallic);
    vec3 diffuseAlbedo = surf.albedo * (1.0f - surf.metallic);

    float diffuseWeight = glm::dot(diffuseAlbedo, vec3(1.0f)) / 3.0f;
    float specWeight = glm::dot(fresnelSchlick(NoV, F0), vec3(1.0f)) / 3.0f;
    float total = diffuseWeight + specWeight;
    float pSpec = total > 0.0f ? saturate(specWeight / total) : 1.0f;
    pSpec = glm::clamp(pSpec, 0.1f, 0.9f);

    vec3 T, B;
    buildBasis(N, T, B);

    if (randomNext(rngState) < pSpec) {
        float alpha = std::max(surf.roughness * surf.roughness, 1e-3f);
        vec3 Vt = vec3(glm::dot(V, T), glm::dot(V, B), glm::dot(V, N));
        vec2 u = vec2(randomNext(rngState), randomNext(rngState));
        vec3 Ht = sampleGGXVNDF(Vt, alpha, u);
        vec3 Lt = glm::reflect(-Vt, Ht);
        if (Lt.z <= 0.0f) return false;

        outDir = glm::normalize(Lt.x * T + Lt.y * B + Lt.z * N);
        float VoH = saturate(glm::dot(Vt, Ht));
        vec3 F = fresnelSchlick(VoH, F0);
        weight = F * smithG1(Lt.z, alpha) / pSpec;
        return true;
    }

    vec2 u = vec2(randomNext(rngState), randomNext(rngState));
    outDir = sampleCosineWeightedHemisphere(u, N);
    if (glm::dot(outDir, N) <= 0.0f) return false;
    vec3 H = glm::normalize(V + outDir);
    vec3 F = fresnelSchlick(saturate(glm::dot(V, H)), F0);
    weight = diffuseAlbedo * (vec3(1.0f) - F) / (1.0f - pSpec);
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
inline vec3 reflectanceViaSampler(HitSurface surf, vec3 N, vec3 V, int samples, uint32_t seed) {
    uint32_t rng = seed;
    vec3 sum(0.0f);
    for (int i = 0; i < samples; i++) {
        vec3 dir, w;
        if (sampleBSDF(surf, N, V, rng, dir, w)) sum += w;
    }
    return sum / float(samples);
}

// B: brute force — uniform hemisphere Monte Carlo of ∫ f·cos dω, sharing no
// code path with the sampler beyond evalBSDF itself.
inline vec3 reflectanceViaUniformMC(HitSurface surf, vec3 N, vec3 V, int samples, uint32_t seed) {
    uint32_t rng = seed;
    vec3 T, B;
    buildBasis(N, T, B);
    vec3 sum(0.0f);
    for (int i = 0; i < samples; i++) {
        // Uniform hemisphere around N (pdf = 1 / 2π).
        float u1 = randomNext(rng);
        float u2 = randomNext(rng);
        float z = u1;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float phi = 2.0f * PI * u2;
        vec3 L = T * (r * std::cos(phi)) + B * (r * std::sin(phi)) + N * z;
        sum += evalBSDF(surf, N, V, L) * glm::dot(N, L);
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
// Sampler ↔ evaluator 一致性（積分器亮度正確性的核心）
// ============================================================

TEST_CASE("PT math: BSDF sampler 期望值 = 數值積分的反射率", "[ptmath]") {
    const vec3 N(0.0f, 0.0f, 1.0f);
    // 45 度視角 —— 掠射角另測，正對時 VNDF 的各向異性看不出來
    const vec3 V = glm::normalize(vec3(0.0f, 0.7f, 0.7f));

    // roughness 下限 0.3：更光滑的 lobe 用均勻半球參考積分的變異數太大，
    // 測不出有意義的界限（sampler 本身沒有這個限制）
    const float roughnessGrid[] = { 0.3f, 0.6f, 1.0f };
    const float metallicGrid[] = { 0.0f, 0.5f, 1.0f };

    uint32_t seed = 1u;
    for (float roughness : roughnessGrid) {
        for (float metallic : metallicGrid) {
            HitSurface surf{ vec3(0.8f, 0.6f, 0.4f), metallic, roughness };
            vec3 a = reflectanceViaSampler(surf, N, V, 400000, seed += 17u);
            vec3 b = reflectanceViaUniformMC(surf, N, V, 1000000, seed += 31u);
            for (int c = 0; c < 3; c++) {
                // 兩個獨立估計器都有 MC 誤差；容差取相對 6% + 絕對 0.005
                const float tolerance = 0.06f * std::max(a[c], b[c]) + 0.005f;
                INFO("roughness=" << roughness << " metallic=" << metallic
                     << " channel=" << c << " sampler=" << a[c] << " reference=" << b[c]);
                REQUIRE(std::abs(a[c] - b[c]) < tolerance);
            }
        }
    }
}

TEST_CASE("PT math: 掠射角下 sampler 與積分仍一致", "[ptmath]") {
    const vec3 N(0.0f, 0.0f, 1.0f);
    const vec3 V = glm::normalize(vec3(0.0f, 0.98f, 0.2f));  // ~78 度
    HitSurface surf{ vec3(1.0f), 0.0f, 0.5f };

    vec3 a = reflectanceViaSampler(surf, N, V, 400000, 99u);
    vec3 b = reflectanceViaUniformMC(surf, N, V, 1000000, 101u);
    for (int c = 0; c < 3; c++) {
        REQUIRE(std::abs(a[c] - b[c]) < 0.06f * std::max(a[c], b[c]) + 0.005f);
    }
}

// ============================================================
// 能量守恆（白爐測試的取樣端）
// ============================================================

TEST_CASE("PT math: 白色表面在任何粗糙度/金屬度下反射率 <= 1", "[ptmath]") {
    const vec3 N(0.0f, 0.0f, 1.0f);
    const vec3 V = glm::normalize(vec3(0.0f, 0.6f, 0.8f));

    // 這裡涵蓋 sharp lobe（consistency 測試蓋不到的區域）：能量上限
    // 不需要參考積分，只需要期望值本身
    const float roughnessGrid[] = { 0.05f, 0.15f, 0.3f, 0.6f, 1.0f };
    const float metallicGrid[] = { 0.0f, 0.5f, 1.0f };

    uint32_t seed = 3u;
    for (float roughness : roughnessGrid) {
        for (float metallic : metallicGrid) {
            HitSurface surf{ vec3(1.0f), metallic, roughness };
            vec3 rho = reflectanceViaSampler(surf, N, V, 300000, seed += 13u);
            for (int c = 0; c < 3; c++) {
                INFO("roughness=" << roughness << " metallic=" << metallic << " rho=" << rho[c]);
                // 1.02：容許 MC 誤差，抓的是「明顯創造能量」這類 bug
                REQUIRE(rho[c] <= 1.02f);
                REQUIRE(rho[c] >= 0.0f);
            }
        }
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

TEST_CASE("PT math: 取樣方向永遠在正半球、權重非負", "[ptmath]") {
    const vec3 N = glm::normalize(vec3(0.2f, 0.3f, 0.9f));
    const vec3 V = glm::normalize(vec3(-0.3f, 0.5f, 0.8f));
    HitSurface surf{ vec3(0.5f, 0.7f, 0.9f), 0.3f, 0.25f };

    uint32_t rng = 42u;
    for (int i = 0; i < 50000; i++) {
        vec3 dir, w;
        if (!sampleBSDF(surf, N, V, rng, dir, w)) continue;
        REQUIRE(glm::dot(dir, N) > 0.0f);
        REQUIRE(w.x >= 0.0f);
        REQUIRE(w.y >= 0.0f);
        REQUIRE(w.z >= 0.0f);
    }
}
