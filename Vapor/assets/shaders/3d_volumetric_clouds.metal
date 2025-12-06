#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"
#include "assets/shaders/3d_volumetric_common.metal"

// ============================================================================
// Volumetric Clouds - Ray Marched Implementation
// ============================================================================
// Based on techniques from Horizon Zero Dawn and other modern games.
// Features:
// - Procedural cloud shapes using layered noise
// - Weather map for coverage control
// - Multi-scattering approximation (silver lining)
// - Temporal reprojection for performance

// ============================================================================
// Data Structures
// ============================================================================

struct VolumetricCloudData {
    float4x4 invViewProj;           // Inverse view-projection matrix
    float4x4 prevViewProj;          // Previous frame view-projection
    float3 cameraPosition;          // Camera world position
    float _pad1;
    float3 sunDirection;            // Sun direction (normalized)
    float _pad2;
    float3 sunColor;                // Sun color
    float sunIntensity;             // Sun light intensity

    // Cloud layer bounds (world space heights)
    float cloudLayerBottom;         // Bottom of cloud layer (e.g., 1500m)
    float cloudLayerTop;            // Top of cloud layer (e.g., 4000m)
    float cloudLayerThickness;      // = top - bottom
    float _pad3;

    // Cloud shape parameters
    float cloudCoverage;            // Global coverage (0-1)
    float cloudDensity;             // Density multiplier
    float cloudType;                // Cloud type blend (0=stratus, 1=cumulus)
    float erosionStrength;          // Detail erosion strength

    // Noise scales
    float shapeNoiseScale;          // Scale for base shape noise
    float detailNoiseScale;         // Scale for detail noise
    float curlNoiseScale;           // Scale for curl noise (distortion)
    float curlNoiseStrength;        // Strength of curl distortion

    // Lighting
    float ambientIntensity;         // Ambient light from sky
    float silverLiningIntensity;    // Multi-scatter silver lining
    float silverLiningSpread;       // Spread of silver lining effect
    float phaseG1;                  // Forward scatter g

    float phaseG2;                  // Back scatter g
    float phaseBlend;               // Blend between phases
    float powderStrength;           // Beer-powder effect strength
    float _pad4;

    // Animation
    float3 windDirection;           // Wind direction
    float windSpeed;                // Wind speed
    float3 windOffset;              // Accumulated wind offset
    float time;                     // Current time

    // Ray marching
    uint primarySteps;              // Primary ray march steps
    uint lightSteps;                // Light ray march steps
    float2 screenSize;              // Screen dimensions

    // Temporal
    uint frameIndex;                // Frame counter
    float temporalBlend;            // TAA blend factor
    float2 _pad5;
};

// ============================================================================
// Cloud Density Functions
// ============================================================================

// Height gradient for cloud type
// Returns density multiplier based on height within cloud layer and cloud type
float cloudHeightGradient(float heightFraction, float cloudType) {
    // Stratus clouds: thin, flat layers
    float stratus = remap(heightFraction, 0.0, 0.1, 0.0, 1.0) * remap(heightFraction, 0.2, 0.3, 1.0, 0.0);

    // Stratocumulus: medium height, rounded tops
    float stratocumulus = remap(heightFraction, 0.0, 0.1, 0.0, 1.0) * remap(heightFraction, 0.4, 0.6, 1.0, 0.0);

    // Cumulus: tall, puffy clouds
    float cumulus = remap(heightFraction, 0.0, 0.1, 0.0, 1.0) * remap(heightFraction, 0.7, 0.95, 1.0, 0.0);

    // Blend based on cloud type
    float gradient = mix(stratus, cumulus, cloudType);
    gradient = mix(gradient, stratocumulus, saturate(cloudType * 2.0) * (1.0 - saturate(cloudType * 2.0 - 1.0)));

    return saturate(gradient);
}

// Sample base cloud shape
float sampleCloudShape(float3 worldPos, constant VolumetricCloudData& data) {
    // Apply wind offset
    float3 samplePos = worldPos + data.windOffset;

    // Sample base shape noise (low frequency)
    float3 shapeUV = samplePos * data.shapeNoiseScale * 0.0001;  // Scale to reasonable UV

    // Procedural noise combination (Perlin-Worley)
    float perlin = gradientNoise3D(shapeUV * 4.0) * 0.5 + 0.5;
    float worley1 = 1.0 - worleyNoise3D(shapeUV * 4.0);
    float worley2 = 1.0 - worleyNoise3D(shapeUV * 8.0);
    float worley3 = 1.0 - worleyNoise3D(shapeUV * 16.0);

    // Combine Worley octaves
    float worleyFBM = worley1 * 0.625 + worley2 * 0.25 + worley3 * 0.125;

    // Perlin-Worley: use Perlin to warp Worley
    float baseShape = remap(perlin, worleyFBM - 1.0, 1.0, 0.0, 1.0);

    return saturate(baseShape);
}

// Sample cloud detail
float sampleCloudDetail(float3 worldPos, constant VolumetricCloudData& data) {
    // Apply wind (detail moves faster)
    float3 samplePos = worldPos + data.windOffset * 1.5;

    // High frequency detail noise
    float3 detailUV = samplePos * data.detailNoiseScale * 0.001;

    // Multi-octave Worley for detail
    float detail1 = 1.0 - worleyNoise3D(detailUV * 2.0);
    float detail2 = 1.0 - worleyNoise3D(detailUV * 4.0);
    float detail3 = 1.0 - worleyNoise3D(detailUV * 8.0);

    float detail = detail1 * 0.625 + detail2 * 0.25 + detail3 * 0.125;

    return detail;
}

// Sample weather map (procedural for now, could be texture)
float2 sampleWeather(float3 worldPos, constant VolumetricCloudData& data) {
    float2 weatherUV = worldPos.xz * 0.00005 + data.time * 0.001;

    // Coverage from low-frequency noise
    float coverage = valueNoise3D(float3(weatherUV * 3.0, 0.0));
    coverage = coverage * 0.5 + 0.5;
    coverage = pow(coverage, 0.5);  // Boost coverage

    // Cloud type from different noise
    float cloudType = valueNoise3D(float3(weatherUV * 2.0 + 100.0, 0.0));
    cloudType = cloudType * 0.5 + 0.5;

    return float2(coverage * data.cloudCoverage, cloudType);
}

// Main cloud density function
float sampleCloudDensity(float3 worldPos, constant VolumetricCloudData& data, bool useCheap) {
    // Calculate height fraction within cloud layer
    float height = worldPos.y;
    float heightFraction = saturate((height - data.cloudLayerBottom) / data.cloudLayerThickness);

    // Outside cloud layer
    if (heightFraction <= 0.0 || heightFraction >= 1.0) {
        return 0.0;
    }

    // Sample weather
    float2 weather = sampleWeather(worldPos, data);
    float coverage = weather.x;
    float cloudType = mix(weather.y, data.cloudType, 0.5);

    // Height gradient
    float heightGradient = cloudHeightGradient(heightFraction, cloudType);

    // Base shape
    float baseShape = sampleCloudShape(worldPos, data);

    // Apply coverage (remapping creates hard edges)
    float baseCloud = remap(baseShape * heightGradient, 1.0 - coverage, 1.0, 0.0, 1.0);
    baseCloud = saturate(baseCloud);

    if (useCheap || baseCloud <= 0.0) {
        return baseCloud * data.cloudDensity;
    }

    // Detail erosion (expensive, only for primary rays)
    float detail = sampleCloudDetail(worldPos, data);

    // Erode edges with detail noise
    float erosion = data.erosionStrength * (1.0 - heightFraction) * 0.5;
    float finalDensity = remap(baseCloud, detail * erosion, 1.0, 0.0, 1.0);

    return saturate(finalDensity) * data.cloudDensity;
}

// ============================================================================
// Lighting Functions
// ============================================================================

// March towards sun to calculate shadow/transmittance
float lightMarch(float3 worldPos, constant VolumetricCloudData& data) {
    float stepSize = data.cloudLayerThickness / float(data.lightSteps);
    float3 lightDir = data.sunDirection;

    float transmittance = 1.0;
    float3 pos = worldPos;

    for (uint i = 0; i < data.lightSteps; i++) {
        pos += lightDir * stepSize;

        // Early exit if above cloud layer
        if (pos.y > data.cloudLayerTop) break;

        float density = sampleCloudDensity(pos, data, true);  // Cheap sample for light march
        transmittance *= beerLambert(density, stepSize);

        // Early exit if fully occluded
        if (transmittance < 0.01) break;
    }

    return transmittance;
}

// Multi-scattering approximation (Schneider's method)
float3 multiScatterApprox(float density, float lightTransmittance, float cosTheta,
                          constant VolumetricCloudData& data) {
    // Direct light with phase function
    float phase = phaseDualLobe(cosTheta, data.phaseG1, data.phaseG2, data.phaseBlend);
    float3 directLight = data.sunColor * data.sunIntensity * phase * lightTransmittance;

    // Multi-scatter approximation (octaves of scattering)
    // Each bounce: dimmer, more isotropic, less shadowed
    float3 multiScatter = float3(0.0);
    float attenuation = 0.3;
    float contribution = 0.4;
    float phaseAttenuation = 0.5;

    float scatterPhase = phase;
    float scatterTransmittance = lightTransmittance;

    for (int i = 0; i < 4; i++) {
        // Each bounce gets more ambient-like
        scatterPhase = mix(scatterPhase, 0.25, phaseAttenuation);  // More isotropic
        scatterTransmittance = mix(scatterTransmittance, 1.0, 0.7);  // Less shadow

        multiScatter += contribution * scatterPhase * scatterTransmittance * data.sunColor;

        contribution *= attenuation;
    }

    // Silver lining effect (bright edges when backlit)
    float silverLining = pow(saturate(1.0 - lightTransmittance), data.silverLiningSpread);
    silverLining *= saturate(-cosTheta * 0.5 + 0.5);  // Only when looking at sun
    multiScatter += data.sunColor * data.silverLiningIntensity * silverLining;

    // Ambient sky light
    float3 ambient = float3(0.5, 0.6, 0.9) * data.ambientIntensity;

    return directLight + multiScatter * data.sunIntensity + ambient;
}

// ============================================================================
// Ray Marching
// ============================================================================

// Find intersection with cloud layer
float2 cloudLayerIntersection(float3 rayOrigin, float3 rayDir, constant VolumetricCloudData& data) {
    // Intersect with two horizontal planes
    float tBottom = (data.cloudLayerBottom - rayOrigin.y) / rayDir.y;
    float tTop = (data.cloudLayerTop - rayOrigin.y) / rayDir.y;

    float tMin = min(tBottom, tTop);
    float tMax = max(tBottom, tTop);

    // Handle camera inside cloud layer
    if (rayOrigin.y >= data.cloudLayerBottom && rayOrigin.y <= data.cloudLayerTop) {
        tMin = 0.0;
    }

    return float2(max(0.0, tMin), max(0.0, tMax));
}

// Main cloud ray march
float4 raymarchClouds(float3 rayOrigin, float3 rayDir, float maxDist,
                      constant VolumetricCloudData& data, float blueNoise) {
    // Find cloud layer intersection
    float2 tRange = cloudLayerIntersection(rayOrigin, rayDir, data);

    if (tRange.y <= tRange.x || tRange.x > maxDist) {
        return float4(0.0, 0.0, 0.0, 1.0);  // No intersection
    }

    // Clamp to max distance
    tRange.y = min(tRange.y, maxDist);

    // Calculate step size
    float rayLength = tRange.y - tRange.x;
    float stepSize = rayLength / float(data.primarySteps);

    // Apply blue noise offset for temporal stability
    float t = tRange.x + stepSize * blueNoise;

    // Accumulation
    float3 scattering = float3(0.0);
    float transmittance = 1.0;

    // View-sun angle for lighting
    float cosTheta = dot(rayDir, data.sunDirection);

    // Ray march through clouds
    for (uint i = 0; i < data.primarySteps && t < tRange.y; i++) {
        float3 pos = rayOrigin + rayDir * t;

        float density = sampleCloudDensity(pos, data, false);

        if (density > 0.001) {
            // Calculate lighting
            float lightTransmittance = lightMarch(pos, data);

            // Multi-scattering approximation
            float3 luminance = multiScatterApprox(density, lightTransmittance, cosTheta, data);

            // Beer-powder effect
            float powder = beerPowderEnergy(density * stepSize * 10.0, cosTheta) * data.powderStrength +
                          (1.0 - data.powderStrength);

            // Integrate
            float stepTransmittance = beerLambert(density, stepSize);
            float3 stepScattering = luminance * (1.0 - stepTransmittance) * powder;

            scattering += transmittance * stepScattering;
            transmittance *= stepTransmittance;

            // Early exit
            if (transmittance < 0.01) {
                transmittance = 0.0;
                break;
            }
        }

        t += stepSize;
    }

    return float4(scattering, transmittance);
}

// ============================================================================
// Render Passes
// ============================================================================

struct CloudVertexOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 cloudTriVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

vertex CloudVertexOut cloudVertex(uint vertexID [[vertex_id]]) {
    CloudVertexOut out;
    out.position = float4(cloudTriVerts[vertexID], 0.0, 1.0);
    out.uv = cloudTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;
    return out;
}

fragment float4 cloudFragment(
    CloudVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    constant VolumetricCloudData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    // Sample scene
    float4 color = sceneColor.sample(linearSampler, in.uv);
    float depth = sceneDepth.sample(linearSampler, in.uv).r;

    // Calculate ray direction
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 worldDir4 = data.invViewProj * clipPos;
    float3 rayDir = normalize(worldDir4.xyz / worldDir4.w - data.cameraPosition);

    // Calculate max distance from depth
    float linearDepth = camera.near * camera.far / (camera.far - depth * (camera.far - camera.near));
    float maxDist = (depth >= 0.9999) ? 100000.0 : linearDepth;

    // Blue noise for temporal jitter
    float blueNoise = temporalJitter(in.position.xy, data.frameIndex);

    // Ray march clouds
    float4 cloudData = raymarchClouds(data.cameraPosition, rayDir, maxDist, data, blueNoise);

    // Composite
    float3 result = color.rgb * cloudData.a + cloudData.rgb;

    return float4(result, 1.0);
}

// ============================================================================
// Low Resolution Pass (Quarter resolution for performance)
// ============================================================================

fragment float4 cloudFragmentLowRes(
    CloudVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneDepth [[texture(0)]],
    constant VolumetricCloudData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float depth = sceneDepth.sample(linearSampler, in.uv).r;

    // Calculate ray direction
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 worldDir4 = data.invViewProj * clipPos;
    float3 rayDir = normalize(worldDir4.xyz / worldDir4.w - data.cameraPosition);

    // Calculate max distance
    float linearDepth = camera.near * camera.far / (camera.far - depth * (camera.far - camera.near));
    float maxDist = (depth >= 0.9999) ? 100000.0 : linearDepth;

    // Blue noise
    float blueNoise = temporalJitter(in.position.xy, data.frameIndex);

    // Ray march
    float4 cloudData = raymarchClouds(data.cameraPosition, rayDir, maxDist, data, blueNoise);

    return cloudData;
}

// ============================================================================
// Temporal Reprojection Pass
// ============================================================================

fragment float4 cloudTemporalResolve(
    CloudVertexOut in [[stage_in]],
    texture2d<float, access::sample> currentCloud [[texture(0)]],
    texture2d<float, access::sample> historyCloud [[texture(1)]],
    texture2d<float, access::sample> sceneDepth [[texture(2)]],
    texture2d<float, access::sample> velocityBuffer [[texture(3)]],
    constant VolumetricCloudData& data [[buffer(0)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float4 current = currentCloud.sample(linearSampler, in.uv);

    // Simple motion vectors from depth reprojection
    float depth = sceneDepth.sample(linearSampler, in.uv).r;
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = data.invViewProj * clipPos;
    worldPos /= worldPos.w;

    // Reproject to previous frame
    float4 prevClip = data.prevViewProj * worldPos;
    float2 prevUV = prevClip.xy / prevClip.w * 0.5 + 0.5;
    prevUV.y = 1.0 - prevUV.y;

    // Sample history
    float4 history = historyCloud.sample(linearSampler, prevUV);

    // Validity check
    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                        prevUV.y >= 0.0 && prevUV.y <= 1.0;

    // Neighborhood clamping for anti-ghosting
    float4 minBound = current;
    float4 maxBound = current;

    // Sample neighbors (simplified - full implementation would use 3x3)
    float2 texelSize = 1.0 / data.screenSize;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float4 neighbor = currentCloud.sample(linearSampler, in.uv + float2(x, y) * texelSize);
            minBound = min(minBound, neighbor);
            maxBound = max(maxBound, neighbor);
        }
    }

    // Clamp history to neighborhood
    history = clamp(history, minBound, maxBound);

    // Blend
    float blend = validHistory ? data.temporalBlend : 1.0;
    return mix(history, current, blend);
}

// ============================================================================
// Upscale and Composite Pass
// ============================================================================

fragment float4 cloudUpscaleComposite(
    CloudVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> cloudTexture [[texture(1)]],
    texture2d<float, access::sample> sceneDepth [[texture(2)]],
    constant VolumetricCloudData& data [[buffer(0)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float4 scene = sceneColor.sample(linearSampler, in.uv);
    float4 cloud = cloudTexture.sample(linearSampler, in.uv);

    // Depth-aware upscale could be added here for better quality

    // Composite: scene * transmittance + scattering
    float3 result = scene.rgb * cloud.a + cloud.rgb;

    return float4(result, scene.a);
}
