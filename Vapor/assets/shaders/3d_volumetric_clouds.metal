#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/3d_volumetric_common.metal"

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
    float3 sunDirection;            // Sun direction (normalized)
    float3 sunColor;                // Sun color
    float sunIntensity;             // Sun light intensity

    // Cloud layer bounds (world space heights)
    float cloudLayerBottom;         // Bottom of cloud layer (e.g., 1500m)
    float cloudLayerTop;            // Top of cloud layer (e.g., 4000m)
    float cloudLayerThickness;      // = top - bottom

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
    float sunLightScale;            // cloud-specific scale on sunIntensity (< 1: clouds occlude)

    // Animation
    float3 windDirection;           // Wind direction
    float3 windOffset;              // Accumulated wind offset
    float windSpeed;                // Wind speed
    float time;                     // Current time

    // Ray marching
    uint primarySteps;              // Primary ray march steps
    uint lightSteps;                // Light ray march steps
    float2 screenSize;              // Screen dimensions
    // MUST stay float2 (twin of the C++ vec2 padding) or frameIndex and
    // temporalBlend shift by 4 bytes and read the wrong fields.
    float2 _pad2;

    // Temporal
    uint frameIndex;                // Frame counter
    float temporalBlend;            // TAA blend factor
    float2 _pad3;

    // Cloud ambient (sky-fill) tint, scaled by ambientIntensity. Weather
    // drives it: blue for clear, neutral gray overcast, storm green.
    // float3 self-pads to 16 B, matching C++ `glm::vec3 ambientColor; float _pad9;`
    // — do NOT add an explicit pad here (that shifts everything below by 16 B).
    float3 ambientColor;
    // Night key light: the moon (moonDir = -sunDirection) takes over as the sun
    // sets. moonLightScale = moon lit brightness as a fraction of the sun term.
    // packed_float3 (12 B) so moonLightScale packs into the vec3's 4th slot,
    // matching C++ `glm::vec3 moonColor; float moonLightScale;` — a plain float3
    // would 16-align moonColor and push moonLightScale to the wrong offset,
    // reading garbage on Metal (night clouds vanish).
    packed_float3 moonColor;
    float moonLightScale;
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

// Baked tileable noise volumes (renderer createCloudNoiseTextures): one
// trilinear fetch replaces the old per-sample procedural Perlin-Worley loops.
// Octave frequencies are baked at the old shader ratios, so the UV scales
// below are unchanged.
constexpr sampler cloudNoiseSampler(address::repeat, filter::linear);

// Baked detail FBM's approximate mean. The distance LOD blends toward it
// rather than toward zero, so far clouds lose the detail VARIANCE but keep its
// average erosion — fading to zero would have made them fatter with distance.
constant float CLOUD_DETAIL_MEAN = 0.5;

// Triangle-wave mirrored repeat (de-tiling; twin of CloudRaymarch.frag).
float2 cloudMirrorRepeat(float2 u) { return abs(2.0 * fract(u * 0.5) - 1.0); }

// Sample base cloud shape (128^3 Perlin-Worley volume). The baked volume
// tiles every 10 km — visibly, at low coverage (the remap keeps ~one peak
// per tile: the same blob on a 10 km grid). De-tiling stack, twin of
// CloudRaymarch.frag: the weather map's BA warp (+-2.5 km, ~13.3 km
// wavelength, coprime with tile and mirror), mirrored-repeat sampling, and
// an aperiodic break-up octave added in sampleCloudDensity.
float sampleCloudShape(float3 worldPos, constant VolumetricCloudData& data,
                       texture3d<float, access::sample> shapeTex, float2 warp) {
    float3 samplePos = worldPos + data.windOffset;
    samplePos.xz += warp;
    float3 uvw = samplePos * (data.shapeNoiseScale * 0.0001);
    uvw.xz = cloudMirrorRepeat(uvw.xz);
    return shapeTex.sample(cloudNoiseSampler, uvw).r;
}

// Curl-ish vector noise: three decorrelated gradient noises. Not a true
// divergence-free curl, but visually equivalent wind-torn wisps at a third
// of the cost of a finite-difference curl. (Twin of CloudRaymarch.frag.)
float3 curlDistort(float3 p) {
    return float3(gradientNoise3D(p),
                  gradientNoise3D(p + float3(31.416, 47.853, 12.793)),
                  gradientNoise3D(p + float3(-23.144, 9.271, 61.043)));
}

// Sample cloud detail (32^3 Worley-FBM volume)
float sampleCloudDetail(float3 worldPos, constant VolumetricCloudData& data,
                        texture3d<float, access::sample> detailTex) {
    float dist = length(worldPos - data.cameraPosition);
    // Distance LOD (twin of CloudRaymarch.frag). The 32^3 volume spans only
    // 200 m of world space; past ~12 km a quarter-res pixel covers more than
    // that, so the octave is subpixel and contributes per-frame noise rather
    // than shape — averaged by the temporal pass into grey mush, and popping
    // as shimmer when rotation drops the history.
    float lodFade = 1.0 - smoothstep(12000.0, 30000.0, dist);
    if (lodFade <= 0.001) return CLOUD_DETAIL_MEAN;

    // Apply wind (detail moves faster)
    float3 samplePos = worldPos + data.windOffset * 1.5;

    // Wind-torn edges: distort the detail lookup with large-scale vector noise
    // (~500 m swirls at curlNoiseScale 1; strength 0.1 → ~30 m displacement).
    // Full-quality samples only — the cheap light-march path skips detail.
    if (data.curlNoiseStrength > 0.0) {
        samplePos += curlDistort(samplePos * (data.curlNoiseScale * 0.002)) *
                     (data.curlNoiseStrength * 300.0);
    }

    float d = detailTex.sample(cloudNoiseSampler, samplePos * (data.detailNoiseScale * 0.001)).r;
    // Close-range octave (twin of CloudRaymarch.frag): same volume at 5x
    // frequency, decorrelated by a UV offset, faded out past ~2.5 km. Signed
    // perturbation keeps the mean erosion (and the far look) unchanged.
    float nearW = 1.0 - smoothstep(800.0, 2500.0, dist);
    if (nearW > 0.01) {
        float hf = detailTex.sample(cloudNoiseSampler,
                                    samplePos * (data.detailNoiseScale * 0.005) + float3(0.37)).r;
        d += (hf - 0.5) * 0.35 * nearW;
    }
    return mix(CLOUD_DETAIL_MEAN, d, lodFade);
}

// Sample the baked weather map — twin of CloudRaymarch.frag. R = coverage
// base, G = type, BA = signed shape de-tiling warp. Scrolls with the wind at
// 0.6x the detail rate. Returns (coverage, type, warp.xy) in metres.
float4 sampleWeather(float3 worldPos, constant VolumetricCloudData& data,
                     texture2d<float, access::sample> weatherTex) {
    float2 weatherUV = (worldPos.xz + data.windOffset.xz * 0.6) * 0.00005 + data.time * 0.0002;
    float4 w = weatherTex.sample(cloudNoiseSampler, weatherUV * 0.5);  // 40 km tile
    return float4(w.r * data.cloudCoverage, w.g, (w.ba * 2.0 - 1.0) * 2500.0);
}

// Main cloud density function. The cheap path never samples detailTex, so
// callers without a detail volume (the shadow map) may pass shapeTex twice.
float sampleCloudDensity(float3 worldPos, constant VolumetricCloudData& data, bool useCheap,
                         texture3d<float, access::sample> shapeTex,
                         texture3d<float, access::sample> detailTex,
                         texture2d<float, access::sample> weatherTex) {
    // Calculate height fraction within cloud layer
    float height = worldPos.y;
    float heightFraction = saturate((height - data.cloudLayerBottom) / data.cloudLayerThickness);

    // Outside cloud layer
    if (heightFraction <= 0.0 || heightFraction >= 1.0) {
        return 0.0;
    }

    // Sample weather
    float4 weather = sampleWeather(worldPos, data, weatherTex);
    float coverage = weather.x;
    float cloudType = mix(weather.y, data.cloudType, 0.5);

    // Height gradient
    float heightGradient = cloudHeightGradient(heightFraction, cloudType);

    // Base shape
    float baseShape = sampleCloudShape(worldPos, data, shapeTex, weather.zw);
    // Aperiodic break-up octave (de-tiling; twin of CloudRaymarch.frag): the
    // unbounded-lattice gradient noise never repeats, so which peaks survive
    // the coverage remap varies region to region instead of per 10 km tile.
    baseShape += gradientNoise3D((worldPos + data.windOffset) * (1.0 / 6000.0)) * 0.15;

    // Apply coverage (remapping creates hard edges). Guard the divide: remap's
    // denominator here IS coverage, which reaches exactly 0 (the weather map's
    // R is 8-bit and bottoms out; the weather system sweeps cloudCoverage
    // through 0 on a state change) and 0/0 is a NaN that saturate() does not
    // launder. It would land in the cloud RT, enter the temporal history and be
    // dragged a texel per frame by reprojection — a permanent streak.
    float cov = max(coverage, 1e-4);
    float baseCloud = remap(baseShape * heightGradient, 1.0 - cov, 1.0, 0.0, 1.0);
    baseCloud = saturate(baseCloud);

    if (useCheap || baseCloud <= 0.0) {
        return baseCloud * data.cloudDensity;
    }

    // Detail erosion (expensive, only for primary rays)
    float detail = sampleCloudDetail(worldPos, data, detailTex);

    // Erode edges with detail noise
    float erosion = data.erosionStrength * (1.0 - heightFraction) * 0.5;
    float finalDensity = remap(baseCloud, detail * erosion, 1.0, 0.0, 1.0);

    return saturate(finalDensity) * data.cloudDensity;
}

// ============================================================================
// Lighting Functions
// ============================================================================

// March toward an arbitrary light (sun by day, moon by night) for shadowing.
float lightMarch(float3 worldPos, float3 lightDir, constant VolumetricCloudData& data,
                 texture3d<float, access::sample> shapeTex,
                 texture3d<float, access::sample> detailTex,
                 texture2d<float, access::sample> weatherTex) {
    float stepSize = data.cloudLayerThickness / float(data.lightSteps);

    float transmittance = 1.0;
    float3 pos = worldPos;

    for (uint i = 0; i < data.lightSteps; i++) {
        pos += lightDir * stepSize;

        // Early exit if above cloud layer
        if (pos.y > data.cloudLayerTop) break;

        float density = sampleCloudDensity(pos, data, true, shapeTex, detailTex, weatherTex);  // Cheap
        transmittance *= beerLambert(density, stepSize);

        // Early exit if fully occluded
        if (transmittance < 0.01) break;
    }

    return transmittance;
}

// Scattering contribution from ONE key light (colour × power), no ambient.
// Called once for the sun and once for the moon; the caller weights each by
// day/night and adds a single ambient term.
float3 scatterFromLight(float3 lightColor, float lightPower, float lightTransmittance,
                        float cosTheta, constant VolumetricCloudData& data) {
    float phase = phaseDualLobe(cosTheta, data.phaseG1, data.phaseG2, data.phaseBlend);
    float3 directLight = lightColor * lightPower * phase * lightTransmittance;

    float3 multiScatter = float3(0.0);
    float attenuation = 0.3;
    float contribution = 0.4;
    float phaseAttenuation = 0.5;

    float scatterPhase = phase;
    float scatterTransmittance = lightTransmittance;

    for (int i = 0; i < 4; i++) {
        scatterPhase = mix(scatterPhase, 0.25, phaseAttenuation);  // More isotropic
        scatterTransmittance = mix(scatterTransmittance, 1.0, 0.7);  // Less shadow
        multiScatter += contribution * scatterPhase * scatterTransmittance * lightColor;
        contribution *= attenuation;
    }

    // Silver lining (bright edges when backlit)
    float silverLining = pow(saturate(1.0 - lightTransmittance), data.silverLiningSpread);
    silverLining *= saturate(-cosTheta * 0.5 + 0.5);
    multiScatter += lightColor * data.silverLiningIntensity * silverLining;

    return directLight + multiScatter * lightPower;
}

// Combined lighting: the sun fades out below the horizon while the moon
// (antipodal, dim, cool) fades in, so night clouds are moonlit — not lit by a
// phantom below-horizon sun. Ambient dims at night but keeps its tint.
float3 cloudLighting(float3 worldPos, float3 rayDir, constant VolumetricCloudData& data,
                     texture3d<float, access::sample> shapeTex,
                     texture3d<float, access::sample> detailTex,
                     texture2d<float, access::sample> weatherTex) {
    // sunLightScale < 1: clouds absorb/self-shadow, so the lit surface sits
    // BELOW the clear-sky brightness instead of blooming over it.
    float sunPower = data.sunIntensity * data.sunLightScale;
    float dayFactor = smoothstep(-0.12, 0.08, data.sunDirection.y);  // 1 day, 0 night

    // Sky ambient absorbs downward through the deck: dark bases, bright tops
    // (twin of CloudRaymarch.frag) — without it the in-cloud view and any
    // sun-shadowed face collapsed to one flat constant.
    float hf = saturate((worldPos.y - data.cloudLayerBottom) / data.cloudLayerThickness);
    float3 lum = data.ambientColor * data.ambientIntensity * (0.35 + 0.65 * hf) * mix(0.3, 1.0, dayFactor);

    if (dayFactor > 0.01) {
        float tr = lightMarch(worldPos, data.sunDirection, data, shapeTex, detailTex, weatherTex);
        lum += scatterFromLight(data.sunColor, sunPower, tr,
                                dot(rayDir, data.sunDirection), data) * dayFactor;
    }
    if (dayFactor < 0.99) {
        float3 moonDir = -data.sunDirection;  // antipodal, matches TimeOfDaySystem
        float tr = lightMarch(worldPos, moonDir, data, shapeTex, detailTex, weatherTex);
        lum += scatterFromLight(data.moonColor, sunPower * data.moonLightScale, tr,
                                dot(rayDir, moonDir), data) * (1.0 - dayFactor);
    }
    return lum;
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
                      constant VolumetricCloudData& data, float blueNoise,
                      texture3d<float, access::sample> shapeTex,
                      texture3d<float, access::sample> detailTex,
                      texture2d<float, access::sample> weatherTex) {
    // Find cloud layer intersection
    float2 tRange = cloudLayerIntersection(rayOrigin, rayDir, data);

    if (tRange.y <= tRange.x || tRange.x > maxDist) {
        return float4(0.0, 0.0, 0.0, 1.0);  // No intersection
    }

    // Clamp to max distance
    tRange.y = min(tRange.y, maxDist);

    float rayLength = tRange.y - tRange.x;

    // Accumulation
    float3 scattering = float3(0.0);
    float transmittance = 1.0;

    // View-sun angle for lighting
    float cosTheta = dot(rayDir, data.sunDirection);

    // Bound the step SIZE, not the step count (twin of CloudRaymarch.frag).
    // A fixed primarySteps made the step length depend on view angle: 57 m at
    // the zenith but 330 m near the horizon, where a ground ray enters the
    // 9.5 km deck 55 km out and crosses 30 km of it. 330 m is longer than the
    // whole 200 m detail-noise period, so the detail octave aliased into
    // per-frame noise — grey mush after temporal averaging, vertical shudder
    // when rotation invalidated the history. Fine steps come from the layer
    // thickness instead (view-angle independent); coarse steps (4x) skip the
    // empty air so the total stays in the same budget.
    float baseFine = min(data.cloudLayerThickness / 96.0, max(rayLength / 32.0, 1.0));
    // Inside the layer the ladder is finer (below) and there is far more of it
    // in front of the eye, so give the fly-through a larger iteration budget.
    bool insideLayer = (tRange.x <= 0.001);
    uint maxIters = data.primarySteps * (insideLayer ? 3u : 2u);

    // Undithered ladder + STRATIFIED sample inside each segment (twin of
    // CloudRaymarch.frag). A single entry dither could not span fineStep once
    // it grows 4x past ~20 km, and the coarse back-up's clamp to tIntegrated
    // erased it outright whenever the first probe hit — the in-layer case.
    float t = tRange.x;
    float tIntegrated = tRange.x;  // never integrate behind this (no double count)
    float tFirstHit = 1e9;         // distance to the nearest contributing sample
    bool inCloud = false;
    int emptyRun = 0;

    for (uint i = 0; i < maxIters && t < tRange.y; i++) {
        // Steps grow with distance, lagging the detail LOD fade: once only the
        // 10 km base shape survives, 4x coarser is still ~40 samples/feature.
        // Near-field LOD (twin of CloudRaymarch.frag). baseFine is ~57 m at
        // every view angle, but at the shipped density (0.3/m) optical depth 1
        // is reached in 3.3 m — one 57 m step drives transmittance 1.0 -> 3e-8,
        // so every cloud boundary is a hard 57 m staircase. That, not the RT
        // resolution, caps in-layer sharpness. Ramp with distance (12 m at the
        // eye, baseFine again by ~2.9 km) for a constant angular footprint;
        // beyond 2.9 km the ladder is bit-for-bit unchanged.
        float nearFine = min(max(t * 0.02, 12.0), baseFine);
        float fineStep = nearFine * mix(1.0, 4.0, smoothstep(20000.0, 45000.0, t));
        // Guarantee the ladder REACHES tRange.y inside the budget (twin of
        // CloudRaymarch.frag). Otherwise a near-horizontal in-layer ray runs
        // out of iterations mid-flight and stops dead; the truncation distance
        // is a function of elevation, i.e. of screen row, so the boundary is a
        // hard HORIZONTAL edge that slides as the camera turns.
        float itersLeft = float(maxIters - i);
        fineStep = max(fineStep, (tRange.y - t) / max(itersLeft, 1.0));
        float coarseStep = fineStep * 4.0;

        if (!inCloud) {
            // Empty-space skip on the cheap density. Detail only ever erodes,
            // so the cheap value is an upper bound — this cannot step over a
            // cloud the full-quality path would have found.
            float3 probePos = rayOrigin + rayDir * (t + blueNoise * coarseStep);
            if (sampleCloudDensity(probePos, data, true, shapeTex, detailTex, weatherTex) > 0.0) {
                // Back up one coarse step so the lit leading edge isn't clipped.
                t = max(t - coarseStep, tIntegrated);
                inCloud = true;
                emptyRun = 0;
            } else {
                t += coarseStep;
            }
            continue;
        }

        // Stratified sample inside [t, t + fineStep]; dt stays fineStep.
        float3 pos = rayOrigin + rayDir * (t + blueNoise * fineStep);
        float density = sampleCloudDensity(pos, data, false, shapeTex, detailTex, weatherTex);

        if (density > 0.001) {
            tFirstHit = min(tFirstHit, t);
            // Sun-by-day / moon-by-night key lighting + ambient.
            float3 luminance = cloudLighting(pos, rayDir, data, shapeTex, detailTex, weatherTex);

            // Beer-powder effect
            float powder = beerPowderEnergy(density * fineStep * 10.0, cosTheta) * data.powderStrength +
                          (1.0 - data.powderStrength);

            // Integrate
            float stepTransmittance = beerLambert(density, fineStep);
            float3 stepScattering = luminance * (1.0 - stepTransmittance) * powder;

            scattering += transmittance * stepScattering;
            transmittance *= stepTransmittance;

            // Early exit
            if (transmittance < 0.01) {
                transmittance = 0.0;
                break;
            }
            emptyRun = 0;
        } else if (++emptyRun >= 8) {
            inCloud = false;  // out the far side — back to skipping
            emptyRun = 0;
        }
        t += fineStep;
        tIntegrated = t;
    }

    // Aerial perspective (twin of CloudRaymarch.frag): distant decks sink into
    // the horizon haze — scattering fades toward a sky tint and the cloud
    // loses opacity (~40 km e-folding on the entry distance). Day-gated.
    float apDay = smoothstep(-0.12, 0.08, data.sunDirection.y);
    // Keyed on the nearest CONTRIBUTING sample, not the layer entry: inside
    // the layer the entry is 0, which switched haze and distFade off entirely
    // and left a near-horizontal in-layer ray ending dead at the iteration
    // budget with no fade (twin of CloudRaymarch.frag).
    float fadeDist = min(max(tFirstHit, tRange.x), 1e8);
    float haze = 1.0 - exp(-max(fadeDist, 0.0) * 2.5e-5);
    float3 hazeTint = (data.sunColor * 0.25 + data.ambientColor * 0.75) *
                      (data.sunIntensity * data.sunLightScale * 0.25) * mix(0.05, 1.0, apDay);
    scattering = mix(scattering, hazeTint * (1.0 - transmittance), haze);
    transmittance = mix(transmittance, 1.0, haze * 0.5);

    // The march stops at maxDist (100 km for sky pixels), reached at ~5 deg
    // elevation on a flat deck — without an explicit fade the clouds ended
    // there, leaving a hard line across the sky.
    float distFade = smoothstep(60000.0, 95000.0, fadeDist);
    scattering *= 1.0 - distFade;
    transmittance = mix(transmittance, 1.0, distFade);

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
    texture3d<float, access::sample> shapeNoiseTex [[texture(2)]],
    texture3d<float, access::sample> detailNoiseTex [[texture(3)]],
    texture2d<float, access::sample> weatherMapTex [[texture(4)]],
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
    float4 cloudData = raymarchClouds(data.cameraPosition, rayDir, maxDist, data, blueNoise,
                                  shapeNoiseTex, detailNoiseTex, weatherMapTex);

    // Composite
    float3 result = color.rgb * cloudData.a + cloudData.rgb;

    return float4(result, 1.0);
}

// ============================================================================
// Low Resolution Pass (reduced resolution + temporal upsample)
// ============================================================================

fragment float4 cloudFragmentLowRes(
    CloudVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneDepth [[texture(0)]],
    texture3d<float, access::sample> shapeNoiseTex [[texture(1)]],
    texture3d<float, access::sample> detailNoiseTex [[texture(2)]],
    texture2d<float, access::sample> weatherMapTex [[texture(3)]],
    constant VolumetricCloudData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    // No sub-texel jitter, deliberately (twin of CloudRaymarch.frag): rotation
    // already sweeps the grid through every sub-texel phase, so measured
    // flicker was identical with and without it.
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
    float4 cloudData = raymarchClouds(data.cameraPosition, rayDir, maxDist, data, blueNoise,
                                  shapeNoiseTex, detailNoiseTex, weatherMapTex);

    return cloudData;
}

// ============================================================================
// Temporal Reprojection Pass
// ============================================================================

// Catmull-Rom history sampling (9-tap, bilinear-fetch optimized) — twin of
// CloudTemporal.frag. Plain bilinear re-resampled the history every frame; at
// temporalBlend 0.05 the accumulator survives ~20 resamples, and 20 stacked
// bilinear tents are a huge low-pass — mush whenever the camera rotated.
static float4 sampleHistoryCatmullRom(texture2d<float, access::sample> tex,
                                      float2 uv, float2 screenSize) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float2 samplePos = uv * screenSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 texPos0 = (texPos1 - 1.0) / screenSize;
    float2 texPos3 = (texPos1 + 2.0) / screenSize;
    float2 texPos12 = (texPos1 + offset12) / screenSize;
    float4 result =
        tex.sample(s, float2(texPos0.x,  texPos0.y))  * w0.x  * w0.y +
        tex.sample(s, float2(texPos12.x, texPos0.y))  * w12.x * w0.y +
        tex.sample(s, float2(texPos3.x,  texPos0.y))  * w3.x  * w0.y +
        tex.sample(s, float2(texPos0.x,  texPos12.y)) * w0.x  * w12.y +
        tex.sample(s, float2(texPos12.x, texPos12.y)) * w12.x * w12.y +
        tex.sample(s, float2(texPos3.x,  texPos12.y)) * w3.x  * w12.y +
        tex.sample(s, float2(texPos0.x,  texPos3.y))  * w0.x  * w3.y +
        tex.sample(s, float2(texPos12.x, texPos3.y))  * w12.x * w3.y +
        tex.sample(s, float2(texPos3.x,  texPos3.y))  * w3.x  * w3.y;
    return max(result, float4(0.0));
}

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
    // Last line of defence against a non-finite raymarch sample: it would be
    // latched into the history for the rest of the session and smeared a texel
    // per frame by the reprojection (twin of CloudTemporal.frag).
    if (any(isnan(current)) || any(isinf(current))) current = float4(0.0, 0.0, 0.0, 1.0);

    float depth = sceneDepth.sample(linearSampler, in.uv).r;
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 farPos = data.invViewProj * float4(ndc, 1.0, 1.0);
    float3 rayDir = normalize(farPos.xyz / farPos.w - data.cameraPosition);

    // Reprojection point (twin of CloudTemporal.frag). Sky pixels used the
    // far plane: exact under pure rotation but parallax-free, so translation
    // reprojected nearby billows from the wrong place and smeared them. Use
    // the mid-cloud distance along the view ray instead; geometry pixels
    // keep the depth-buffer position. parallaxTexels = divergence between
    // reprojecting the near and far end of the in-layer segment: exactly
    // zero under pure rotation, grows only with translation.
    float4 worldPos;
    float parallaxTexels = 0.0;
    if (depth >= 0.9999) {
        float tBottom = (data.cloudLayerBottom - data.cameraPosition.y) / rayDir.y;
        float tTop = (data.cloudLayerTop - data.cameraPosition.y) / rayDir.y;
        float tMin = min(tBottom, tTop);
        float tMax = max(tBottom, tTop);
        if (data.cameraPosition.y >= data.cloudLayerBottom &&
            data.cameraPosition.y <= data.cloudLayerTop) tMin = 0.0;
        float tNear = clamp(max(tMin, 0.0), 200.0, 60000.0);
        float tRep = (tMax <= 0.0)
            ? 30000.0  // layer fully behind the ray: distance is moot, any works
            : tNear + 0.5 * min(max(tMax - tNear, 0.0), 8000.0);
        worldPos = float4(data.cameraPosition + rayDir * clamp(tRep, 200.0, 60000.0), 1.0);
        float4 pcN = data.prevViewProj * float4(data.cameraPosition + rayDir * tNear, 1.0);
        float4 pcF = data.prevViewProj * float4(data.cameraPosition + rayDir * (tNear + 8000.0), 1.0);
        float2 uvN = pcN.xy / pcN.w, uvF = pcF.xy / pcF.w;
        parallaxTexels = length((uvN - uvF) * 0.5 * data.screenSize);
    } else {
        // Geometry pixel: carries no cloud, and the horizon sweeps across it
        // as the camera turns, so its history is stale sky. Maximally
        // untrustworthy — leaving it 0 kept a low blend on every ground pixel
        // (twin of CloudTemporal.frag).
        parallaxTexels = 1e3;
        worldPos = data.invViewProj * float4(ndc, depth, 1.0);
        worldPos /= worldPos.w;
    }

    // Reproject to previous frame
    float4 prevClip = data.prevViewProj * worldPos;
    float2 prevUV = prevClip.xy / prevClip.w * 0.5 + 0.5;
    prevUV.y = 1.0 - prevUV.y;

    // Validity check (prevClip.w > 0 rejects behind-camera reprojections —
    // parity with CloudTemporal.frag).
    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                        prevUV.y >= 0.0 && prevUV.y <= 1.0 && prevClip.w > 0.0;

    float4 history = sampleHistoryCatmullRom(historyCloud, prevUV, data.screenSize);
    if (any(isnan(history)) || any(isinf(history))) { history = current; validHistory = false; }

    // Anti-ghosting: VARIANCE clip, not a hard min/max box (twin of
    // CloudTemporal.frag). The min/max box is inert over smooth cloud but
    // binds hard at an edge, where it collapses onto the two grid-quantised
    // levels and snaps the accumulator back onto this frame's level every
    // frame — defeating the accumulation entirely, independent of
    // temporalBlend. mean +- k*sigma is wide enough to average the jittered
    // samples into sub-texel resolution while still rejecting real outliers.
    float4 m1 = float4(0.0);
    float4 m2 = float4(0.0);
    float2 texelSize = 1.0 / data.screenSize;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float4 neighbor = currentCloud.sample(linearSampler, in.uv + float2(x, y) * texelSize);
            m1 += neighbor;
            m2 += neighbor * neighbor;
        }
    }
    float4 mean = m1 / 9.0;
    float4 sigma = sqrt(max(m2 / 9.0 - mean * mean, float4(0.0)));
    // WIDEN the box, never disable it (twin of CloudTemporal.frag). The floor
    // is RELATIVE to the mean, which is what does the anti-ghosting: where the
    // neighborhood is empty, mean and sigma are both 0 so the box collapses to
    // [0,0] and stale history is forced to zero however wide k is. Where there
    // is signal the box is mean +- 0.8*mean, loose enough for the accumulator
    // to average instead of being snapped onto this frame's quantised value.
    // A hard min/max box overwrote the accumulator every frame (the field is
    // near-binary: optical depth 1 in 3.3 m); gating the clamp off under exact
    // reprojection fixed that but smeared history across disocclusions.
    // Clip in a HUE-PRESERVING basis (twin of CloudTemporal.frag): clamping R,
    // G and B independently on premultiplied inscatter clips the channels by
    // different amounts and shifts hue — the coloured fringing on cloud edges.
    const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
    float hLum = dot(history.rgb, LUMA);
    float mLum = dot(mean.rgb, LUMA);
    float sLum = max(dot(sigma.rgb, LUMA), abs(mLum) * 0.05);
    float cLum = clamp(hLum, mLum - 16.0 * sLum, mLum + 16.0 * sLum);
    history.rgb *= (abs(hLum) > 1e-6) ? (cLum / hLum) : 0.0;
    float sA = max(sigma.a, abs(mean.a) * 0.05);
    history.a = clamp(history.a, mean.a - 16.0 * sA, mean.a + 16.0 * sA);

    // Parallax-adaptive blend (twin of CloudTemporal.frag): base rate at rest
    // AND under pure rotation — history is exact there, and dumping it on
    // rotation was the shudder (per-frame raymarch grain pulsing through
    // bloom). Extra current only where translation makes the reprojection
    // depth-ambiguous.
    float blend = validHistory
        ? clamp(data.temporalBlend + parallaxTexels * 0.05, data.temporalBlend, 0.5)
        : 1.0;
    return mix(history, current, blend);
}

// ============================================================================
// Cloud Shadow Map Pass
// ============================================================================
// Sun-light transmittance through the deck over a camera-centered world region
// (twin of CloudShadow.frag — constants must match its CSM_HALF/CSM_SNAP and
// the sampling code in the PBR fragments). Cheap density only.

constant float kCloudShadowHalf = 2048.0;
constant float kCloudShadowSnap = 16.0;

fragment float4 cloudShadowMap(
    CloudVertexOut in [[stage_in]],
    texture3d<float, access::sample> shapeTex [[texture(0)]],
    texture2d<float, access::sample> weatherMapTex [[texture(1)]],
    constant VolumetricCloudData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]]
) {
    float dayFactor = smoothstep(-0.12, 0.08, data.sunDirection.y);
    if (dayFactor < 0.01 || data.sunDirection.y < 0.05) {
        return float4(1.0);
    }

    float2 center = floor(camera.position.xz / kCloudShadowSnap) * kCloudShadowSnap;
    float3 world = float3(center.x + (in.uv.x * 2.0 - 1.0) * kCloudShadowHalf,
                          0.0,
                          center.y + (in.uv.y * 2.0 - 1.0) * kCloudShadowHalf);

    float t0 = (data.cloudLayerBottom - world.y) / data.sunDirection.y;
    float t1 = (data.cloudLayerTop    - world.y) / data.sunDirection.y;
    const int STEPS = 6;
    float stepLen = (t1 - t0) / float(STEPS);
    float tau = 0.0;
    for (int i = 0; i < STEPS; i++) {
        float3 pos = world + data.sunDirection * (t0 + (float(i) + 0.5) * stepLen);
        tau += sampleCloudDensity(pos, data, true, shapeTex, shapeTex, weatherMapTex) * stepLen;
    }
    return float4(mix(1.0, exp(-tau), dayFactor));
}

// ============================================================================
// Upscale and Composite Pass
// ============================================================================

fragment float4 cloudUpscaleComposite(
    CloudVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> cloudTexture [[texture(1)]],
    texture2d<float, access::sample> sceneDepth [[texture(2)]],
    constant VolumetricCloudData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float4 scene = sceneColor.sample(linearSampler, in.uv);

    // Depth-aware (bilateral) upsample — twin of CloudComposite.frag: the four
    // nearest coarse texels weighted by bilinear weight x depth similarity, so
    // cloud values don't bleed across geometry edges (eave/sky halo).
    const float near = camera.near;
    const float far  = camera.far;
    float dCenterRaw = sceneDepth.sample(linearSampler, in.uv).r;
    float dCenter = near * far / (far - dCenterRaw * (far - near));

    float2 cloudSize = float2(cloudTexture.get_width(), cloudTexture.get_height());
    float2 coord = in.uv * cloudSize - 0.5;
    float2 base = floor(coord);
    float2 f = coord - base;

    float4 cloudSum = float4(0.0);
    float wSum = 0.0;
    for (int i = 0; i < 4; ++i) {
        float2 off = float2(float(i & 1), float(i >> 1));
        float2 uvTap = (base + off + 0.5) / cloudSize;  // texel center = point value
        float wBilin = mix(1.0 - f.x, f.x, off.x) * mix(1.0 - f.y, f.y, off.y);
        float dTapRaw = sceneDepth.sample(linearSampler, uvTap).r;
        float dTap = near * far / (far - dTapRaw * (far - near));
        // ~10%-of-depth tolerance: taps behind a different surface get ~zero weight.
        float wDepth = exp(-abs(dTap - dCenter) / (0.1 * dCenter + 0.5));
        float w = wBilin * wDepth;
        cloudSum += cloudTexture.sample(linearSampler, uvTap) * w;
        wSum += w;
    }
    float4 cloud = wSum > 1e-4 ? cloudSum / wSum
                               : cloudTexture.sample(linearSampler, in.uv);

    // Composite: scene * transmittance + scattering
    float3 result = scene.rgb * cloud.a + cloud.rgb;

    return float4(result, scene.a);
}
