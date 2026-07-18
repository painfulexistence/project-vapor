// ============================================================================
// MicroVoxel traced GI — MSL twins of MicroVoxelGI.comp, MicroVoxelAtrous.comp
// and MicroVoxelGIComposite.frag. Keep the binding contracts mirrored.
//
// Includes 3d_microvoxel.metal for the shared traversal/shading helpers (the
// same runtime-include pattern as 3d_volume_raymarch.metal -> 3d_common.metal);
// the extra vertex/fragment definitions it brings along are simply unused by
// this library.
//
// GI kernel: one cosine-weighted diffuse bounce per pixel per frame (+ one
// sun-shadow ray at the bounce hit), temporal accumulation by reprojecting the
// primary hit's WORLD position. Primary visibility comes from the voxel
// G-buffer written by microVoxelFragment — never re-traced. Output stores
// INCIDENT radiance with camera distance in alpha (history validation +
// à-trous depth weight); the composite multiplies by albedo/AO.
//
// Bindings (Metal): GI kernel — buffers 0 volume params, 1 GI params,
// 2 pageTable, 3 brickPool, 4 palette; textures 0 giOut (write), 1 hitT,
// 2 normalMat, 3 history. Atrous kernel — bytes buffer 0 (step/size),
// textures 0 atrousOut (write), 1 giIn, 2 normalMat. Composite fragment —
// buffer 0 GI params; textures 0 sceneColor, 1 hitT, 2 albedoAO,
// 3 giDenoised, 4 giRaw.
// ============================================================================

#include "Res/shaders/3d_microvoxel.metal"

// Must match Vapor::MicroVoxelGIRenderData.
struct MicroVoxelGIData {
    float4x4 invViewProj;
    float4x4 prevViewProj;
    float4 giCameraPosition;   // xyz; w = frameIndex
    float4 prevCameraPosition; // xyz; w = temporal blend (history weight)
    float4 giParams;           // x = giStrength, y = splitX, z = giWidth, w = giHeight
    float4 giSigmas;           // x = depth, y = normal, z = luma, w = debugModeGI
};

static inline float3 mvDecodeNormal(int idx) {
    constant float3 normals[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
                                   float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
    return normals[clamp(idx, 0, 5)];
}

// Integer hash -> two [0,1) randoms per (pixel, frame).
static inline float2 mvRand2(uint2 pix, uint frame) {
    uint h = pix.x * 374761393u + pix.y * 668265263u + frame * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    uint h2 = (h ^ 0x9E3779B9u) * 2654435761u;
    h2 ^= h2 >> 15;
    return float2(float(h & 0xFFFFu), float(h2 & 0xFFFFu)) / 65535.0;
}

kernel void microVoxelGIKernel(
    uint2 gid [[thread_position_in_grid]],
    constant MicroVoxelData& u [[buffer(0)]],
    constant MicroVoxelGIData& gi [[buffer(1)]],
    device const uint* pageTable [[buffer(2)]],
    device const uint* brickPool [[buffer(3)]],
    device const uint* palette [[buffer(4)]],
    texture2d<float, access::write> giOut [[texture(0)]],
    texture2d<float, access::sample> hitTTex [[texture(1)]],
    texture2d<float, access::sample> normalMatTex [[texture(2)]],
    texture2d<float, access::sample> historyTex [[texture(3)]]
) {
    constexpr sampler pointSampler(filter::nearest, address::clamp_to_edge);
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    int2 giSize = int2(gi.giParams.zw);
    if (int(gid.x) >= giSize.x || int(gid.y) >= giSize.y) return;

    float2 uv = (float2(gid) + 0.5) / float2(giSize);
    float hitT = hitTTex.sample(pointSampler, uv).r;
    if (hitT <= 0.0f) {
        // Miss: zero radiance, zero distance — invalid history, rejected by
        // the à-trous depth weight. Every volume's dispatch agrees on this.
        giOut.write(float4(0.0), gid);
        return;
    }
    float4 nm = normalMatTex.sample(pointSampler, uv);
    if (int(nm.b * 255.0f + 0.5f) != int(u.extra0.x + 0.5f)) return;  // another volume's pixel

    // Reconstruct the primary hit from the G-buffer (no re-trace).
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 farPt = gi.invViewProj * float4(ndc, 0.9999f, 1.0f);
    float3 rd = normalize(farPt.xyz / farPt.w - gi.giCameraPosition.xyz);
    float3 hitWorld = gi.giCameraPosition.xyz + rd * hitT;
    float3 n = mvDecodeNormal(int(nm.r * 255.0f + 0.5f));
    float voxelSize = u.volumeOrigin.w;
    float3 hitLocal = hitWorld - u.volumeOrigin.xyz;

    // Cosine-weighted bounce around the face normal.
    float2 xi = mvRand2(gid, uint(gi.giCameraPosition.w));
    float3 t1 = normalize(fabs(n.x) > 0.5f ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    float3 t2 = cross(n, t1);
    float phi = 6.2831853f * xi.x;
    float sq = sqrt(xi.y);
    float3 bounceDir = normalize(t1 * cos(phi) * sq + t2 * sin(phi) * sq + n * sqrt(max(1.0f - xi.y, 0.0f)));

    float3 radiance;
    MvHit bh;
    float3 bounceOrigin = hitLocal + n * voxelSize * 0.51f;
    if (mvRaycast(u, pageTable, brickPool, bounceOrigin, bounceDir, 1e9f, bh)) {
        float3 bAlbedo;
        float bEmission, bRefl;
        mvDecodeMaterial(palette, bh.mat, bAlbedo, bEmission, bRefl);
        // Sun at the bounce hit with a full shadow ray; the 0.3 sky term
        // stands in for further bounces. Emissive voxels act as area lights.
        float bNdl = max(dot(bh.normal, u.sunDirection.xyz), 0.0f);
        float bShadow = 1.0f;
        if (bNdl > 0.0f) {
            MvHit sh;
            float3 so = bounceOrigin + bounceDir * bh.t + bh.normal * voxelSize * 0.51f;
            if (mvRaycast(u, pageTable, brickPool, so, u.sunDirection.xyz, 1e9f, sh)) bShadow = 0.0f;
        }
        radiance = bAlbedo * (u.sunColor.xyz * u.sunColor.w * MV_INV_PI * bNdl * bShadow
                              + mvSkyRadiance(u, bh.normal) * 0.3f)
                 + bAlbedo * bEmission * u.gridDim.w;
    } else {
        radiance = mvSkyRadiance(u, bounceDir);  // sky-light occlusion falls out naturally
    }

    // Temporal accumulation: reproject the WORLD hit into the previous frame,
    // validate by stored camera distance (alpha), then blend.
    float camDist = length(hitWorld - gi.giCameraPosition.xyz);
    float3 result = radiance;
    float4 prevClip = gi.prevViewProj * float4(hitWorld, 1.0f);
    if (prevClip.w > 0.0f) {
        float2 prevNdc = prevClip.xy / prevClip.w;
        float2 prevUV = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
        if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f)) {
            float4 hist = historyTex.sample(linearSampler, prevUV);
            float expected = length(hitWorld - gi.prevCameraPosition.xyz);
            if (hist.a > 0.0f && fabs(hist.a - expected) < 0.05f * expected + voxelSize) {
                result = mix(radiance, hist.rgb, gi.prevCameraPosition.w);
            }
        }
    }

    giOut.write(float4(result, camDist), gid);
}

// ============================================================================
// À-trous denoise (SVGF-lite): one iteration per dispatch with increasing
// step size, ping-ponging two half-res targets. Display-only — the temporal
// history keeps the RAW accumulation.
// ============================================================================

struct MvAtrousPush {
    int4 stepAndSize;  // x = stepSize, y = giWidth, z = giHeight
    float4 sigmas;     // x = depth, y = normal, z = luma
};

static inline float mvLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

kernel void microVoxelAtrousKernel(
    uint2 gid [[thread_position_in_grid]],
    constant MvAtrousPush& push [[buffer(0)]],
    texture2d<float, access::write> atrousOut [[texture(0)]],
    texture2d<float, access::sample> giIn [[texture(1)]],
    texture2d<float, access::sample> normalMatTex [[texture(2)]]
) {
    constexpr sampler pointSampler(filter::nearest, address::clamp_to_edge);

    int2 giSize = int2(push.stepAndSize.y, push.stepAndSize.z);
    if (int(gid.x) >= giSize.x || int(gid.y) >= giSize.y) return;

    float2 texel = 1.0 / float2(giSize);
    float2 uv = (float2(gid) + 0.5) * texel;
    float4 center = giIn.sample(pointSampler, uv);
    if (center.a <= 0.0f) {
        atrousOut.write(center, gid);
        return;
    }
    float3 n0 = mvDecodeNormal(int(normalMatTex.sample(pointSampler, uv).r * 255.0f + 0.5f));
    float l0 = mvLuma(center.rgb);
    float stepSize = float(push.stepAndSize.x);

    constant float kernelW[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };
    float3 sum = float3(0.0);
    float wSum = 0.0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            float2 tuv = uv + float2(dx, dy) * stepSize * texel;
            if (any(tuv < 0.0f) || any(tuv > 1.0f)) continue;
            float4 tap = giIn.sample(pointSampler, tuv);
            if (tap.a <= 0.0f) continue;

            float3 nt = mvDecodeNormal(int(normalMatTex.sample(pointSampler, tuv).r * 255.0f + 0.5f));
            float wN = pow(max(dot(n0, nt), 0.0f), push.sigmas.y);
            float wZ = exp(-fabs(center.a - tap.a) / (push.sigmas.x * stepSize + 1e-3f));
            float wL = exp(-fabs(l0 - mvLuma(tap.rgb)) / (push.sigmas.z + 1e-3f));
            float w = kernelW[dx + 2] * kernelW[dy + 2] * wN * wZ * wL;
            sum += tap.rgb * w;
            wSum += w;
        }
    }
    float3 filtered = (wSum > 1e-5f) ? sum / wSum : center.rgb;
    atrousOut.write(float4(filtered, center.a), gid);
}

// ============================================================================
// GI composite: fullscreen pass-through + albedo * gi * ao on voxel pixels,
// colorRT -> tempColorRT (the renderer swaps). Split compare + GI debug view.
// ============================================================================

struct MvCompositeVertexOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 mvFsTriVerts[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

vertex MvCompositeVertexOut microVoxelGICompositeVertex(uint vertexID [[vertex_id]]) {
    MvCompositeVertexOut out;
    out.position = float4(mvFsTriVerts[vertexID], 0.0, 1.0);
    out.uv = mvFsTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;
    return out;
}

fragment float4 microVoxelGICompositeFragment(
    MvCompositeVertexOut in [[stage_in]],
    constant MicroVoxelGIData& gi [[buffer(0)]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> hitTTex [[texture(1)]],
    texture2d<float, access::sample> albedoAOTex [[texture(2)]],
    texture2d<float, access::sample> giDenoisedTex [[texture(3)]],
    texture2d<float, access::sample> giRawTex [[texture(4)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);
    float2 uv = in.uv;

    float4 color = sceneColor.sample(linearSampler, uv);
    float hitT = hitTTex.sample(linearSampler, uv).r;
    if (hitT <= 0.0f) return color;

    float3 giSample;
    float splitX = gi.giParams.y;
    if (splitX >= 0.0f && uv.x < splitX) {
        giSample = giRawTex.sample(linearSampler, uv).rgb;
    } else {
        giSample = giDenoisedTex.sample(linearSampler, uv).rgb;
    }

    float4 albedoAO = albedoAOTex.sample(linearSampler, uv);
    float3 indirect = albedoAO.rgb * giSample * albedoAO.a * gi.giParams.x;

    float3 result = color.rgb + indirect;
    if (gi.giSigmas.w > 0.5f) result = giSample;
    if (splitX >= 0.0f && fabs(uv.x - splitX) < 0.0012f) result = float3(1.0);

    return float4(result, color.a);
}
