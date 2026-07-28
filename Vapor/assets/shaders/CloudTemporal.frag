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
    vec4 worldPos;
    if (depth >= 0.9999) {
        float tBottom = (cloudLayerBottom - cameraPosition.y) / rayDir.y;
        float tTop = (cloudLayerTop - cameraPosition.y) / rayDir.y;
        float tMin = min(tBottom, tTop);
        float tMax = max(tBottom, tTop);
        if (cameraPosition.y >= cloudLayerBottom && cameraPosition.y <= cloudLayerTop) tMin = 0.0;
        float tRep = (tMax <= 0.0)
            ? 30000.0  // layer fully behind the ray: distance is moot, any works
            : max(tMin, 0.0) + 0.5 * min(tMax - max(tMin, 0.0), 8000.0);
        worldPos = vec4(cameraPosition + rayDir * clamp(tRep, 200.0, 60000.0), 1.0);
    } else {
        worldPos = invViewProj * vec4(ndc, depth, 1.0);
        worldPos /= worldPos.w;
    }

    vec4 prevClip = prevViewProj * worldPos;
    vec2 prevNDC = prevClip.xy / prevClip.w;
    vec2 prevUV = vec2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));

    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                        prevUV.y >= 0.0 && prevUV.y <= 1.0 && prevClip.w > 0.0;

    vec4 history = sampleHistoryCatmullRom(prevUV);

    // Neighborhood clamp (anti-ghosting).
    vec4 minBound = current;
    vec4 maxBound = current;
    vec2 texelSize = 1.0 / screenSize;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec4 n = texture(currentCloud, tex_uv + vec2(x, y) * texelSize);
            minBound = min(minBound, n);
            maxBound = max(maxBound, n);
        }
    }
    history = clamp(history, minBound, maxBound);

    // Motion-adaptive blend. At the fixed 0.05 the history dominated 20:1
    // while rotation kept shifting it, and the per-frame fight between the
    // stale clamped history and the noisy current read as shudder. Fold in
    // more current the faster the reprojection moves (in history texels):
    // still 0.05 at rest (deep accumulation), mostly-current under fast
    // rotation — grainier for a moment, but crisp and stable.
    float texelsMoved = length((tex_uv - prevUV) * screenSize);
    float blend = validHistory
        ? clamp(temporalBlend + texelsMoved * 0.08, temporalBlend, 0.65)
        : 1.0;
    outCloud = mix(history, current, blend);
}
