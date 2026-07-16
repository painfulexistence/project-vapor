#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// ============================================================================
// Heterogeneous Volume Raymarch (EmberGen-style density grids)
// ============================================================================
// MSL twin of VolumeRaymarch.frag. One axis-aligned box volume per pass:
// slab-test the view ray against the box, clamp to the scene depth, then march
// the 3D density texture accumulating single-scattered sun light (one PSSM
// cascade tap per step, plus a short secondary march toward the sun for volume
// self-shadowing) and an ambient floor, under Beer-Lambert transmittance and
// Henyey-Greenstein phase.
//
// The density grid is intended to come from an EmberGen export (import lives
// in a separate PR); until then the renderer feeds a procedural test volume.

// Must match Vapor::VolumeRenderData (vec4-only layout, identical in GLSL).
struct VolumeRenderData {
    float4x4 invViewProj;
    float4 cameraPosition;  // xyz
    float4 boxMin;          // xyz; w = densityScale
    float4 boxMax;          // xyz; w = anisotropy
    float4 albedo;          // xyz = scattering albedo; w = ambientIntensity
    float4 sunDirection;    // xyz (normalized, pointing FROM the sun)
    float4 sunColor;        // xyz; w = sunIntensity
    float4 params;          // x = primary steps, y = self-shadow steps
};

struct VolumeVertexOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 volFsTriVerts[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

vertex VolumeVertexOut volumeRaymarchVertex(uint vertexID [[vertex_id]]) {
    VolumeVertexOut out;
    out.position = float4(volFsTriVerts[vertexID], 0.0, 1.0);
    out.uv = volFsTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;
    return out;
}

static inline float volPhaseHG(float cosTheta, float g) {
    const float INV_4PI = 0.07957747;
    float g2 = g * g;
    float d = 1.0 + g2 - 2.0 * g * cosTheta;
    return INV_4PI * (1.0 - g2) / pow(d, 1.5);
}

// Slab-test a ray against the volume box; returns (tEnter, tExit).
static inline float2 volIntersectBox(float3 ro, float3 rd, float3 bmin, float3 bmax) {
    float3 inv = 1.0 / rd;  // IEEE inf on axis-parallel rays works with min/max
    float3 t0 = (bmin - ro) * inv;
    float3 t1 = (bmax - ro) * inv;
    float3 tmin = min(t0, t1);
    float3 tmax = max(t0, t1);
    return float2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

fragment float4 volumeRaymarchFragment(
    VolumeVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    depth2d_array<float, access::sample> pssmShadowMaps [[texture(2)]],
    texture3d<float, access::sample> densityVolume [[texture(3)]],
    constant VolumeRenderData& data [[buffer(0)]],
    constant PSSMData& pssmData [[buffer(1)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);
    constexpr sampler volumeSampler(filter::linear, address::clamp_to_edge);
    constexpr sampler shadowCmpSampler(filter::linear, compare_func::less_equal,
                                       address::clamp_to_edge);

    float4 color = sceneColor.sample(linearSampler, in.uv);
    float depth = sceneDepth.sample(linearSampler, in.uv).r;

    // Reconstruct the view ray. Sky pixels (depth ~1) still intersect the
    // volume: reconstruct at the far plane and march to the box exit.
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Metal Y-flip
    float4 worldPos4 = data.invViewProj * float4(ndc, min(depth, 0.9999), 1.0);
    float3 worldPos = worldPos4.xyz / worldPos4.w;
    float3 cam = data.cameraPosition.xyz;
    float3 rayDir = normalize(worldPos - cam);
    float sceneDist = length(worldPos - cam);

    float3 bmin = data.boxMin.xyz;
    float3 bmax = data.boxMax.xyz;
    float2 hit = volIntersectBox(cam, rayDir, bmin, bmax);
    float tEnter = max(hit.x, 0.0);
    float tExit = min(hit.y, sceneDist);
    if (tExit <= tEnter) {
        return color;
    }

    int steps = int(data.params.x);
    int shadowSteps = int(data.params.y);
    float stepLen = (tExit - tEnter) / float(steps);
    float densityScale = data.boxMin.w;
    float g = data.boxMax.w;
    float3 sunDir = data.sunDirection.xyz;
    float sunPhase = volPhaseHG(dot(rayDir, sunDir), g);
    float3 invBoxSize = 1.0 / (bmax - bmin);
    // Fixed self-shadow march length: half the box diagonal covers the volume.
    float shadowLen = 0.5 * length(bmax - bmin);
    float shadowStepLen = shadowLen / float(max(shadowSteps, 1));

    float3 scatter = float3(0.0);
    float trans = 1.0;
    for (int st = 0; st < steps; st++) {
        float t = tEnter + (float(st) + 0.5) * stepLen;
        float3 p = cam + rayDir * t;
        float dens = densityVolume.sample(volumeSampler, (p - bmin) * invBoxSize).r
                   * densityScale;
        if (dens < 1e-4) continue;

        // Self-shadowing: short march toward the sun through the grid.
        float sunOD = 0.0;
        for (int ss = 0; ss < shadowSteps; ss++) {
            float3 sp = p + sunDir * ((float(ss) + 0.5) * shadowStepLen);
            sunOD += densityVolume.sample(volumeSampler, (sp - bmin) * invBoxSize).r
                   * densityScale;
        }
        float sunTrans = exp(-sunOD * shadowStepLen);

        // One PSSM cascade tap so scene geometry also shadows the volume.
        // Cascade selection approximates view depth with the march distance
        // (the GLSL twin does the same).
        float sunVis = 1.0;
        {
            int ci = 0;
            if      (t > pssmData.cascadeSplits.z) ci = 2;
            else if (t > pssmData.cascadeSplits.y) ci = 1;
            float4 lsPos = pssmData.lightSpaceMatrices[ci] * float4(p, 1.0);
            float3 proj = lsPos.xyz / lsPos.w;
            float2 sUV = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
            if (all(sUV >= 0.0) && all(sUV <= 1.0) && proj.z <= 1.0) {
                sunVis = pssmShadowMaps.sample_compare(shadowCmpSampler, sUV, ci,
                                                       proj.z - 0.002);
            }
        }

        float3 L = data.sunColor.xyz * data.sunColor.w * sunPhase * sunTrans * sunVis;
        L += float3(0.5, 0.6, 0.7) * data.albedo.w;  // ambient floor

        scatter += trans * data.albedo.xyz * L * dens * stepLen;
        trans *= exp(-dens * stepLen);
        if (trans < 0.005) break;
    }

    return float4(color.rgb * trans + scatter, color.a);
}
