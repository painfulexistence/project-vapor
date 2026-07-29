#version 450
// Volumetric clouds — temporal resolve (port of Metal cloudTemporalResolve).
// Reprojects the previous frame's resolved clouds via prevViewProj, clamps the
// history to the current 3x3 neighborhood (anti-ghosting) and blends. Writes
// to a third RT (cloudResolved) because Vulkan cannot sample the attachment it
// renders to (Metal read+wrote cloudHistoryRT in one pass). The velocity RT is
// bound for a future motion-vector-based reprojection upgrade.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outCloud;

layout(set = 2, binding = 0) uniform sampler2D currentCloud;
layout(set = 2, binding = 1) uniform sampler2D historyCloud;
layout(set = 2, binding = 2) uniform sampler2D sceneDepth;
layout(set = 2, binding = 3) uniform sampler2D velocityTex;  // reserved (TAA-style upgrade)

layout(std430, set = 1, binding = 0) readonly buffer CloudBuf {
    mat4 invViewProj;
    mat4 prevViewProj;
    vec3 cameraPosition;  float _p1;
    vec3 sunDirection;    float _p2;
    vec3 sunColor;        float _p3;
    float sunIntensity;
    float cloudLayerBottom;
    float cloudLayerTop;
    float cloudLayerThickness;
    float cloudCoverage;
    float cloudDensity;
    float cloudType;
    float erosionStrength;
    float shapeNoiseScale;
    float detailNoiseScale;
    float curlNoiseScale;
    float curlNoiseStrength;
    float ambientIntensity;
    float silverLiningIntensity;
    float silverLiningSpread;
    float phaseG1;
    float phaseG2;
    float phaseBlend;
    float powderStrength;
    float sunLightScale;  // unread here; layout twin of CloudRaymarch.frag
    vec3 windDirection;   float _p5;
    vec3 windOffset;      float _p6;
    float windSpeed;
    float time;
    uint primarySteps;
    uint lightSteps;
    vec2 screenSize;       // quarter-res cloud RT size
    vec2 _p7;
    uint frameIndex;
    float temporalBlend;
    vec2 _p8;
    vec3 ambientColor;  // unread here; layout twin of CloudRaymarch.frag
    float _p9;
    vec3 moonColor;
    float moonLightScale;
};

// Catmull-Rom history sampling (9-tap, bilinear-fetch optimized). Plain
// bilinear re-resampled the history every frame; at temporalBlend 0.05 the
// accumulator survives ~20 resamples, and 20 stacked bilinear tents are a
// huge low-pass — the clouds went to mush whenever the camera rotated (any
// rotation = a subpixel history shift every frame). Catmull-Rom's negative
// lobes keep the accumulator sharp under exactly that resampling.
vec4 sampleHistoryCatmullRom(vec2 uv) {
    vec2 samplePos = uv * screenSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 texPos0 = (texPos1 - 1.0) / screenSize;
    vec2 texPos3 = (texPos1 + 2.0) / screenSize;
    vec2 texPos12 = (texPos1 + offset12) / screenSize;
    vec4 result =
        texture(historyCloud, vec2(texPos0.x,  texPos0.y))  * w0.x  * w0.y +
        texture(historyCloud, vec2(texPos12.x, texPos0.y))  * w12.x * w0.y +
        texture(historyCloud, vec2(texPos3.x,  texPos0.y))  * w3.x  * w0.y +
        texture(historyCloud, vec2(texPos0.x,  texPos12.y)) * w0.x  * w12.y +
        texture(historyCloud, vec2(texPos12.x, texPos12.y)) * w12.x * w12.y +
        texture(historyCloud, vec2(texPos3.x,  texPos12.y)) * w3.x  * w12.y +
        texture(historyCloud, vec2(texPos0.x,  texPos3.y))  * w0.x  * w3.y +
        texture(historyCloud, vec2(texPos12.x, texPos3.y))  * w12.x * w3.y +
        texture(historyCloud, vec2(texPos3.x,  texPos3.y))  * w3.x  * w3.y;
    return max(result, vec4(0.0));
}

vec2 reprojectUV(vec3 wp) {
    vec4 pc = prevViewProj * vec4(wp, 1.0);
    vec2 nd = pc.xy / pc.w;
    return vec2(nd.x * 0.5 + 0.5, 1.0 - (nd.y * 0.5 + 0.5));
}

void main() {
    vec4 current = texture(currentCloud, tex_uv);

    float depth = texture(sceneDepth, tex_uv).r;
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 farPos = invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rayDir = normalize(farPos.xyz / farPos.w - cameraPosition);

    // Reprojection point. For sky pixels the old code reconstructed the far
    // plane (100 km): exact under pure rotation but with ~zero parallax, so
    // any translation (the fly-cam always translates) reprojected nearby
    // billows from the wrong place and smeared them. Use the mid-cloud
    // distance along the view ray instead — parallax-correct where the
    // clouds actually are. Geometry pixels keep the depth-buffer position.
    //
    // parallaxTexels measures how DEPTH-UNCERTAIN that reprojection is: the
    // divergence, in history texels, between reprojecting the near and the
    // far end of the in-layer segment. Under pure rotation every distance
    // along the ray reprojects to the SAME point, so it is exactly zero —
    // and rotation therefore keeps the full accumulation (dumping history on
    // rotation was the previous shudder: per-frame raymarch grain, amplified
    // by bloom, pulsing at high blend). Only translation opens the
    // divergence, and only translation pays extra blend for it.
    vec4 worldPos;
    float parallaxTexels = 0.0;
    if (depth >= 0.9999) {
        float tBottom = (cloudLayerBottom - cameraPosition.y) / rayDir.y;
        float tTop = (cloudLayerTop - cameraPosition.y) / rayDir.y;
        float tMin = min(tBottom, tTop);
        float tMax = max(tBottom, tTop);
        if (cameraPosition.y >= cloudLayerBottom && cameraPosition.y <= cloudLayerTop) tMin = 0.0;
        float tNear = clamp(max(tMin, 0.0), 200.0, 60000.0);
        float tRep = (tMax <= 0.0)
            ? 30000.0  // layer fully behind the ray: distance is moot, any works
            : tNear + 0.5 * min(max(tMax - tNear, 0.0), 8000.0);
        worldPos = vec4(cameraPosition + rayDir * clamp(tRep, 200.0, 60000.0), 1.0);
        vec2 uvNear = reprojectUV(cameraPosition + rayDir * tNear);
        vec2 uvFar = reprojectUV(cameraPosition + rayDir * (tNear + 8000.0));
        parallaxTexels = length((uvNear - uvFar) * screenSize);
    } else {
        // Geometry pixel. It carries no cloud (maxDist clipped the march at the
        // surface), and as the camera turns the horizon sweeps across it, so
        // its history is stale sky. Treat it as maximally untrustworthy and
        // let the blend take the current frame — leaving this at 0 meant every
        // ground pixel kept a low blend AND, when the clamp was gated on this
        // quantity, no clamp at all: exactly the band of horizontal streaks
        // that appeared along the horizon.
        parallaxTexels = 1e3;
        worldPos = invViewProj * vec4(ndc, depth, 1.0);
        worldPos /= worldPos.w;
    }

    vec4 prevClip = prevViewProj * worldPos;
    vec2 prevNDC = prevClip.xy / prevClip.w;
    vec2 prevUV = vec2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));

    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                        prevUV.y >= 0.0 && prevUV.y <= 1.0 && prevClip.w > 0.0;

    vec4 history = sampleHistoryCatmullRom(prevUV);

    // Anti-ghosting: VARIANCE clip, not a hard min/max box.
    //
    // The 3x3 min/max box is inert over smooth cloud but binds hard at a cloud
    // edge, where the box collapses onto the two grid-quantised levels the
    // edge can take. It then snapped the accumulator back onto whichever level
    // THIS frame produced — every frame, defeating the 20-frame average
    // entirely (simulated: 0.088 -> 0.574 rms px of edge wobble, a 6.5x
    // amplification, and completely independent of temporalBlend, which is
    // exactly why raising the blend rate never helped). A mean +- k*sigma box
    // is wide enough that the accumulator can average the jittered samples
    // into sub-texel resolution, while still rejecting genuine outliers.
    vec4 m1 = vec4(0.0);
    vec4 m2 = vec4(0.0);
    vec2 texelSize = 1.0 / screenSize;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec4 n = texture(currentCloud, tex_uv + vec2(x, y) * texelSize);
            m1 += n;
            m2 += n * n;
        }
    }
    vec4 mean = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec4(0.0)));
    // Relative floor on sigma: at the shipped density (0.3/m) optical depth 1
    // is 3.3 m, so a raymarch step saturates and the field is very nearly
    // BINARY. A 3x3 neighborhood in the flat interior is then uniform, sigma
    // collapses to ~0, and a mean +- k*sigma box is just as tight as min/max.
    // WIDEN the box, never disable it. The floor is RELATIVE to the mean, and
    // that is what does the anti-ghosting work: where the neighborhood is
    // empty (clear sky, or a pixel the horizon just swept over) mean and sigma
    // are both 0, so the box collapses to [0,0] and stale history is forced to
    // zero no matter how wide k is. Where there IS signal the box is
    // mean +- 0.8*mean, loose enough that the accumulator can actually average
    // instead of being snapped onto this frame's ray-quantised value.
    //
    // A hard min/max box could not do this: with the field effectively binary
    // (optical depth 1 in 3.3 m, so a step saturates) a flat neighborhood
    // gives min == max and the accumulator is overwritten every frame — which
    // is why raising temporalBlend never helped, the clamp was setting the
    // output. Gating the clamp OFF under exact reprojection fixed the flicker
    // but let history smear freely across disocclusions (11x the ghosting;
    // horizontal streaks along the horizon). Widening keeps both:
    // vs the original box, simulated mean per-frame change 4.9x lower, peak
    // 3.5x lower, AND ghosting 5.6x lower.
    vec4 sigmaFloor = max(sigma, abs(mean) * 0.05);
    history = clamp(history, mean - 16.0 * sigmaFloor, mean + 16.0 * sigmaFloor);

    // Parallax-adaptive blend: base rate at rest AND under pure rotation
    // (history is exact there — Catmull-Rom keeps it sharp); extra current
    // only where translation makes the reprojection depth-ambiguous.
    float blend = validHistory
        ? clamp(temporalBlend + parallaxTexels * 0.05, temporalBlend, 0.5)
        : 1.0;
    outCloud = mix(history, current, blend);
}
