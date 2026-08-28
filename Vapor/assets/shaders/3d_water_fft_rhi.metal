#include <metal_stdlib>
using namespace metal;

// FFT water simulation — MSL twins of the WaterSpectrum*/WaterFFT* GLSL
// kernels. See those files for the algorithm notes; layouts and math are
// identical (Vapor::WaterSimParams is the single source of truth).
//
// Bindings through the RHI:
//   buffers:  setComputeBuffer(0) = WaterSimParams (FFT kernel: direction uint)
//   textures: setComputeTexture(N) = texture(N)

struct WaterSimParamsRHI {
    uint resolution;
    float patchSize;
    float windSpeedMps;
    float windDirRad;
    float amplitude;
    float choppiness;
    float depthMeters;
    float smallWaveCutoff;
    float directionalSpread;
    float gravity;
    float surfaceTension;
    float timeScale;
    float time;
    uint seed;
    float _padA;
    float _padB;
};

constant float FFT_PI = 3.14159265359;

static float waterHash1(uint x) {
    x = x * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    x = (x >> 22u) ^ x;
    return float(x) / 4294967296.0;
}

static float waterGaussian(uint id, uint salt, uint seed) {
    float u1 = max(waterHash1(id * 4u + salt + seed * 0x9E3779B9u), 1e-6);
    float u2 = waterHash1(id * 4u + salt + 1u + seed * 0x85EBCA6Bu);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * FFT_PI * u2);
}

static float waterSpectrumAmp(float2 k, constant WaterSimParamsRHI& sim) {
    float kLen = length(k);
    if (kLen < 1e-5) return 0.0;

    float kSq = kLen * kLen;
    float2 windDir = float2(cos(sim.windDirRad), sin(sim.windDirRad));

    float L = sim.windSpeedMps * sim.windSpeedMps / sim.gravity;
    float phillips = sim.amplitude * exp(-1.0 / max(kSq * L * L, 1e-8)) / max(kSq * kSq, 1e-8);

    float c = dot(normalize(k), windDir);
    float dir = mix(0.08, 1.0, pow(abs(c), sim.directionalSpread));
    if (c < 0.0) dir *= 0.35;

    float cutoff = sim.smallWaveCutoff;
    phillips *= exp(-kSq * cutoff * cutoff);
    phillips *= tanh(min(kLen * sim.depthMeters, 20.0));

    return sqrt(max(phillips * dir, 0.0) * 0.5);
}

static float2 waterCmul(float2 a, float2 b) {
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// ── Initial spectrum ────────────────────────────────────────────────────────

kernel void waterSpectrumInit(
    uint2 id [[thread_position_in_grid]],
    constant WaterSimParamsRHI& sim [[buffer(0)]],
    texture2d<float, access::write> h0Tex [[texture(0)]]
) {
    uint n = sim.resolution;
    if (id.x >= n || id.y >= n) return;

    float twoPiOverL = 2.0 * FFT_PI / sim.patchSize;
    float2 k = float2(float(int(id.x) - int(n) / 2), float(int(id.y) - int(n) / 2)) * twoPiOverL;
    uint2 idNeg = uint2((n - id.x) % n, (n - id.y) % n);

    uint flatId = id.y * n + id.x;
    uint flatNeg = idNeg.y * n + idNeg.x;

    float2 h0k = float2(waterGaussian(flatId, 0u, sim.seed), waterGaussian(flatId, 2u, sim.seed))
               * waterSpectrumAmp(k, sim);
    float2 h0mk = float2(waterGaussian(flatNeg, 0u, sim.seed), waterGaussian(flatNeg, 2u, sim.seed))
                * waterSpectrumAmp(-k, sim);

    h0Tex.write(float4(h0k, h0mk.x, -h0mk.y), id);
}

// ── Time evolution ──────────────────────────────────────────────────────────

kernel void waterSpectrumEvolve(
    uint2 id [[thread_position_in_grid]],
    constant WaterSimParamsRHI& sim [[buffer(0)]],
    texture2d<float, access::read> h0Tex [[texture(0)]],
    texture2d<float, access::write> specATex [[texture(1)]],
    texture2d<float, access::write> specBTex [[texture(2)]]
) {
    uint n = sim.resolution;
    if (id.x >= n || id.y >= n) return;

    float twoPiOverL = 2.0 * FFT_PI / sim.patchSize;
    float2 k = float2(float(int(id.x) - int(n) / 2), float(int(id.y) - int(n) / 2)) * twoPiOverL;
    float kLen = length(k);

    float4 h0 = h0Tex.read(id);

    float w2 = (sim.gravity * kLen + sim.surfaceTension * kLen * kLen * kLen)
             * tanh(min(kLen * sim.depthMeters, 20.0));
    float w = sqrt(max(w2, 0.0));
    float phase = w * sim.time * sim.timeScale;
    float2 e = float2(cos(phase), sin(phase));
    float2 eConj = float2(e.x, -e.y);

    float2 h = waterCmul(h0.xy, e) + waterCmul(h0.zw, eConj);

    float2 kn = (kLen > 1e-5) ? k / kLen : float2(0.0);
    float2 dx = float2(h.y, -h.x) * kn.x;
    float2 dz = float2(h.y, -h.x) * kn.y;

    specATex.write(float4(h, dx), id);
    specBTex.write(float4(dz, 0.0, 0.0), id);
}

// ── One inverse-FFT pass over one axis (N = 256) ────────────────────────────

kernel void waterFFT(
    uint t [[thread_position_in_threadgroup]],
    uint line [[threadgroup_position_in_grid]],
    constant uint& direction [[buffer(0)]],
    texture2d<float, access::read_write> spec [[texture(0)]]
) {
    constexpr uint N = 256u;
    threadgroup float4 s[N];

    uint2 coord = (direction == 0u) ? uint2(t, line) : uint2(line, t);

    uint rev = reverse_bits(t) >> 24;
    s[rev] = spec.read(coord);
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (uint len = 2u; len <= N; len <<= 1u) {
        uint half_ = len >> 1u;
        if (t < N / 2u) {
            uint m = t % half_;
            uint i = (t / half_) * len + m;
            uint j = i + half_;
            float ang = 2.0 * FFT_PI * float(m) / float(len);  // inverse: +i
            float2 w = float2(cos(ang), sin(ang));
            float4 a = s[i];
            float4 b = s[j];
            float4 bw = float4(waterCmul(b.xy, w), waterCmul(b.zw, w));
            s[i] = a + bw;
            s[j] = a - bw;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    spec.write(s[t], coord);
}

// ── Assemble displacement ───────────────────────────────────────────────────

kernel void waterFFTAssemble(
    uint2 id [[thread_position_in_grid]],
    constant WaterSimParamsRHI& sim [[buffer(0)]],
    device float4* dispBuf [[buffer(1)]],
    texture2d<float, access::read> specATex [[texture(0)]],
    texture2d<float, access::read> specBTex [[texture(1)]],
    texture2d<float, access::write> dispTex [[texture(2)]]
) {
    uint n = sim.resolution;
    if (id.x >= n || id.y >= n) return;

    float sign_ = (((id.x + id.y) & 1u) == 0u) ? 1.0 : -1.0;

    float4 a = specATex.read(id);
    float4 b = specBTex.read(id);

    float h = a.x * sign_;
    float dx = a.z * sign_ * sim.choppiness;
    float dz = b.x * sign_ * sim.choppiness;

    float4 d = float4(dx, h, dz, 0.0);
    dispTex.write(d, id);
    dispBuf[id.y * n + id.x] = d;  // vertex-stage mirror (manual bilinear)
}

// ── Normals + whitecap foam from the displacement map ───────────────────────

static float3 waterDispAt(int2 id, int n, texture2d<float, access::read> dispTex) {
    uint2 wrapped = uint2((id.x % n + n) % n, (id.y % n + n) % n);
    return dispTex.read(wrapped).xyz;
}

kernel void waterFFTNormals(
    uint2 id [[thread_position_in_grid]],
    constant WaterSimParamsRHI& sim [[buffer(0)]],
    texture2d<float, access::read> dispTex [[texture(0)]],
    texture2d<float, access::write> normalTex [[texture(1)]]
) {
    int n = int(sim.resolution);
    if (int(id.x) >= n || int(id.y) >= n) return;

    float texel = sim.patchSize / float(n);

    float3 xp = waterDispAt(int2(id) + int2(1, 0), n, dispTex);
    float3 xm = waterDispAt(int2(id) - int2(1, 0), n, dispTex);
    float3 zp = waterDispAt(int2(id) + int2(0, 1), n, dispTex);
    float3 zm = waterDispAt(int2(id) - int2(0, 1), n, dispTex);

    float3 tx = float3(2.0 * texel + xp.x - xm.x, xp.y - xm.y, xp.z - xm.z);
    float3 tz = float3(zp.x - zm.x, zp.y - zm.y, 2.0 * texel + zp.z - zm.z);
    float3 nrm = normalize(cross(tz, tx));
    if (nrm.y < 0.0) nrm = -nrm;

    float jxx = 1.0 + (xp.x - xm.x) / (2.0 * texel);
    float jzz = 1.0 + (zp.z - zm.z) / (2.0 * texel);
    float jxz = (zp.x - zm.x) / (2.0 * texel);
    float jacobian = jxx * jzz - jxz * jxz;
    float foam = clamp(-(jacobian - 1.0) * 3.0, 0.0, 1.0);

    normalTex.write(float4(nrm, foam), id);
}
