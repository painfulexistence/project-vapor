#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/3d_volumetric_common.metal"

// ============================================================================
// Volumetric Fog - Froxel-based Implementation
// ============================================================================
// A 3D froxel (frustum voxel) grid aligned to the camera frustum with an
// exponential depth distribution. Three stages:
//   1. froxelInjection      (compute) — per froxel, blend every fog volume's
//      density/scattering, light it (sun + PSSM shadow, tile-culled points,
//      spots, rects), write (in-scatter.rgb, extinction) to a 3D texture.
//   2. scatteringIntegration (compute) — march each froxel column front-to-back
//      accumulating scattering + transmittance into a second 3D texture.
//   3. volumetricFogFragment (fragment) — sample the integrated volume at the
//      pixel's depth slice and composite: scene * transmittance + in-scatter.
// This decouples fog cost from screen resolution (the per-pixel work is a single
// 3D texture tap) and is the froxel upgrade over the simpleFogFragment raymarch
// kept below as a fallback.

// Froxel grid dimensions.
constant uint FROXEL_SIZE_X = 160;
constant uint FROXEL_SIZE_Y = 90;
constant uint FROXEL_SIZE_Z = 64;

// Max fog volumes injected per frame — matches kMaxFogVolumes (render_data.hpp)
// and the per-volume buffer the renderer uploads.
constant uint MAX_FOG_VOLUMES = 16;

// ============================================================================
// Data Structures
// ============================================================================

// Froxel-fog globals. Byte-for-byte twin of Vapor::VolumetricFogData
// (graphics_effects.hpp): MSL float3 occupies a full 16-byte slot, so each vec3
// there matches a vec3 + trailing pad here. The explicit float2 pads
// (_pad5/_pad6) are REQUIRED to keep frameIndex/windDirection aligned with the
// C++ struct — the froxel kernels read the whole tail, unlike simpleFogFragment.
struct VolumetricFogData {
    float4x4 invViewProj;
    float4x4 prevViewProj;
    float3 cameraPosition;
    float3 sunDirection;
    float3 sunColor;
    float sunIntensity;
    float fogDensity;
    float fogHeightFalloff;
    float fogBaseHeight;
    float fogMaxHeight;
    float scatteringCoeff;
    float extinctionCoeff;
    float anisotropy;
    float ambientIntensity;
    float nearPlane;
    float farPlane;
    uint  volumeCount;      // fog volumes in the volume buffer (was _pad4)
    float2 screenSize;
    float2 _pad5;
    uint  frameIndex;
    float temporalBlend;
    float noiseScale;
    float noiseIntensity;
    float windSpeed;
    float time;
    float2 _pad6;
    float3 windDirection;
};

// One fog volume. vec4-only twin of Vapor::VolumetricFogVolumeGPU (96 bytes).
struct VolumetricFogVolume {
    float4 boundsMin;      // xyz world AABB min; w = bounded flag (0 = global, 1 = AABB)
    float4 boundsMax;      // xyz world AABB max; w = edgeFalloff (world units)
    float4 densityParams;  // x = density, y = heightFalloff, z = baseHeight, w = maxHeight
    float4 albedoBlend;    // xyz = albedo/tint; w = blendWeight
    float4 phaseNoise;     // x = anisotropy, y = ambientIntensity, z = noiseScale, w = noiseIntensity
    float4 wind;           // x = windSpeed (already × global strength)
};

// ============================================================================
// Helper Functions
// ============================================================================

// Reconstruct the world-space center of a froxel from its normalized grid coord.
// Depth is distributed exponentially (more slices near the camera). The froxel
// sits at a PLANAR view-space depth (-Z), matching depthToSlice() in the
// composite. Uses the camera's own invProj/invView so the reconstruction is
// exact for the render projection.
float3 froxelToWorld(float3 uvw, constant VolumetricFogData& data, constant CameraData& camera) {
    float viewDepth = data.nearPlane * pow(data.farPlane / data.nearPlane, uvw.z);
    // Normalized froxel xy -> Metal (Y-down) screen UV -> NDC.
    float2 ndc = float2(uvw.x * 2.0 - 1.0, 1.0 - uvw.y * 2.0);
    float4 viewH = camera.invProj * float4(ndc, 1.0, 1.0);
    float3 viewRay = viewH.xyz / viewH.w;                       // point on far plane, view space
    float3 viewPos = viewRay * (viewDepth / max(-viewRay.z, 1e-4));  // rescale to planar depth
    return (camera.invView * float4(viewPos, 1.0)).xyz;
}

// View-space planar depth -> fractional froxel Z slice.
float depthToSlice(float viewDepth, float nearPlane, float farPlane) {
    return log(max(viewDepth, nearPlane) / nearPlane) / log(farPlane / nearPlane) * float(FROXEL_SIZE_Z);
}

// Density of a single fog volume at a world position (0 outside its extent).
// Global (bounded==0): exponential height fog. Bounded: uniform density inside
// the AABB, cross-faded to 0 over edgeFalloff at each face. Both modulated by
// the shared wind-animated value-noise (windSpeed already folds in wind strength).
float evalVolumeDensity(const device VolumetricFogVolume& v, float3 p,
                        constant VolumetricFogData& data) {
    float density  = v.densityParams.x;
    float bounded  = v.boundsMin.w;

    float base;
    if (bounded < 0.5) {
        // Global exponential height fog.
        float baseHeight    = v.densityParams.z;
        float maxHeight     = v.densityParams.w;
        float heightFalloff = v.densityParams.y;
        if (p.y > maxHeight) return 0.0;
        base = density * exp(-max(0.0, p.y - baseHeight) * max(heightFalloff, 1e-4));
    } else {
        // AABB fog bank with soft edges.
        float3 bmin = v.boundsMin.xyz;
        float3 bmax = v.boundsMax.xyz;
        if (any(p < bmin) || any(p > bmax)) return 0.0;
        float edgeFalloff = v.boundsMax.w;
        float3 dmin = p - bmin;
        float3 dmax = bmax - p;
        float edge = min(min(min(dmin.x, dmin.y), dmin.z),
                         min(min(dmax.x, dmax.y), dmax.z));
        base = density * saturate(edge / max(edgeFalloff, 1e-4));
    }
    if (base <= 0.0) return 0.0;

    float noiseScale = v.phaseNoise.z;
    if (noiseScale > 0.0) {
        float noiseIntensity = v.phaseNoise.w;
        float3 np = p * noiseScale + data.windDirection * (data.time * v.wind.x);
        float n = fbm3D(np, 4, 2.0, 0.5) * 0.5 + 0.5;   // [0,1]
        base *= (1.0 + (n - 0.5) * noiseIntensity * 2.0);
    }
    return max(0.0, base);
}

// ============================================================================
// Compute Shader: Froxel Injection
// ============================================================================
// Blend every fog volume at each froxel, light the medium, store scattering +
// extinction. Light handling mirrors simpleFogFragment (shared conventions).

kernel void froxelInjection(
    uint3 gid [[thread_position_in_grid]],
    constant VolumetricFogData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    constant PSSMData& pssmData [[buffer(2)]],
    const device PointLight* pointLights [[buffer(3)]],
    const device Cluster* clusters [[buffer(4)]],
    const device SpotLight* spotLights [[buffer(5)]],
    const device RectLight* rectLights [[buffer(6)]],
    constant uint4& fogLightParams [[buffer(7)]],  // x=cullGridX y=cullGridY z=spotCount w=rectCount
    const device VolumetricFogVolume* volumes [[buffer(8)]],
    texture3d<float, access::write> froxelGrid [[texture(0)]],
    depth2d_array<float, access::sample> pssmShadowMaps [[texture(1)]]
) {
    if (gid.x >= FROXEL_SIZE_X || gid.y >= FROXEL_SIZE_Y || gid.z >= FROXEL_SIZE_Z) {
        return;
    }
    constexpr sampler shadowCmpSampler(filter::linear, compare_func::less_equal,
                                       address::clamp_to_edge);

    float3 uvw = (float3(gid) + 0.5) / float3(FROXEL_SIZE_X, FROXEL_SIZE_Y, FROXEL_SIZE_Z);
    float3 worldPos = froxelToWorld(uvw, data, camera);

    // Volume blend: densities add (extinction is additive across overlapping
    // media); albedo/phase-g/ambient are density×blendWeight-weighted averages,
    // so a dense bank dominates a thin haze where they overlap.
    float  totalDensity = 0.0;
    float3 wAlbedo  = float3(0.0);
    float  wG       = 0.0;
    float  wAmbient = 0.0;
    float  wSum     = 0.0;
    uint count = min(data.volumeCount, MAX_FOG_VOLUMES);
    for (uint vi = 0; vi < count; ++vi) {
        float d = evalVolumeDensity(volumes[vi], worldPos, data);
        if (d <= 0.0) continue;
        float w = d * max(volumes[vi].albedoBlend.w, 0.0);
        totalDensity += d;
        wAlbedo  += volumes[vi].albedoBlend.xyz * w;
        wG       += volumes[vi].phaseNoise.x * w;
        wAmbient += volumes[vi].phaseNoise.y * w;
        wSum     += w;
    }
    if (totalDensity < 1e-5 || wSum <= 0.0) {
        froxelGrid.write(float4(0.0), gid);
        return;
    }
    float3 albedo  = wAlbedo / wSum;
    float  g       = wG / wSum;
    float  ambient = wAmbient / wSum;

    float3 rayDir = normalize(worldPos - data.cameraPosition);

    // Per-froxel tile light list (2D screen tiles, y-up cull convention — the
    // same mapping simpleFogFragment uses).
    uint tileX = min(uint(uvw.x * float(fogLightParams.x)), fogLightParams.x - 1);
    uint tileY = min(uint((1.0 - uvw.y) * float(fogLightParams.y)), fogLightParams.y - 1);
    const device Cluster& tile = clusters[tileX + tileY * fogLightParams.x];
    uint tileLights = min(tile.lightCount, 16u);

    float3 L = float3(0.0);

    // Sun with a volumetric PSSM cascade tap (light shafts). Positions outside
    // every cascade count as lit.
    {
        float sunPhase = phaseHenyeyGreenstein(dot(rayDir, data.sunDirection), g);
        float viewDepth = abs((camera.view * float4(worldPos, 1.0)).z);
        int ci = 0;
        if      (viewDepth > pssmData.cascadeSplits.z) ci = 2;
        else if (viewDepth > pssmData.cascadeSplits.y) ci = 1;
        float4 lsPos = pssmData.lightSpaceMatrices[ci] * float4(worldPos, 1.0);
        float3 proj = lsPos.xyz / lsPos.w;
        float2 sUV = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
        float sunVis = 1.0;
        if (all(sUV >= 0.0) && all(sUV <= 1.0) && proj.z <= 1.0) {
            // Explicit LOD: a compute kernel has no screen-space derivatives, so
            // the implicit-LOD sample_compare (used in the fragment path) is
            // illegal here.
            sunVis = pssmShadowMaps.sample_compare(shadowCmpSampler, sUV, ci,
                                                   proj.z - 0.002, level(0));
        }
        L += data.sunColor * data.sunIntensity * sunPhase * sunVis;
    }

    // Tile-culled point lights (surface-matching falloff).
    for (uint i = 0; i < tileLights; i++) {
        PointLight pl = pointLights[tile.lightIndices[i]];
        float3 toL = pl.position - worldPos;
        float d2 = max(dot(toL, toL), 1e-4);
        float d = sqrt(d2);
        if (d >= pl.radius) continue;
        float atten = (1.0 - smoothstep(pl.radius * 0.8, pl.radius, d)) / d2;
        float phase = phaseHenyeyGreenstein(dot(rayDir, toL / d), g);
        L += pl.color * pl.intensity * atten * phase;
    }

    // Spot cones (loop-all; scenes carry a handful).
    for (uint i = 0; i < fogLightParams.z; i++) {
        SpotLight sl = spotLights[i];
        float3 toL = sl.position - worldPos;
        float d2 = max(dot(toL, toL), 1e-4);
        float d = sqrt(d2);
        if (d >= sl.radius) continue;
        float3 ldir = toL / d;
        float cone = clamp((dot(-ldir, sl.direction) - sl.cosOuter)
                               / max(sl.cosInner - sl.cosOuter, 1e-4), 0.0, 1.0);
        if (cone <= 0.0) continue;
        float atten = (1.0 - smoothstep(sl.radius * 0.8, sl.radius, d)) / d2 * cone * cone;
        float phase = phaseHenyeyGreenstein(dot(rayDir, ldir), g);
        L += sl.color * sl.intensity * atten * phase;
    }

    // Rect area lights: attenuate from the closest point on the quad (see the
    // simpleFogFragment note); two-sided coarse approximation.
    for (uint i = 0; i < fogLightParams.w; i++) {
        RectLight rl = rectLights[i];
        float3 lp = float3(rl.position);
        float3 lr = float3(rl.right);
        float3 lu = float3(rl.up);
        float3 rel = worldPos - lp;
        float pu = clamp(dot(rel, lr), -rl.halfWidth,  rl.halfWidth);
        float pv = clamp(dot(rel, lu), -rl.halfHeight, rl.halfHeight);
        float3 closest = lp + lr * pu + lu * pv;
        float area = 4.0 * rl.halfWidth * rl.halfHeight;
        float range = 8.0 * max(rl.halfWidth, rl.halfHeight);
        float3 toL = closest - worldPos;
        float d2 = max(dot(toL, toL), 1e-4);
        float d = sqrt(d2);
        if (d >= range) continue;
        float atten = (1.0 - smoothstep(range * 0.8, range, d)) / d2 * area;
        float phase = phaseHenyeyGreenstein(dot(rayDir, toL / d), g);
        L += float3(rl.color) * rl.intensity * atten * phase;
    }

    // Ambient floor so fog reads even away from direct light.
    L += float3(0.5, 0.6, 0.7) * ambient;

    // In-scattered radiance = albedo · σ_s · L (σ_s ≈ density); extinction σ_t = density.
    float3 inScatter = albedo * totalDensity * L;
    froxelGrid.write(float4(inScatter, totalDensity), gid);
}

// ============================================================================
// Compute Shader: Scattering Integration
// ============================================================================
// One thread per froxel column: march near->far accumulating scattering and
// transmittance (analytic per-slice integration), writing the running result
// into every slice of the integrated volume.

kernel void scatteringIntegration(
    uint2 gid [[thread_position_in_grid]],
    constant VolumetricFogData& data [[buffer(0)]],
    texture3d<float, access::read> froxelGrid [[texture(0)]],
    texture3d<float, access::write> integratedVolume [[texture(1)]]
) {
    if (gid.x >= FROXEL_SIZE_X || gid.y >= FROXEL_SIZE_Y) {
        return;
    }

    float3 accumScatter = float3(0.0);
    float  accumTrans = 1.0;
    for (uint z = 0; z < FROXEL_SIZE_Z; z++) {
        float4 se = froxelGrid.read(uint3(gid, z));
        float3 inScatter = se.rgb;
        float extinction = se.a;

        float sliceNear = data.nearPlane * pow(data.farPlane / data.nearPlane, float(z) / float(FROXEL_SIZE_Z));
        float sliceFar  = data.nearPlane * pow(data.farPlane / data.nearPlane, float(z + 1) / float(FROXEL_SIZE_Z));
        float stepSize = sliceFar - sliceNear;

        float sliceTrans = exp(-extinction * stepSize);
        // Analytic integral of in-scatter under self-extinction over the slice.
        float3 sliceScatter = inScatter * (1.0 - sliceTrans) / max(extinction, 1e-5);

        accumScatter += accumTrans * sliceScatter;
        accumTrans   *= sliceTrans;

        integratedVolume.write(float4(accumScatter, accumTrans), uint3(gid, z));
    }
}

// ============================================================================
// Fragment Shader: Apply Fog to Scene
// ============================================================================

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Full screen triangle vertices.
constant float2 fsTriVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

vertex VertexOut volumetricFogVertex(uint vertexID [[vertex_id]]) {
    VertexOut out;
    out.position = float4(fsTriVerts[vertexID], 0.0, 1.0);
    out.uv = fsTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;
    return out;
}

fragment float4 volumetricFogFragment(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    texture3d<float, access::sample> integratedVolume [[texture(2)]],
    constant VolumetricFogData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float4 color = sceneColor.sample(linearSampler, in.uv);
    float depth = sceneDepth.sample(linearSampler, in.uv).r;

    // Sky ends beyond the froxel grid (which stops at the fog far plane) — no
    // aerial perspective, matching the raymarch fallback.
    if (depth >= 0.9999) {
        return color;
    }

    // Reconstruct world position, take its planar view depth, map to the slice.
    float2 ndc = float2(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    float4 wp = data.invViewProj * float4(ndc, depth, 1.0);
    float3 worldPos = wp.xyz / wp.w;
    float viewDepth = abs((camera.view * float4(worldPos, 1.0)).z);

    float3 volumeUV;
    volumeUV.xy = in.uv;
    volumeUV.z = saturate(depthToSlice(viewDepth, data.nearPlane, data.farPlane) / float(FROXEL_SIZE_Z));

    float4 fog = integratedVolume.sample(linearSampler, volumeUV);
    return float4(color.rgb * fog.a + fog.rgb, color.a);
}

// ============================================================================
// Simplified Single-Pass Fog (fallback / lower-end path)
// ============================================================================

// True volumetric raymarch (replaces the old single-sample analytic height
// fog): 32 steps along the view ray, per-step in-scattering from the FULL
// light set — sun with a PSSM cascade tap per step (real volumetric light
// shafts), the pixel's tile-culled point-light list, spot cones, and rect
// area lights (center-point x area approximation) — with Henyey-Greenstein
// phase and Beer-Lambert transmittance. Density keeps the exponential height
// profile, so the same panel knobs steer it (the brightness response differs
// from the old analytic curve; expect a light retune).
fragment float4 simpleFogFragment(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    depth2d_array<float, access::sample> pssmShadowMaps [[texture(2)]],
    constant VolumetricFogData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    constant PSSMData& pssmData [[buffer(2)]],
    const device PointLight* pointLights [[buffer(3)]],
    const device Cluster* clusters [[buffer(4)]],
    const device SpotLight* spotLights [[buffer(5)]],
    const device RectLight* rectLights [[buffer(6)]],
    constant uint4& fogLightParams [[buffer(7)]]  // x=gridX y=gridY z=spotCount w=rectCount
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);
    constexpr sampler shadowCmpSampler(filter::linear, compare_func::less_equal,
                                       address::clamp_to_edge);

    float4 color = sceneColor.sample(linearSampler, in.uv);
    float depth = sceneDepth.sample(linearSampler, in.uv).r;

    // Skip sky pixels (matches the old behavior; fogging the sky needs a
    // far-plane march and its own tuning — a follow-up).
    if (depth >= 0.9999) {
        return color;
    }

    // Reconstruct world position using invViewProj
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Metal Y-flip
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = data.invViewProj * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    float3 rayDir = normalize(worldPos - data.cameraPosition);
    float marchDist = length(worldPos - data.cameraPosition);

    // Per-pixel tile light list (2D grid, y-up — the PBR reader convention).
    uint tileX = min(uint(in.uv.x * float(fogLightParams.x)), fogLightParams.x - 1);
    uint tileY = min(uint((1.0 - in.uv.y) * float(fogLightParams.y)), fogLightParams.y - 1);
    const device Cluster& tile = clusters[tileX + tileY * fogLightParams.x];
    uint tileLights = min(tile.lightCount, 16u);  // fog cost cap

    const int STEPS = 32;
    float stepLen = marchDist / float(STEPS);
    float sunPhase = phaseHenyeyGreenstein(dot(rayDir, data.sunDirection), data.anisotropy);

    float3 scatter = float3(0.0);
    float trans = 1.0;
    for (int st = 0; st < STEPS; st++) {
        float t = (float(st) + 0.5) * stepLen;
        float3 p = data.cameraPosition + rayDir * t;
        float dens = data.fogDensity
                   * exp(-max(0.0, p.y) * max(data.fogHeightFalloff, 0.0001));
        if (dens < 1e-6) continue;

        float3 L = float3(0.0);

        // Sun with volumetric PSSM shadow (1 tap per step -> light shafts).
        // Cascade selection mirrors the PBR shader; positions outside every
        // cascade count as lit.
        {
            float viewDepth = abs((camera.view * float4(p, 1.0)).z);
            int ci = 0;
            if      (viewDepth > pssmData.cascadeSplits.z) ci = 2;
            else if (viewDepth > pssmData.cascadeSplits.y) ci = 1;
            float4 lsPos = pssmData.lightSpaceMatrices[ci] * float4(p, 1.0);
            float3 proj = lsPos.xyz / lsPos.w;
            float2 sUV = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
            float sunVis = 1.0;
            if (all(sUV >= 0.0) && all(sUV <= 1.0) && proj.z <= 1.0) {
                sunVis = pssmShadowMaps.sample_compare(shadowCmpSampler, sUV, ci,
                                                       proj.z - 0.002);
            }
            L += data.sunColor * data.sunIntensity * sunPhase * sunVis;
        }

        // Tile-culled point lights (same falloff as the surface shading).
        for (uint i = 0; i < tileLights; i++) {
            PointLight pl = pointLights[tile.lightIndices[i]];
            float3 toL = pl.position - p;
            float d2 = max(dot(toL, toL), 1e-4);
            float d = sqrt(d2);
            if (d >= pl.radius) continue;
            float atten = (1.0 - smoothstep(pl.radius * 0.8, pl.radius, d)) / d2;
            float phase = phaseHenyeyGreenstein(dot(rayDir, toL / d), data.anisotropy);
            L += pl.color * pl.intensity * atten * phase;
        }

        // Spot cones (loop-all; scenes carry a handful).
        for (uint i = 0; i < fogLightParams.z; i++) {
            SpotLight sl = spotLights[i];
            float3 toL = sl.position - p;
            float d2 = max(dot(toL, toL), 1e-4);
            float d = sqrt(d2);
            if (d >= sl.radius) continue;
            float3 ldir = toL / d;
            float cone = clamp((dot(-ldir, sl.direction) - sl.cosOuter)
                                   / max(sl.cosInner - sl.cosOuter, 1e-4), 0.0, 1.0);
            if (cone <= 0.0) continue;
            float atten = (1.0 - smoothstep(sl.radius * 0.8, sl.radius, d)) / d2
                        * cone * cone;
            float phase = phaseHenyeyGreenstein(dot(rayDir, ldir), data.anisotropy);
            L += sl.color * sl.intensity * atten * phase;
        }

        // Rect area lights: attenuate from the CLOSEST point on the quad, so
        // the scattering hugs the rectangle's extent (a rounded slab near the
        // panel) instead of a spherical center-point glow. Far away the clamp
        // collapses to the center and it degenerates to a point light, as it
        // should. Two-sided (fog is a coarse scattering approximation, so no
        // front-face cull). (packed_float3 fields hoisted before math.)
        for (uint i = 0; i < fogLightParams.w; i++) {
            RectLight rl = rectLights[i];
            float3 lp = float3(rl.position);
            float3 lr = float3(rl.right);
            float3 lu = float3(rl.up);
            float3 rel = p - lp;
            float pu = clamp(dot(rel, lr), -rl.halfWidth,  rl.halfWidth);
            float pv = clamp(dot(rel, lu), -rl.halfHeight, rl.halfHeight);
            float3 closest = lp + lr * pu + lu * pv;
            float area = 4.0 * rl.halfWidth * rl.halfHeight;
            float range = 8.0 * max(rl.halfWidth, rl.halfHeight);
            float3 toL = closest - p;
            float d2 = max(dot(toL, toL), 1e-4);
            float d = sqrt(d2);
            if (d >= range) continue;
            float atten = (1.0 - smoothstep(range * 0.8, range, d)) / d2 * area;
            float phase = phaseHenyeyGreenstein(dot(rayDir, toL / d), data.anisotropy);
            L += float3(rl.color) * rl.intensity * atten * phase;
        }

        // Ambient floor so fog reads even away from direct light.
        L += float3(0.5, 0.6, 0.7) * data.ambientIntensity;

        scatter += trans * L * dens * stepLen;
        trans *= exp(-dens * stepLen);
    }

    float3 result = color.rgb * trans + scatter;
    return float4(result, color.a);
}
