#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using raytracing::instance_acceleration_structure;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/restir_shadow_common.metal"

// ReSTIR stochastic-shadow pass 2: spatial reuse + winner visibility rays.
// Merges a few geometry-compatible neighbor reservoirs into the pixel's own
// (biased combiner — neighbor W is reused as-is under depth/normal guards),
// traces the merged winner's visibility, and writes
//   R = point visibility, G = rect-area visibility, B = spot visibility
// to the HALF-RES raw shadow target (joint-bilaterally upsampled to full res
// by 3d_point_shadow_upsample.metal before the temporal accumulator).
//
// HALF-RES: same fp = tid*2 mapping as the temporal kernel — G-buffer reads
// at fp, reservoir grid and shadow writes on the half grid. The 4x ray/eval
// saving is partly reinvested: the rect winner gets FOUR quad-point rays per
// half-pixel (a per-frame 2x2 stratum riding a per-pixel R2 walk — fresh
// every frame, never reservoir-frozen; see restir_shadow_common.metal), so
// penumbra variance is halved vs the old full-res 2-ray version while the
// total cost is still ~1.5 rays per full-res pixel (legacy cap: 4). The
// post-spatial reservoirs become next frame's temporal history.

// Plastic-constant (R2) low-discrepancy increment for the rect quad walk.
constant float2 kR2 = float2(0.7548776662, 0.5698402910);

static float traceVisibility(
    instance_acceleration_structure TLAS,
    float3 origin, float3 normal, float3 target
) {
    float3 toL = target - origin;
    float dist = length(toL);
    if (dist <= 0.01) return 1.0;
    float3 dir = toL / dist;
    if (dot(dir, normal) <= 0.0) return 1.0;
    raytracing::ray ray;
    ray.origin = origin + normal * 0.005;
    ray.direction = dir;
    ray.min_distance = 0.001;
    ray.max_distance = dist - 0.01;
    raytracing::intersector<raytracing::instancing> occl;
    occl.assume_geometry_type(raytracing::geometry_type::triangle);
    occl.accept_any_intersection(true);
    auto hit = occl.intersect(ray, TLAS, 0xFF);
    return (hit.type == raytracing::intersection_type::none) ? 1.0 : 0.0;
}

kernel void computeMain(
    texture2d<float>              depthTexture   [[texture(0)]],
    texture2d<float>              normalTexture  [[texture(1)]],
    texture2d<float, access::write> shadowTexture [[texture(2)]],
    constant CameraData&          camera         [[buffer(0)]],
    const device PointLight*      pointLights    [[buffer(1)]],
    const device Cluster*         clusters       [[buffer(2)]],
    const device SpotLight*       spotLights     [[buffer(3)]],
    const device RectLight*       rectLights     [[buffer(4)]],
    const device ShadowReservoirSet* inReservoirs [[buffer(5)]],
    device ShadowReservoirSet*    history        [[buffer(6)]],
    instance_acceleration_structure TLAS         [[buffer(7)]],
    constant RestirShadowParams&  params         [[buffer(8)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = uint(params.screenSize.x);
    uint h = uint(params.screenSize.y);
    uint halfW = (w + 1u) / 2u;
    uint halfH = (h + 1u) / 2u;
    if (tid.x >= halfW || tid.y >= halfH) return;
    uint pixel = tid.y * halfW + tid.x;
    uint2 fp = tid * 2u;  // representative full-res pixel of this half texel

    float depth = depthTexture.read(fp).r;
    if (depth >= 1.0) {
        shadowTexture.write(float4(params.debugMode == 1 ? 0.0 : 1.0), tid);
        history[pixel] = restirEmptySet(0.0);
        return;
    }

    RestirSurface surf = restirReconstructSurface(fp, w, h, depth, camera);
    float3 worldNormal = normalize(normalTexture.read(fp).xyz);

    ShadowReservoirSet own = inReservoirs[pixel];

    // Tile heatmap debug (parity with the legacy kernel's debugMode 1).
    if (params.debugMode == 1) {
        float2 uv = float2(fp) / float2(w, h);
        uint lightCount = clusters[restirClusterIndex(uv, params.gridDims)].lightCount;
        shadowTexture.write(float4(float(lightCount) / 8.0), tid);
        history[pixel] = own; // keep reservoir state flowing
        return;
    }

    uint rng = tid.x * 2371u + tid.y * 8933u + params.frameIndex * 15881u;

    // Rebuild working reservoirs from the stored (sample, W, M) form:
    // wSum = W * M * p̂(y) with p̂ evaluated here (same pixel, same frame).
    WRSReservoir rPoint = wrsEmpty();
    {
        uint idx = restirUnpackIdx(own.pointData);
        if (idx < params.pointCount && own.pointW > 0.0 && isfinite(own.pointW)) {
            float pdf = restirPointPdf(pointLights[idx], surf.worldPos, worldNormal);
            rPoint.candidate = idx;
            rPoint.pdf = pdf;
            rPoint.wSum = own.pointW * restirUnpackM(own.pointData) * pdf;
        }
        rPoint.M = restirUnpackM(own.pointData);
    }
    WRSReservoir rRect = wrsEmpty();
    {
        uint idx = restirUnpackIdx(own.rectData);
        if (idx < params.rectCount && own.rectW > 0.0 && isfinite(own.rectW)) {
            float2 quadUV = float2(randomNext(rng), randomNext(rng));
            float pdf = restirRectPdf(rectLights[idx], quadUV, surf.worldPos, worldNormal);
            rRect.candidate = idx;
            rRect.pdf = pdf;
            rRect.wSum = own.rectW * restirUnpackM(own.rectData) * pdf;
        }
        rRect.M = restirUnpackM(own.rectData);
    }
    WRSReservoir rSpot = wrsEmpty();
    {
        uint idx = restirUnpackIdx(own.spotData);
        if (idx < params.spotCount && own.spotW > 0.0 && isfinite(own.spotW)) {
            float pdf = restirSpotPdf(spotLights[idx], surf.worldPos, worldNormal);
            rSpot.candidate = idx;
            rSpot.pdf = pdf;
            rSpot.wSum = own.spotW * restirUnpackM(own.spotData) * pdf;
        }
        rSpot.M = restirUnpackM(own.spotData);
    }

    // ---- Spatial reuse ------------------------------------------------------
    // Tap offsets are in HALF-res pixels; the CPU pre-scales spatialRadius by
    // 0.5 so the panel value keeps meaning full-res pixels of visual radius.
    float angle = randomNext(rng) * 2.0 * PI;
    float2x2 rot = float2x2(float2(cos(angle), sin(angle)),
                            float2(-sin(angle), cos(angle)));
    for (uint t = 0; t < params.spatialTaps; t++) {
        float2 offset = rot * poissonDisk8[t % 8u] * params.spatialRadius;
        int2 nb = int2(float2(tid) + offset + 0.5);
        if (nb.x < 0 || nb.y < 0 || nb.x >= int(halfW) || nb.y >= int(halfH)) continue;
        if (uint(nb.x) == tid.x && uint(nb.y) == tid.y) continue;

        ShadowReservoirSet nbSet = inReservoirs[uint(nb.y) * halfW + uint(nb.x)];
        // Geometry compatibility: same-ish depth (sky neighbors carry 0) and
        // same-ish orientation, else the reused W darkens across silhouettes.
        // The neighbor's normal rides in its reservoir — no texture fetch.
        if (nbSet.viewDepth == 0.0 ||
            abs(nbSet.viewDepth - surf.viewDepth) > params.depthTolerance * abs(surf.viewDepth)) continue;
        if (dot(restirUnpackNormal(nbSet.packedNormal), worldNormal) < params.normalTolerance) continue;

        uint idx = restirUnpackIdx(nbSet.pointData);
        if (idx < params.pointCount && nbSet.pointW > 0.0 && isfinite(nbSet.pointW)) {
            float pdf = restirPointPdf(pointLights[idx], surf.worldPos, worldNormal);
            wrsMerge(rPoint, idx, pdf, nbSet.pointW, restirUnpackM(nbSet.pointData), randomNext(rng));
        }
        idx = restirUnpackIdx(nbSet.rectData);
        if (idx < params.rectCount && nbSet.rectW > 0.0 && isfinite(nbSet.rectW)) {
            float2 quadUV = float2(randomNext(rng), randomNext(rng));
            float pdf = restirRectPdf(rectLights[idx], quadUV, surf.worldPos, worldNormal);
            wrsMerge(rRect, idx, pdf, nbSet.rectW, restirUnpackM(nbSet.rectData), randomNext(rng));
        }
        idx = restirUnpackIdx(nbSet.spotData);
        if (idx < params.spotCount && nbSet.spotW > 0.0 && isfinite(nbSet.spotW)) {
            float pdf = restirSpotPdf(spotLights[idx], surf.worldPos, worldNormal);
            wrsMerge(rSpot, idx, pdf, nbSet.spotW, restirUnpackM(nbSet.spotData), randomNext(rng));
        }
    }

    // ---- Store post-spatial reservoirs as next frame's history --------------
    history[pixel] = restirPackSet(rPoint, rRect, rSpot, surf.viewDepth, worldNormal);

    // Debug views need only the merged reservoirs — return before spending rays.
    if (params.debugMode == 2) {
        // Winner id bands (0 = no winner) — selection stability check.
        float3 ids = float3(
            rPoint.pdf > 0.0 ? (float(rPoint.candidate % 8u) + 1.0) / 9.0 : 0.0,
            rRect.pdf > 0.0 ? (float(rRect.candidate % 8u) + 1.0) / 9.0 : 0.0,
            rSpot.pdf > 0.0 ? (float(rSpot.candidate % 8u) + 1.0) / 9.0 : 0.0);
        shadowTexture.write(float4(ids, 1.0), tid);
        return;
    }
    if (params.debugMode == 3) {
        // Reservoir confidence (M relative to the temporal clamp).
        float3 ms = float3(rPoint.M / max(params.pointMClamp * 2.0, 1.0),
                           rRect.M / max(params.rectMClamp * 2.0, 1.0),
                           rSpot.M / max(params.spotMClamp * 2.0, 1.0));
        shadowTexture.write(float4(saturate(ms), 1.0), tid);
        return;
    }

    // ---- Winner visibility (one ray per domain) -----------------------------
    bool tlasValid = !is_null_instance_acceleration_structure(TLAS);
    float pointVis = 1.0;
    if (rPoint.pdf > 0.0 && tlasValid) {
        pointVis = traceVisibility(TLAS, surf.worldPos, worldNormal,
                                   pointLights[rPoint.candidate].position);
    }
    float rectVis = 1.0;
    if (rRect.pdf > 0.0 && tlasValid) {
        // Four quad points per frame, stratified in space AND time: a static
        // per-pixel Cranley-Patterson rotation + an R2 walk over the frame
        // index gives the base point, and the other three fill the remaining
        // 2x2 strata (half-quad offsets). Independent random points would
        // leave the accumulator's EMA a variance floor of alpha/(2-alpha)
        // forever; the stratified walk covers the quad evenly inside the EMA
        // window, so the accumulated mean actually converges on the coverage
        // fraction. (& 1023 keeps the float walk inside fract() precision —
        // a 1024-frame stratification cycle.)
        RectLight rl = rectLights[rRect.candidate];
        float2 cp = random(tid.x * 9781u + tid.y * 6271u);
        float2 uv0 = fract(cp + float(params.frameIndex & 1023u) * kR2);
        rectVis = 0.25 * (traceVisibility(TLAS, surf.worldPos, worldNormal, restirRectPoint(rl, uv0)) +
                          traceVisibility(TLAS, surf.worldPos, worldNormal, restirRectPoint(rl, fract(uv0 + float2(0.5, 0.0)))) +
                          traceVisibility(TLAS, surf.worldPos, worldNormal, restirRectPoint(rl, fract(uv0 + float2(0.0, 0.5)))) +
                          traceVisibility(TLAS, surf.worldPos, worldNormal, restirRectPoint(rl, fract(uv0 + float2(0.5, 0.5)))));
    }
    float spotVis = 1.0;
    if (rSpot.pdf > 0.0 && tlasValid) {
        spotVis = traceVisibility(TLAS, surf.worldPos, worldNormal,
                                  spotLights[rSpot.candidate].position);
    }

    shadowTexture.write(float4(pointVis, rectVis, spotVis, 1.0), tid);
}
