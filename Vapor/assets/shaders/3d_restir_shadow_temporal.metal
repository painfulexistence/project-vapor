#include <metal_stdlib>
using namespace metal;
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

    // World position reconstruction — same convention as the stochastic kernel.
    float2 uv = float2(tid) / float2(w, h);
    float2 ndcXY = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float4 ndc = float4(ndcXY, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float3 worldNormal = normalize(normalTexture.read(tid).xyz);
    float viewDepth = viewPos.z;

    uint rng = tid.x * 1973u + tid.y * 9277u + params.frameIndex * 26699u;

    // Tile cluster — matches the PBR/stochastic kernels' 2D tile convention.
    uint tileX = uint(uv.x * float(params.gridDims.x));
    uint tileY = uint((1.0 - uv.y) * float(params.gridDims.y));
    const device Cluster& cluster = clusters[tileX + tileY * params.gridDims.x];
    uint lightCount = min(cluster.lightCount, MAX_LIGHTS_PER_CLUSTER);

    // ---- Fresh candidates (uniform proposal q, resampling weight p̂/q) ----
    WRSReservoir rPoint = wrsEmpty();
    if (lightCount > 0) {
        for (uint i = 0; i < params.pointCandidates; i++) {
            uint slot = min(uint(restirRand(rng) * float(lightCount)), lightCount - 1);
            uint li = cluster.lightIndices[slot];
            float pdf = restirPointPdf(pointLights[li], worldPos, worldNormal);
            wrsUpdate(rPoint, li, pdf * float(lightCount), pdf, restirRand(rng));
        }
    }

    WRSReservoir rRect = wrsEmpty();
    if (params.rectCount > 0) {
        for (uint i = 0; i < params.rectCandidates; i++) {
            uint ri = min(uint(restirRand(rng) * float(params.rectCount)), params.rectCount - 1);
            float2 quadUV = float2(restirRand(rng), restirRand(rng));
            float pdf = restirRectPdf(rectLights[ri], quadUV, worldPos, worldNormal);
            wrsUpdate(rRect, restirPackRect(ri, quadUV), pdf * float(params.rectCount), pdf, restirRand(rng));
        }
    }

    WRSReservoir rSpot = wrsEmpty();
    if (params.spotCount > 0) {
        for (uint i = 0; i < params.spotCandidates; i++) {
            uint si = min(uint(restirRand(rng) * float(params.spotCount)), params.spotCount - 1);
            float pdf = restirSpotPdf(spotLights[si], worldPos, worldNormal);
            wrsUpdate(rSpot, si, pdf * float(params.spotCount), pdf, restirRand(rng));
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
            // Disocclusion guard: the sample survives only if the surface it
            // was built on is at a compatible depth (sky history has depth 0).
            if (hist.viewDepth != 0.0 &&
                abs(hist.viewDepth - viewDepth) <= params.depthTolerance * abs(viewDepth)) {
                uint idx = restirUnpackIdx(hist.pointData);
                float M = min(restirUnpackM(hist.pointData), params.pointMClamp);
                if (idx < params.pointCount && M > 0.0 && hist.pointW > 0.0) {
                    float pdf = restirPointPdf(pointLights[idx], worldPos, worldNormal);
                    wrsMerge(rPoint, idx, pdf, hist.pointW, M, restirRand(rng));
                }

                idx = restirUnpackRectIdx(hist.rectData);
                M = min(float(hist.rectM & 0xFFFFu), params.rectMClamp);
                if (idx < params.rectCount && M > 0.0 && hist.rectW > 0.0) {
                    float2 quadUV = restirUnpackRectUV(hist.rectData);
                    float pdf = restirRectPdf(rectLights[idx], quadUV, worldPos, worldNormal);
                    wrsMerge(rRect, hist.rectData, pdf, hist.rectW, M, restirRand(rng));
                }

                idx = restirUnpackIdx(hist.spotData);
                M = min(restirUnpackM(hist.spotData), params.pointMClamp);
                if (idx < params.spotCount && M > 0.0 && hist.spotW > 0.0) {
                    float pdf = restirSpotPdf(spotLights[idx], worldPos, worldNormal);
                    wrsMerge(rSpot, idx, pdf, hist.spotW, M, restirRand(rng));
                }
            }
        }
    }

    // ---- Store -------------------------------------------------------------
    ShadowReservoirSet out;
    bool pointValid = rPoint.pdf > 0.0;
    out.pointData = restirPackIdxM(pointValid ? rPoint.candidate : RESTIR_INVALID_LIGHT, rPoint.M);
    out.pointW = wrsFinalizeW(rPoint);
    bool spotValid = rSpot.pdf > 0.0;
    out.spotData = restirPackIdxM(spotValid ? rSpot.candidate : RESTIR_INVALID_LIGHT, rSpot.M);
    out.spotW = wrsFinalizeW(rSpot);
    bool rectValid = rRect.pdf > 0.0;
    out.rectData = rectValid ? rRect.candidate : RESTIR_INVALID_RECT;
    out.rectW = wrsFinalizeW(rRect);
    out.rectM = min(uint(rRect.M + 0.5), 0xFFFFu);
    out.viewDepth = viewDepth;
    outReservoirs[pixel] = out;
}
