#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// Temporal accumulation denoiser for the stochastic RT shadows (point / rect /
// spot — the R/G/B channels). Velocity reprojection + adaptive-alpha (SVGF
// history-length) accumulation; see the alpha derivation below.
kernel void computeMain(
    texture2d<float>              currentShadow   [[texture(0)]],
    texture2d<float>              historyShadow   [[texture(1)]],
    texture2d<float>              velocityTexture [[texture(2)]],
    texture2d<float, access::write> outputShadow  [[texture(3)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = outputShadow.get_width();
    uint h = outputShadow.get_height();
    if (tid.x >= w || tid.y >= h) return;

    float2 screenSize = float2(w, h);
    float2 uv = (float2(tid) + 0.5) / screenSize;

    // Three visibility channels: R = point, G = rect area, B = spot — all
    // accumulated identically. On the legacy R16F targets (native path) the
    // extra channels read as 0 and the writes drop them — harmless.
    float3 current = currentShadow.read(tid).rgb;

    // Velocity from 3d_velocity.metal is (currNDC - prevNDC) * 0.5 in y-up NDC
    // space; convert to y-down texture UV by negating the y component.
    float2 velocity = velocityTexture.read(tid).rg;
    float2 prevUV = uv - float2(velocity.x, -velocity.y);

    // 3x3 mean of the current frame — a per-frame denoised estimate used only
    // to decide whether the SHADOW changed (below), robust to single-ray flips.
    float3 mean = float3(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            uint2 nc = uint2(clamp(int2(tid) + int2(dx, dy), int2(0), int2(w-1, h-1)));
            mean += currentShadow.read(nc).rgb;
        }
    }
    mean /= 9.0;

    // Adaptive temporal accumulation (SVGF-style history length) instead of a
    // fixed 15% blend + variance clamp. The shadow factors are stochastic
    // FRACTIONS (a rect penumbra converges on its covered fraction) sampled
    // with 1-2 binary rays/frame; a fixed alpha leaves a permanent EMA
    // variance floor no amount of frames removes. Here alpha = 1/(N+1) where N
    // is how many frames this surface point has accumulated: a STATIC penumbra
    // drives N up and alpha down, averaging the binary samples toward the true
    // fraction (clean); alpha springs back up on a reset so response stays
    // fast. The frame count rides in the history's unused alpha channel (the
    // denoise pass writes rgb only, so it survives the swap).
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float4 hist = historyShadow.sample(s, prevUV);
    // Sanitize: the history RT is uninitialized on the first frame after a
    // resize. Shadow factors are [0,1] and the frame count [0,256], so these
    // clamps also scrub any NaN/Inf (min/max return the non-NaN operand) before
    // it can feed back through the accumulator.
    float3 history = clamp(hist.rgb, 0.0, 1.0);
    float histLen = clamp(hist.a, 0.0, 256.0);

    // History length reset — springs alpha back up so the accumulator drops
    // stale history. Two triggers, deliberately asymmetric:
    //   - Off-screen reproject = real disocclusion, unambiguous -> HARD reset.
    //   - The denoised current (3x3 mean) departing from history = the shadow
    //     may have moved, but a penumbra's coherent per-frame sampling swing
    //     also trips it occasionally. A hard reset there flashes raw 0/1 current
    //     as a bright noise band, so this trigger is CAPPED: it only partially
    //     shortens history, nudging alpha up a little. A one-frame penumbra
    //     spike becomes an imperceptible bump; a SUSTAINED real change keeps
    //     shortening history each frame and still catches up within a few.
    bool onScreen = all(prevUV >= 0.0) && all(prevUV <= 1.0);
    float3 d = abs(mean - history);
    float changed = max(d.r, max(d.g, d.b));
    // History-shortening resets:
    //  - Off-screen reproject = disocclusion, unambiguous -> HARD reset.
    //  - VALUE change (shadow moved on static geometry) -> capped at 0.5 so a
    //    static penumbra sampling spike can't flash a noise band.
    float valueReset = saturate((changed - 0.30) / 0.25) * 0.5;
    float reset = onScreen ? valueReset : 1.0;
    histLen *= (1.0 - reset);

    // Camera motion does NOT hard-reset. Under a pan a static shadow reprojects
    // correctly (value unchanged, so the value-reset never fires) but bilinear
    // reprojection still smears the history; at the converged alpha of 0.04
    // that smear is a slow ghost trail. A hard reset kills the trail but flashes
    // raw 1-2 ray noise. Instead, CAP the history length by motion so alpha
    // can't fall below ~0.5 while moving: half the frame stays averaged history
    // (far less noise than a full reset) while the smear still decays ~50% per
    // frame — a faint few-frame trail, not the old persistent one. Static keeps
    // full convergence. Raise the cap toward 256 for less noise / more trail.
    float pxMotion = length(velocity * screenSize);
    float maxHistLen = mix(256.0, 1.0, saturate((pxMotion - 0.5) * 0.5));
    histLen = min(histLen, maxHistLen);

    float alpha = max(1.0 / (histLen + 1.0), 0.04);  // floor: stay responsive
    float3 result = mix(history, current, alpha);
    histLen = min(histLen + 1.0, 256.0);
    outputShadow.write(float4(result, histLen), tid);
}
