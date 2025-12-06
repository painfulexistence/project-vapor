#ifndef VOLUMETRIC_COMMON_METAL
#define VOLUMETRIC_COMMON_METAL

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Constants
// ============================================================================

constant float VOL_PI = 3.14159265359;
constant float VOL_INV_4PI = 1.0 / (4.0 * VOL_PI);

// ============================================================================
// Phase Functions - Shared by Fog and Clouds
// ============================================================================

// Henyey-Greenstein phase function
// g: anisotropy parameter (-1 = full back-scatter, 0 = isotropic, 1 = full forward-scatter)
// cosTheta: dot product between view direction and light direction
float phaseHenyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return VOL_INV_4PI * (1.0 - g2) / pow(denom, 1.5);
}

// Dual-lobe phase function for cloud silver lining effect
// Combines forward and back scatter lobes
float phaseDualLobe(float cosTheta, float g1, float g2, float blend) {
    float phase1 = phaseHenyeyGreenstein(cosTheta, g1);
    float phase2 = phaseHenyeyGreenstein(cosTheta, g2);
    return mix(phase1, phase2, blend);
}

// Cornette-Shanks phase function (improved HG for clouds)
float phaseCornetteShanks(float cosTheta, float g) {
    float g2 = g * g;
    float cos2 = cosTheta * cosTheta;
    float num = 3.0 * (1.0 - g2) * (1.0 + cos2);
    float denom = 8.0 * VOL_PI * (2.0 + g2) * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
    return num / denom;
}

// Schlick phase function (fast approximation)
float phaseSchlick(float cosTheta, float k) {
    float tmp = 1.0 + k * cosTheta;
    return VOL_INV_4PI * (1.0 - k * k) / (tmp * tmp);
}

// Rayleigh phase function (for atmospheric scattering)
float phaseRayleigh(float cosTheta) {
    return (3.0 / (16.0 * VOL_PI)) * (1.0 + cosTheta * cosTheta);
}

// ============================================================================
// Beer-Lambert Law - Light Extinction
// ============================================================================

// Basic Beer-Lambert transmittance
float beerLambert(float density, float stepSize) {
    return exp(-density * stepSize);
}

// Beer-Powder approximation for clouds (dark edges effect)
// This models the fact that light must travel through more cloud to illuminate
// the interior of the cloud
float beerPowder(float density, float stepSize) {
    float beer = beerLambert(density, stepSize);
    float powder = 1.0 - exp(-density * stepSize * 2.0);
    return beer * powder;
}

// Energy-conserving Beer-Powder (Schneider's approximation from Horizon)
float beerPowderEnergy(float density, float cosTheta) {
    float powder = 1.0 - exp(-density * 2.0);
    // Only apply powder effect when looking away from the sun
    return mix(1.0, powder, saturate(-cosTheta * 0.5 + 0.5));
}

// ============================================================================
// Noise Functions
// ============================================================================

// Simple hash function for procedural noise
float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash12(float2 p) {
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(float3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float3 hash33(float3 p3) {
    p3 = fract(p3 * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}

// Value noise 3D
float valueNoise3D(float3 p) {
    float3 pi = floor(p);
    float3 pf = fract(p);

    // Smooth interpolation
    float3 w = pf * pf * (3.0 - 2.0 * pf);

    return mix(
        mix(
            mix(hash13(pi + float3(0, 0, 0)), hash13(pi + float3(1, 0, 0)), w.x),
            mix(hash13(pi + float3(0, 1, 0)), hash13(pi + float3(1, 1, 0)), w.x),
            w.y
        ),
        mix(
            mix(hash13(pi + float3(0, 0, 1)), hash13(pi + float3(1, 0, 1)), w.x),
            mix(hash13(pi + float3(0, 1, 1)), hash13(pi + float3(1, 1, 1)), w.x),
            w.y
        ),
        w.z
    );
}

// Gradient noise (Perlin-like)
float gradientNoise3D(float3 p) {
    float3 pi = floor(p);
    float3 pf = fract(p);

    // Quintic interpolation
    float3 w = pf * pf * pf * (pf * (pf * 6.0 - 15.0) + 10.0);

    // Generate gradients
    float3 g000 = hash33(pi + float3(0, 0, 0)) * 2.0 - 1.0;
    float3 g100 = hash33(pi + float3(1, 0, 0)) * 2.0 - 1.0;
    float3 g010 = hash33(pi + float3(0, 1, 0)) * 2.0 - 1.0;
    float3 g110 = hash33(pi + float3(1, 1, 0)) * 2.0 - 1.0;
    float3 g001 = hash33(pi + float3(0, 0, 1)) * 2.0 - 1.0;
    float3 g101 = hash33(pi + float3(1, 0, 1)) * 2.0 - 1.0;
    float3 g011 = hash33(pi + float3(0, 1, 1)) * 2.0 - 1.0;
    float3 g111 = hash33(pi + float3(1, 1, 1)) * 2.0 - 1.0;

    // Dot products
    float n000 = dot(g000, pf - float3(0, 0, 0));
    float n100 = dot(g100, pf - float3(1, 0, 0));
    float n010 = dot(g010, pf - float3(0, 1, 0));
    float n110 = dot(g110, pf - float3(1, 1, 0));
    float n001 = dot(g001, pf - float3(0, 0, 1));
    float n101 = dot(g101, pf - float3(1, 0, 1));
    float n011 = dot(g011, pf - float3(0, 1, 1));
    float n111 = dot(g111, pf - float3(1, 1, 1));

    // Interpolate
    return mix(
        mix(mix(n000, n100, w.x), mix(n010, n110, w.x), w.y),
        mix(mix(n001, n101, w.x), mix(n011, n111, w.x), w.y),
        w.z
    );
}

// Worley noise (cellular noise)
float worleyNoise3D(float3 p) {
    float3 pi = floor(p);
    float3 pf = fract(p);

    float minDist = 1.0;

    for (int z = -1; z <= 1; z++) {
        for (int y = -1; y <= 1; y++) {
            for (int x = -1; x <= 1; x++) {
                float3 offset = float3(x, y, z);
                float3 cellPoint = hash33(pi + offset);
                float3 diff = offset + cellPoint - pf;
                float dist = dot(diff, diff);
                minDist = min(minDist, dist);
            }
        }
    }

    return sqrt(minDist);
}

// Fractal Brownian Motion
float fbm3D(float3 p, int octaves, float lacunarity, float gain) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * valueNoise3D(p * frequency);
        amplitude *= gain;
        frequency *= lacunarity;
    }

    return value;
}

// Tileable 3D noise using texture lookup
float sampleNoise3D(texture3d<float> noiseTex, sampler s, float3 p) {
    return noiseTex.sample(s, p).r;
}

// ============================================================================
// Ray-Geometry Intersection
// ============================================================================

// Ray-sphere intersection
// Returns (tNear, tFar), or (-1, -1) if no intersection
float2 raySphereIntersect(float3 rayOrigin, float3 rayDir, float3 sphereCenter, float sphereRadius) {
    float3 oc = rayOrigin - sphereCenter;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float h = b * b - c;

    if (h < 0.0) {
        return float2(-1.0, -1.0);
    }

    h = sqrt(h);
    return float2(-b - h, -b + h);
}

// Ray-box intersection (AABB)
float2 rayBoxIntersect(float3 rayOrigin, float3 rayDirInv, float3 boxMin, float3 boxMax) {
    float3 t0 = (boxMin - rayOrigin) * rayDirInv;
    float3 t1 = (boxMax - rayOrigin) * rayDirInv;
    float3 tmin = min(t0, t1);
    float3 tmax = max(t0, t1);

    float tNear = max(max(tmin.x, tmin.y), tmin.z);
    float tFar = min(min(tmax.x, tmax.y), tmax.z);

    return float2(tNear, tFar);
}

// ============================================================================
// Utility Functions
// ============================================================================

// Remap value from one range to another
float remap(float value, float inMin, float inMax, float outMin, float outMax) {
    return outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin);
}

// Smooth minimum (for blending shapes)
float smin(float a, float b, float k) {
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// Height-based fog density
float heightFogDensity(float height, float fogBaseHeight, float fogFalloff) {
    return exp(-max(0.0, height - fogBaseHeight) * fogFalloff);
}

// Exponential height fog
float exponentialHeightFog(float distance, float height, float fogDensity, float fogHeightFalloff) {
    float fogAmount = fogDensity * exp(-height * fogHeightFalloff) * (1.0 - exp(-distance * fogHeightFalloff)) / fogHeightFalloff;
    return 1.0 - exp(-fogAmount);
}

// ============================================================================
// Temporal Dithering / Blue Noise
// ============================================================================

// Interleaved gradient noise for temporal stability
float interleavedGradientNoise(float2 screenPos, uint frameIndex) {
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos + float(frameIndex) * 5.588238, magic.xy)));
}

// Blue noise offset for ray marching
float blueNoiseOffset(float2 uv, uint frameIndex, texture2d<float> blueNoiseTex, sampler s) {
    float2 noiseUV = fract(uv * 256.0 + float(frameIndex % 64) * 0.618);
    return blueNoiseTex.sample(s, noiseUV).r;
}

// Simple temporal jitter without blue noise texture
float temporalJitter(float2 screenPos, uint frameIndex) {
    return interleavedGradientNoise(screenPos, frameIndex);
}

// ============================================================================
// Color / Light Utilities
// ============================================================================

// Luminance calculation
float luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Ambient light approximation for fog
float3 ambientFogLight(float3 skyColor, float3 groundColor, float height, float skyBlend) {
    return mix(groundColor, skyColor, saturate(height * skyBlend + 0.5));
}

// Sun disc for volumetric lighting
float sunDisc(float3 viewDir, float3 sunDir, float sunSize) {
    float cosAngle = dot(viewDir, sunDir);
    float sunAngle = acos(saturate(cosAngle));
    return smoothstep(sunSize, sunSize * 0.5, sunAngle);
}

#endif // VOLUMETRIC_COMMON_METAL
