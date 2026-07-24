#ifndef TERRAIN_NOISE_METAL
#define TERRAIN_NOISE_METAL

// ============================================================================
// Terrain height field — FastNoiseLite v1.1.1 OpenSimplex2 FBm, ported
// function-for-function from the vendored header (Vapor/FastNoiseLite).
// This is THE terrain height source: TerrainWorld::heightAt runs the real
// library on the CPU, RHIMain.frag carries the GLSL twin, and this include is
// the MSL twin — shared by the Main pass's terrain branch
// (3d_pbr_normal_mapped.metal) and the CBT tessellation pipeline
// (3d_tess_lib.metal) so displaced tessellation and per-pixel terrain normals
// evaluate the SAME field the streamed mesh is built on. Verified against the
// real FastNoiseLite by tests/terrain_world_test.cpp's parity suite (via the
// C++ transcription of this exact code).
// ============================================================================

constant int kFnlPrimeX = 501125321;
constant int kFnlPrimeY = 1136930381;
// Gradients2D is 128 pairs: a 24-direction fan (15 deg steps from 82.5 deg)
// repeated 5 times, then 8 picks at 45 deg steps (fan indices 1,4,7,...,22).
constant float2 kFnlFan[24] = {
    float2(0.130526192220052, 0.99144486137381),   float2(0.38268343236509, 0.923879532511287),
    float2(0.608761429008721, 0.793353340291235),  float2(0.793353340291235, 0.608761429008721),
    float2(0.923879532511287, 0.38268343236509),   float2(0.99144486137381, 0.130526192220051),
    float2(0.99144486137381, -0.130526192220051),  float2(0.923879532511287, -0.38268343236509),
    float2(0.793353340291235, -0.60876142900872),  float2(0.608761429008721, -0.793353340291235),
    float2(0.38268343236509, -0.923879532511287),  float2(0.130526192220052, -0.99144486137381),
    float2(-0.130526192220052, -0.99144486137381), float2(-0.38268343236509, -0.923879532511287),
    float2(-0.608761429008721, -0.793353340291235),float2(-0.793353340291235, -0.608761429008721),
    float2(-0.923879532511287, -0.38268343236509), float2(-0.99144486137381, -0.130526192220052),
    float2(-0.99144486137381, 0.130526192220051),  float2(-0.923879532511287, 0.38268343236509),
    float2(-0.793353340291235, 0.608761429008721), float2(-0.608761429008721, 0.793353340291235),
    float2(-0.38268343236509, 0.923879532511287),  float2(-0.130526192220052, 0.99144486137381)};
inline float2 fnlGradient2(int pairIndex) {
    return pairIndex < 120 ? kFnlFan[pairIndex % 24] : kFnlFan[1 + 3 * (pairIndex - 120)];
}
inline float fnlGradCoord(int seed, int xPrimed, int yPrimed, float xd, float yd) {
    int hash = (seed ^ xPrimed ^ yPrimed) * 0x27d4eb2d;
    hash ^= hash >> 15;
    hash &= 127 << 1;
    float2 g = fnlGradient2(hash >> 1);
    return xd * g.x + yd * g.y;
}
// SingleSimplex (2D OpenSimplex2): input is already frequency-scaled + skewed.
static float fnlSimplex2(int seed, float2 p) {
    const float SQRT3 = 1.7320508075688772935;
    const float G2 = (3.0 - SQRT3) / 6.0;
    int i = int(floor(p.x)), j = int(floor(p.y));
    float xi = p.x - float(i), yi = p.y - float(j);
    float t = (xi + yi) * G2;
    float x0 = xi - t, y0 = yi - t;
    i *= kFnlPrimeX;
    j *= kFnlPrimeY;
    float n0 = 0.0, n1 = 0.0, n2 = 0.0;
    float a = 0.5 - x0 * x0 - y0 * y0;
    if (a > 0.0) n0 = (a * a) * (a * a) * fnlGradCoord(seed, i, j, x0, y0);
    float c = (2.0 * (1.0 - 2.0 * G2) * (1.0 / G2 - 2.0)) * t
            + ((-2.0 * (1.0 - 2.0 * G2) * (1.0 - 2.0 * G2)) + a);
    if (c > 0.0) {
        float x2 = x0 + (2.0 * G2 - 1.0), y2 = y0 + (2.0 * G2 - 1.0);
        n2 = (c * c) * (c * c) * fnlGradCoord(seed, i + kFnlPrimeX, j + kFnlPrimeY, x2, y2);
    }
    if (y0 > x0) {
        float x1 = x0 + G2, y1 = y0 + (G2 - 1.0);
        float b = 0.5 - x1 * x1 - y1 * y1;
        if (b > 0.0) n1 = (b * b) * (b * b) * fnlGradCoord(seed, i, j + kFnlPrimeY, x1, y1);
    } else {
        float x1 = x0 + (G2 - 1.0), y1 = y0 + G2;
        float b = 0.5 - x1 * x1 - y1 * y1;
        if (b > 0.0) n1 = (b * b) * (b * b) * fnlGradCoord(seed, i + kFnlPrimeX, j, x1, y1);
    }
    return (n0 + n1 + n2) * 99.83685446303647;
}
// GetNoise: TransformNoiseCoordinate (frequency + F2 skew) -> GenFractalFBm
// (lacunarity 2, gain 0.5, weightedStrength 0), then the heightFn mapping
// noise * 0.5 + 0.5 clamped to [0,1] and scaled — matching heightAt exactly.
inline float trhHeightAt(float2 xz, float noiseFreq, int octaves, uint seed, float heightScale) {
    float2 p = xz * noiseFreq;
    const float SQRT3 = 1.7320508075688772935;
    const float F2 = 0.5 * (SQRT3 - 1.0);
    p += float2((p.x + p.y) * F2);
    const float gain = 0.5;
    float amp = gain, ampFractal = 1.0;
    for (int i = 1; i < octaves; ++i) { ampFractal += amp; amp *= gain; }
    int s = int(seed);
    float sum = 0.0;
    amp = 1.0 / ampFractal;  // fractalBounding
    for (int i = 0; i < octaves; ++i) {
        sum += fnlSimplex2(s++, p) * amp;
        p *= 2.0;
        amp *= gain;
    }
    return clamp(sum * 0.5 + 0.5, 0.0, 1.0) * heightScale;
}

#endif // TERRAIN_NOISE_METAL
