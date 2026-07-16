#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/restir_shadow_common.metal"

// ReSTIR stochastic-shadow pass 1: initial candidates + temporal reuse.
// Streams a handful of fresh light samples per domain through a weighted
// reservoir (target = unshadowed contribution), then merges the reprojected
// previous-frame reservoir under an M clamp. Pure ALU — the single shadow ray
// per domain is traced by the resolve pass for the post-spatial winner.
kernel void computeMain(
    texture2d<float>              depthTexture    [[texture(0)]],
    texture2d<float>              normalTexture   [[texture(1)]],
    texture2d<float>              velocityTexture [[texture(2)]],
    constant CameraData&          camera          [[buffer(0)]],
    const device PointLight*      pointLights     [[buffer(1)]],
    const device Cluster*         clusters        [[buffer(2)]],
    const device SpotLight*       spotLights      [[buffer(3)]],
    const device RectLight*       rectLights      [[buffer(4)]],
    const device ShadowReservoirSet* history      [[buffer(5)]],
    device ShadowReservoirSet*    outReservoirs   [[buffer(6)]],
    constant RestirShadowParams&  params          [[buffer(7)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = uint(params.screenSize.x);
    uint h = uint(params.screenSize.y);
    if (tid.x >= w || tid.y >= h) return;
    uint pixel = tid.y * w + tid.x;

    float depth = depthTexture.read(tid).r;
    if (depth >= 1.0) {
        // Sky: empty reservoirs (viewDepth 0 marks them unusable for reuse).
        outReservoirs[pixel] = restirEmptySet(0.0);
        return;
    }

    RestirSurface surf = restirReconstructSurface(tid, w, h, depth, camera);
    float3 worldNormal = normalize(normalTexture.read(tid).xyz);

    uint rng = tid.x * 1973u + tid.y * 9277u + params.frameIndex * 26699u;

    float2 uv = float2(tid) / float2(w, h);
    const device Cluster& cluster = clusters[restirClusterIndex(uv, params.gridDims)];
    uint lightCount = min(cluster.lightCount, MAX_LIGHTS_PER_CLUSTER);

    // ---- Fresh candidates (uniform proposal q, resampling weight p̂/q) ----
    // With a single light the pick is deterministic — one evaluation suffices.
    WRSReservoir rPoint = wrsEmpty();
    if (lightCount > 0) {
        uint candidates = (lightCount == 1) ? 1u : params.pointCandidates;
        for (uint i = 0; i < candidates; i++) {
            uint slot = min(uint(randomNext(rng) * float(lightCount)), lightCount - 1);
            uint li = cluster.lightIndices[slot];
            float pdf = restirPointPdf(pointLights[li], surf.worldPos, worldNormal);
            wrsUpdate(rPoint, li, pdf * float(lightCount), pdf, randomNext(rng));
        }
    }

    WRSReservoir rRect = wrsEmpty();
    if (params.rectCount > 0) {
        // No single-light shortcut here: each candidate is a distinct quad point.
        for (uint i = 0; i < params.rectCandidates; i++) {
            uint ri = min(uint(randomNext(rng) * float(params.rectCount)), params.rectCount - 1);
            float2 quadUV = float2(randomNext(rng), randomNext(rng));
            float pdf = restirRectPdf(rectLights[ri], quadUV, surf.worldPos, worldNormal);
            wrsUpdate(rRect, restirPackRect(ri, quadUV), pdf * float(params.rectCount), pdf, randomNext(rng));
        }
    }

    WRSReservoir rSpot = wrsEmpty();
    if (params.spotCount > 0) {
        uint candidates = (params.spotCount == 1) ? 1u : params.spotCandidates;
        for (uint i = 0; i < candidates; i++) {
            uint si = min(uint(randomNext(rng) * float(params.spotCount)), params.spotCount - 1);
            float pdf = restirSpotPdf(spotLights[si], surf.worldPos, worldNormal);
            wrsUpdate(rSpot, si, pdf * float(params.spotCount), pdf, randomNext(rng));
        }
    }

    // ---- Temporal reuse ----------------------------------------------------
    if (params.historyValid != 0u) {
        // Velocity is (currNDC - prevNDC) * 0.5 in y-up NDC; y-down UV negates y.
        float2 velocity = velocityTexture.read(tid).rg;
        float2 prevUV = (float2(tid) + 0.5) / float2(w, h) - float2(velocity.x, -velocity.y);
        int2 prevPix = int2(floor(prevUV * float2(w, h)));
        if (all(prevPix >= 0) && prevPix.x < int(w) && prevPix.y < int(h)) {
            ShadowReservoirSet hist = history[uint(prevPix.y) * w + uint(prevPix.x)];
            // Disocclusion guards: same-ish depth (sky history carries 0) AND
            // same-ish orientation — a corner pixel at compatible depth must
            // not inherit the other face's reservoir.
            if (hist.viewDepth != 0.0 &&
                abs(hist.viewDepth - surf.viewDepth) <= params.depthTolerance * abs(surf.viewDepth) &&
                dot(restirUnpackNormal(hist.rectM), worldNormal) >= params.normalTolerance) {
                uint idx = restirUnpackIdx(hist.pointData);
                float M = min(restirUnpackM(hist.pointData), params.pointMClamp);
                if (idx < params.pointCount && M > 0.0 && hist.pointW > 0.0 && isfinite(hist.pointW)) {
                    float pdf = restirPointPdf(pointLights[idx], surf.worldPos, worldNormal);
                    wrsMerge(rPoint, idx, pdf, hist.pointW, M, randomNext(rng));
                }

                idx = restirUnpackRectIdx(hist.rectData);
                M = min(restirUnpackRectM(hist.rectM), params.rectMClamp);
                if (idx < params.rectCount && M > 0.0 && hist.rectW > 0.0 && isfinite(hist.rectW)) {
                    float2 quadUV = restirUnpackRectUV(hist.rectData);
                    float pdf = restirRectPdf(rectLights[idx], quadUV, surf.worldPos, worldNormal);
                    wrsMerge(rRect, hist.rectData, pdf, hist.rectW, M, randomNext(rng));
                }

                idx = restirUnpackIdx(hist.spotData);
                M = min(restirUnpackM(hist.spotData), params.spotMClamp);
                if (idx < params.spotCount && M > 0.0 && hist.spotW > 0.0 && isfinite(hist.spotW)) {
                    float pdf = restirSpotPdf(spotLights[idx], surf.worldPos, worldNormal);
                    wrsMerge(rSpot, idx, pdf, hist.spotW, M, randomNext(rng));
                }
            }
        }
    }

    outReservoirs[pixel] = restirPackSet(rPoint, rRect, rSpot, surf.viewDepth, worldNormal);
}
