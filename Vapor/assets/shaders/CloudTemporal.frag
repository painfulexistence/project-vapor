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
};

void main() {
    vec4 current = texture(currentCloud, tex_uv);

    // Reproject via depth + prevViewProj (faithful to the Metal resolve).
    float depth = texture(sceneDepth, tex_uv).r;
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 worldPos = invViewProj * vec4(ndc, depth, 1.0);
    worldPos /= worldPos.w;

    vec4 prevClip = prevViewProj * worldPos;
    vec2 prevNDC = prevClip.xy / prevClip.w;
    vec2 prevUV = vec2(prevNDC.x * 0.5 + 0.5, 1.0 - (prevNDC.y * 0.5 + 0.5));

    vec4 history = texture(historyCloud, prevUV);

    bool validHistory = prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                        prevUV.y >= 0.0 && prevUV.y <= 1.0 && prevClip.w > 0.0;

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

    float blend = validHistory ? temporalBlend : 1.0;
    outCloud = mix(history, current, blend);
}
