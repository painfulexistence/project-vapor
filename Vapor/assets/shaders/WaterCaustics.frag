#version 450
// Water caustics — fullscreen composite (GLSL twin of 3d_water_caustics_rhi.metal).
//
// Runs before the water surface pass: reconstructs the world position of every
// opaque pixel and, for pixels inside the water volume (the AABB in WaterData,
// below the water line), boosts the scene color by an animated caustic web so
// the pool floor and anything submerged picks up the moving light pattern.
// The water surface pass then snapshots this caustic-lit scene for refraction,
// so the pattern correctly shows through the surface. colorRT -> tempColorRT,
// the pass swaps them afterwards (HeightFog pattern).
//
// The pattern itself is the classic iterated-lattice water caustic: a few
// domain-warp iterations of sin/cos, sharpened by a power curve. Slight
// per-channel offset fakes chromatic dispersion. Uses FullScreen.vert.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

struct Wave {
    vec3 direction;
    float _wpad;   // std430 packs the next float into a vec3's tail; the CPU
                   // WaveData has an explicit pad here, so mirror it
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

// Full CPU WaterData layout (graphics_effects.hpp) — keep in exact sync.
layout(std430, set = 1, binding = 0) readonly buffer WaterBuf {
    mat4 modelMatrix;
    vec4 surfaceColor;
    vec4 refractionColor;
    vec4 ssrSettings;
    vec4 normalMapScroll;
    vec2 normalMapScrollSpeed;
    vec2 _pad1;
    float refractionDistortionFactor;
    float refractionHeightFactor;
    float refractionDistanceFactor;
    float depthSofteningDistance;
    float foamHeightStart;
    float foamFadeDistance;
    float foamTiling;
    float foamAngleExponent;
    float roughness;
    float reflectance;
    float specIntensity;
    float foamBrightness;
    Wave waves[4];
    uint waveCount;
    float dampeningFactor;
    float time;
    float _pad2;
    vec4 sunDirection;       // xyz = world dir FROM the sun
    vec4 sunColorIntensity;  // rgb + w = intensity
    vec4 causticsParams;     // x=intensity y=world scale z=speed w=water level Y
    vec4 causticsBoundsMin;  // xyz = water AABB min, w = fade depth
    vec4 causticsBoundsMax;  // xyz = water AABB max, w = env reflection intensity
} water;

layout(std430, set = 1, binding = 1) readonly buffer CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float near;
    float far;
    vec3 cameraPosition;
    float _camPad;
};

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;
layout(set = 2, binding = 2) uniform sampler2D sceneNormal;  // world-space

// Iterated-lattice caustic intensity at a 2D point (the standard water-caustic
// domain-warp: each iteration folds the coordinate through sin/cos of itself).
float causticPattern(vec2 p, float t) {
    vec2 i = p;
    float c = 1.0;
    const float inten = 0.005;
    for (int n = 0; n < 4; n++) {
        float phase = t * (1.0 - (3.5 / float(n + 1)));
        i = p + vec2(cos(phase - i.x) + sin(phase + i.y),
                     sin(phase - i.y) + cos(phase + i.x));
        vec2 denom = vec2(sin(i.x + phase) / inten, cos(i.y + phase) / inten);
        c += 1.0 / max(length(vec2(p.x / denom.x, p.y / denom.y)), 1e-5);
    }
    c /= 4.0;
    c = 1.17 - pow(c, 1.4);
    return pow(abs(c), 8.0);
}

void main() {
    vec4 scene = texture(sceneColor, tex_uv);
    float depth = texture(sceneDepth, tex_uv).r;
    if (depth >= 0.9999 || water.causticsParams.x <= 0.0) {
        outColor = scene;
        return;
    }

    // Reconstruct world position (engine convention: y-down UV, [0,1] depth).
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 viewPos = invProj * vec4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    vec3 worldPos = (invView * vec4(viewPos.xyz, 1.0)).xyz;

    float waterLevel = water.causticsParams.w;
    vec3 bmin = water.causticsBoundsMin.xyz;
    vec3 bmax = water.causticsBoundsMax.xyz;

    // Soft mask: inside the water AABB, below the waterline.
    const float edge = 0.15;
    float mask = smoothstep(0.0, edge, worldPos.x - bmin.x)
               * smoothstep(0.0, edge, bmax.x - worldPos.x)
               * smoothstep(0.0, edge, worldPos.z - bmin.z)
               * smoothstep(0.0, edge, bmax.z - worldPos.z)
               * smoothstep(0.0, 0.05, waterLevel - worldPos.y)
               * smoothstep(0.0, edge, worldPos.y - bmin.y + edge);
    if (mask <= 0.001) {
        outColor = scene;
        return;
    }

    // Caustics concentrate on sun-facing surfaces but never vanish entirely —
    // the water surface scatters skylight downward too.
    vec3 n = normalize(texture(sceneNormal, tex_uv).xyz);
    float nDotL = clamp(dot(n, -normalize(water.sunDirection.xyz)), 0.0, 1.0);
    float facing = 0.25 + 0.75 * nDotL;

    // Dim with depth below the waterline (fade depth in causticsBoundsMin.w).
    float submersion = waterLevel - worldPos.y;
    float fadeDepth = max(water.causticsBoundsMin.w, 0.01);
    float depthFade = pow(clamp(1.0 - submersion / fadeDepth, 0.0, 1.0), 1.5);

    vec2 p = worldPos.xz * water.causticsParams.y;
    float t = water.time * water.causticsParams.z;

    // Slight per-channel domain offset fakes dispersion fringes.
    float cG = causticPattern(p, t);
    float cR = causticPattern(p + vec2(0.008, 0.004), t);
    float cB = causticPattern(p - vec2(0.006, 0.008), t);
    vec3 caustic = vec3(cR, cG, cB);

    // Sun-tinted multiplicative boost: scales with the scene color, so albedo
    // and existing shading keep modulating the pattern instead of washing out.
    vec3 sunTint = normalize(water.sunColorIntensity.rgb + vec3(0.001)) * 1.732;
    vec3 boost = caustic * sunTint * (water.causticsParams.x * mask * facing * depthFade);

    outColor = vec4(scene.rgb * (vec3(1.0) + boost), scene.a);
}
