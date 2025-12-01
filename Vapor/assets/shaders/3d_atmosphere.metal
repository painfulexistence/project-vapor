#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// Atmosphere rendering using Rayleigh and Mie scattering
// Based on: https://cpp-rendering.io/sky-and-atmosphere-rendering/
// Implements physically-based sky rendering with Rayleigh, Mie, and Ozone

struct AtmosphereData {
    float3 sunDirection;
    float3 sunColor;
    float sunIntensity;
    float planetRadius;
    float atmosphereRadius;
    float exposure;
    float3 rayleighCoefficients;
    float rayleighScaleHeight;
    float mieCoefficient;
    float mieScaleHeight;
    float miePreferredDirection;
    float3 groundColor;  // Ground color for horizon and IBL
};

// Physical constants
constant float3 OZONE_ABSORPTION = float3(3.426, 8.298, 0.356) * 0.06 * 1e-5;  // Ozone absorption
constant float OZONE_SCALE_HEIGHT = 8000.0;  // Same as Rayleigh for simplicity
constant float MIE_EXTINCTION_FACTOR = 1.11;  // Mie has ~10% absorption

// Full screen triangle vertices
constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct SkyVertexOut {
    float4 position [[position]];
    float2 uv;
};

// Ray-sphere intersection
// Returns distance to intersection or -1 if no intersection
float2 raySphereIntersect(float3 rayOrigin, float3 rayDir, float3 sphereCenter, float sphereRadius) {
    float3 oc = rayOrigin - sphereCenter;
    float a = dot(rayDir, rayDir);
    float b = 2.0 * dot(oc, rayDir);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - 4.0 * a * c;

    if (discriminant < 0.0) {
        return float2(-1.0);
    }

    float sqrtDisc = sqrt(discriminant);
    return float2(
        (-b - sqrtDisc) / (2.0 * a),
        (-b + sqrtDisc) / (2.0 * a)
    );
}

// Rayleigh phase function
float rayleighPhase(float cosTheta) {
    return 3.0 / (16.0 * PI) * (1.0 + cosTheta * cosTheta);
}

// Henyey-Greenstein phase function for Mie scattering
float miePhase(float cosTheta, float g) {
    float g2 = g * g;
    float num = 3.0 * (1.0 - g2) * (1.0 + cosTheta * cosTheta);
    float denom = 8.0 * PI * (2.0 + g2) * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
    return num / denom;
}

// Compute atmospheric scattering
float3 computeAtmosphere(
    float3 rayOrigin,      // Ray origin (camera position)
    float3 rayDir,         // Normalized ray direction
    float3 sunDir,         // Normalized sun direction
    float sunIntensity,    // Sun intensity
    float3 sunColor,       // Sun color
    float planetRadius,    // Planet radius
    float atmosphereRadius,// Atmosphere radius
    float3 rayleighCoeff,  // Rayleigh scattering coefficients
    float mieCoeff,        // Mie scattering coefficient
    float rayleighScale,   // Rayleigh scale height
    float mieScale,        // Mie scale height
    float mieG             // Mie preferred scattering direction
) {
    const int PRIMARY_STEPS = 16;
    const int SECONDARY_STEPS = 8;

    float3 planetCenter = float3(0.0, -planetRadius, 0.0);

    // Calculate intersection with atmosphere
    float2 atmosphereHit = raySphereIntersect(rayOrigin, rayDir, planetCenter, atmosphereRadius);

    if (atmosphereHit.y < 0.0) {
        return float3(0.0); // No intersection
    }

    // Check if we hit the planet
    float2 planetHit = raySphereIntersect(rayOrigin, rayDir, planetCenter, planetRadius);

    // Clamp to start at camera if inside atmosphere
    float rayStart = max(atmosphereHit.x, 0.0);
    float rayEnd = atmosphereHit.y;

    // If we hit the planet, end the ray there
    if (planetHit.x > 0.0) {
        rayEnd = planetHit.x;
    }

    float stepSize = (rayEnd - rayStart) / float(PRIMARY_STEPS);

    // Accumulators
    float3 rayleighAccum = float3(0.0);
    float3 mieAccum = float3(0.0);
    float rayleighOpticalDepth = 0.0;
    float mieOpticalDepth = 0.0;
    float ozoneOpticalDepth = 0.0;

    // March along the primary ray
    for (int i = 0; i < PRIMARY_STEPS; i++) {
        float3 samplePos = rayOrigin + rayDir * (rayStart + stepSize * (float(i) + 0.5));
        float sampleHeight = length(samplePos - planetCenter) - planetRadius;

        // Calculate optical depth at this sample
        float rayleighDensity = exp(-sampleHeight / rayleighScale) * stepSize;
        float mieDensity = exp(-sampleHeight / mieScale) * stepSize;
        // Ozone density peaks around 25km altitude with ~15km width
        float ozoneDensity = exp(-sampleHeight / OZONE_SCALE_HEIGHT) * stepSize;

        rayleighOpticalDepth += rayleighDensity;
        mieOpticalDepth += mieDensity;
        ozoneOpticalDepth += ozoneDensity;

        // Calculate light ray optical depth (secondary ray to sun)
        float2 sunAtmosphereHit = raySphereIntersect(samplePos, sunDir, planetCenter, atmosphereRadius);

        if (sunAtmosphereHit.y > 0.0) {
            float sunRayLength = sunAtmosphereHit.y;
            float sunStepSize = sunRayLength / float(SECONDARY_STEPS);

            float rayleighOpticalDepthLight = 0.0;
            float mieOpticalDepthLight = 0.0;
            float ozoneOpticalDepthLight = 0.0;

            // March along the light ray
            for (int j = 0; j < SECONDARY_STEPS; j++) {
                float3 lightSamplePos = samplePos + sunDir * sunStepSize * (float(j) + 0.5);
                float lightSampleHeight = length(lightSamplePos - planetCenter) - planetRadius;

                if (lightSampleHeight < 0.0) {
                    // Below planet surface - in shadow
                    rayleighOpticalDepthLight = 1e10;
                    break;
                }

                rayleighOpticalDepthLight += exp(-lightSampleHeight / rayleighScale) * sunStepSize;
                mieOpticalDepthLight += exp(-lightSampleHeight / mieScale) * sunStepSize;
                ozoneOpticalDepthLight += exp(-lightSampleHeight / OZONE_SCALE_HEIGHT) * sunStepSize;
            }

            // Calculate attenuation with ozone absorption and Mie extinction factor
            // Mie extinction = Mie scattering * 1.11 (accounts for ~10% absorption)
            float3 attenuation = exp(
                -rayleighCoeff * (rayleighOpticalDepth + rayleighOpticalDepthLight) -
                mieCoeff * MIE_EXTINCTION_FACTOR * (mieOpticalDepth + mieOpticalDepthLight) -
                OZONE_ABSORPTION * (ozoneOpticalDepth + ozoneOpticalDepthLight)
            );

            rayleighAccum += rayleighDensity * attenuation;
            mieAccum += mieDensity * attenuation;
        }
    }

    // Calculate phase functions
    float cosTheta = dot(rayDir, sunDir);
    float rayleighPhaseVal = rayleighPhase(cosTheta);
    float miePhaseVal = miePhase(cosTheta, mieG);

    // Combine scattering
    float3 rayleigh = rayleighAccum * rayleighCoeff * rayleighPhaseVal;
    float3 mie = mieAccum * mieCoeff * miePhaseVal;

    return sunIntensity * sunColor * (rayleigh + mie);
}

vertex SkyVertexOut vertexMain(uint vertexID [[vertex_id]]) {
    SkyVertexOut out;
    out.position = float4(ndcVerts[vertexID], 1.0, 1.0); // z = 1.0 for far plane
    out.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y; // flip Y axis because Metal uses Y-down UV space
    return out;
}

fragment float4 fragmentMain(
    SkyVertexOut in [[stage_in]],
    constant CameraData& camera [[buffer(0)]],
    constant AtmosphereData& atmosphere [[buffer(1)]]
) {

    // Reconstruct view ray from UV
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y; // Flip Y for Metal's coordinate system

    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 viewPos = camera.invProj * clipPos;
    viewPos /= viewPos.w;

    float3 rayDir = normalize((camera.invView * float4(viewPos.xyz, 0.0)).xyz);

    // Use actual camera position for ray origin (in world space)
    float3 rayOrigin = camera.position; // Camera position in world space

    // Check if ray points below horizon (towards ground)
    // Planet center is at (0, -planetRadius, 0), so ground is at y = 0
    // If ray would hit the planet surface, compute ground color instead of sky
    float3 planetCenter = float3(0.0, -atmosphere.planetRadius, 0.0);
    float2 planetHit = raySphereIntersect(rayOrigin, rayDir, planetCenter, atmosphere.planetRadius);

    // If ray hits planet surface (below horizon), compute ground color
    // This provides fallback ground color for IBL and areas without geometry
    if (planetHit.x > 0.0) {
        // Calculate simple ground color based on sun angle (Lambertian lighting)
        float3 groundNormal = float3(0.0, 1.0, 0.0); // Ground normal points up
        float3 sunDir = normalize(atmosphere.sunDirection);
        float sunDot = max(0.0, dot(groundNormal, sunDir));

        // Simple Lambertian ground color with sun lighting
        float3 groundColor = atmosphere.groundColor * atmosphere.sunColor * atmosphere.sunIntensity * sunDot * 0.1;

        // Apply exposure (tone mapping)
        groundColor = 1.0 - exp(-atmosphere.exposure * groundColor);

        return float4(groundColor, 1.0);
    }

    // Compute atmosphere color
    float3 color = computeAtmosphere(
        rayOrigin,
        rayDir,
        normalize(atmosphere.sunDirection),
        atmosphere.sunIntensity,
        atmosphere.sunColor,
        atmosphere.planetRadius,
        atmosphere.atmosphereRadius,
        atmosphere.rayleighCoefficients,
        atmosphere.mieCoefficient,
        atmosphere.rayleighScaleHeight,
        atmosphere.mieScaleHeight,
        atmosphere.miePreferredDirection
    );

    // Apply exposure (tone mapping)
    color = 1.0 - exp(-atmosphere.exposure * color);

    // Add sun disk
    float sunDot = dot(rayDir, normalize(atmosphere.sunDirection));
    float sunDisk = smoothstep(0.9995, 0.9999, sunDot);
    color += atmosphere.sunColor * atmosphere.sunIntensity * sunDisk * 0.5;

    return float4(color, 1.0);
}
