#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// Sky capture shader - renders atmosphere to a cubemap face
// Used for IBL (Image-Based Lighting)

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
    float3 groundColor;
};

struct IBLCaptureData {
    float4x4 viewProj;
    uint faceIndex;
    float roughness;
    float _pad[2];
};

// Physical constants (matching 3d_atmosphere.metal)
constant float3 OZONE_ABSORPTION = float3(3.426, 8.298, 0.356) * 0.06 * 1e-5;
constant float OZONE_SCALE_HEIGHT = 8000.0;
constant float MIE_EXTINCTION_FACTOR = 1.11;

// Full screen triangle vertices
constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct VertexOut {
    float4 position [[position]];
    float3 localPos;
};

// Ray-sphere intersection
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
    float3 rayOrigin,
    float3 rayDir,
    float3 sunDir,
    float sunIntensity,
    float3 sunColor,
    float planetRadius,
    float atmosphereRadius,
    float3 rayleighCoeff,
    float mieCoeff,
    float rayleighScale,
    float mieScale,
    float mieG
) {
    const int PRIMARY_STEPS = 16;
    const int SECONDARY_STEPS = 8;

    float3 planetCenter = float3(0.0, -planetRadius, 0.0);

    float2 atmosphereHit = raySphereIntersect(rayOrigin, rayDir, planetCenter, atmosphereRadius);
    if (atmosphereHit.y < 0.0) {
        return float3(0.0);
    }

    float2 planetHit = raySphereIntersect(rayOrigin, rayDir, planetCenter, planetRadius);
    float rayStart = max(atmosphereHit.x, 0.0);
    float rayEnd = atmosphereHit.y;

    if (planetHit.x > 0.0) {
        rayEnd = planetHit.x;
    }

    float stepSize = (rayEnd - rayStart) / float(PRIMARY_STEPS);

    float3 rayleighAccum = float3(0.0);
    float3 mieAccum = float3(0.0);
    float rayleighOpticalDepth = 0.0;
    float mieOpticalDepth = 0.0;
    float ozoneOpticalDepth = 0.0;

    for (int i = 0; i < PRIMARY_STEPS; i++) {
        float3 samplePos = rayOrigin + rayDir * (rayStart + stepSize * (float(i) + 0.5));
        float sampleHeight = length(samplePos - planetCenter) - planetRadius;

        float rayleighDensity = exp(-sampleHeight / rayleighScale) * stepSize;
        float mieDensity = exp(-sampleHeight / mieScale) * stepSize;
        float ozoneDensity = exp(-sampleHeight / OZONE_SCALE_HEIGHT) * stepSize;

        rayleighOpticalDepth += rayleighDensity;
        mieOpticalDepth += mieDensity;
        ozoneOpticalDepth += ozoneDensity;

        float2 sunAtmosphereHit = raySphereIntersect(samplePos, sunDir, planetCenter, atmosphereRadius);

        if (sunAtmosphereHit.y > 0.0) {
            float sunRayLength = sunAtmosphereHit.y;
            float sunStepSize = sunRayLength / float(SECONDARY_STEPS);

            float rayleighOpticalDepthLight = 0.0;
            float mieOpticalDepthLight = 0.0;
            float ozoneOpticalDepthLight = 0.0;

            for (int j = 0; j < SECONDARY_STEPS; j++) {
                float3 lightSamplePos = samplePos + sunDir * sunStepSize * (float(j) + 0.5);
                float lightSampleHeight = length(lightSamplePos - planetCenter) - planetRadius;

                if (lightSampleHeight < 0.0) {
                    rayleighOpticalDepthLight = 1e10;
                    break;
                }

                rayleighOpticalDepthLight += exp(-lightSampleHeight / rayleighScale) * sunStepSize;
                mieOpticalDepthLight += exp(-lightSampleHeight / mieScale) * sunStepSize;
                ozoneOpticalDepthLight += exp(-lightSampleHeight / OZONE_SCALE_HEIGHT) * sunStepSize;
            }

            // Calculate attenuation with ozone absorption and Mie extinction factor
            float3 attenuation = exp(
                -rayleighCoeff * (rayleighOpticalDepth + rayleighOpticalDepthLight) -
                mieCoeff * MIE_EXTINCTION_FACTOR * (mieOpticalDepth + mieOpticalDepthLight) -
                OZONE_ABSORPTION * (ozoneOpticalDepth + ozoneOpticalDepthLight)
            );

            rayleighAccum += rayleighDensity * attenuation;
            mieAccum += mieDensity * attenuation;
        }
    }

    float cosTheta = dot(rayDir, sunDir);
    float rayleighPhaseVal = rayleighPhase(cosTheta);
    float miePhaseVal = miePhase(cosTheta, mieG);

    float3 rayleigh = rayleighAccum * rayleighCoeff * rayleighPhaseVal;
    float3 mie = mieAccum * mieCoeff * miePhaseVal;

    return sunIntensity * sunColor * (rayleigh + mie);
}

// Convert cubemap face UV to world direction
float3 uvToDirection(float2 uv, uint face) {
    // UV is in [0,1], convert to [-1,1]
    float2 st = uv * 2.0 - 1.0;

    float3 dir;
    switch (face) {
        case 0: dir = float3( 1.0, -st.y, -st.x); break; // +X
        case 1: dir = float3(-1.0, -st.y,  st.x); break; // -X
        case 2: dir = float3( st.x,  1.0,  st.y); break; // +Y
        case 3: dir = float3( st.x, -1.0, -st.y); break; // -Y
        case 4: dir = float3( st.x, -st.y,  1.0); break; // +Z
        case 5: dir = float3(-st.x, -st.y, -1.0); break; // -Z
    }

    return normalize(dir);
}

vertex VertexOut vertexMain(
    uint vertexID [[vertex_id]],
    constant IBLCaptureData& capture [[buffer(0)]]
) {
    VertexOut out;
    out.position = float4(ndcVerts[vertexID], 0.0, 1.0);

    // Calculate UV from NDC
    float2 uv = ndcVerts[vertexID] * 0.5 + 0.5;

    // Convert to world direction for this cubemap face
    out.localPos = uvToDirection(uv, capture.faceIndex);

    return out;
}

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    constant AtmosphereData& atmosphere [[buffer(0)]]
) {
    float3 rayDir = normalize(in.localPos);
    float3 rayOrigin = float3(0.0, 1.0, 0.0); // 1 meter above ground

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

    // Apply exposure (but keep HDR values for IBL)
    color = 1.0 - exp(-atmosphere.exposure * color);

    // Add sun disk
    float sunDot = dot(rayDir, normalize(atmosphere.sunDirection));
    float sunDisk = smoothstep(0.9995, 0.9999, sunDot);
    color += atmosphere.sunColor * atmosphere.sunIntensity * sunDisk * 0.5;

    return float4(color, 1.0);
}
