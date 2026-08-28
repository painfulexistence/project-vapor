#version 450
// Grass ring vertex shader (Vulkan). One blade = 15 vertices (two tapered
// quads + a tip triangle), generated procedurally from gl_VertexIndex — no
// vertex buffer. Instances pull GrassBladeGpu from the pool at
// gl_InstanceIndex (the draw's firstInstance selects the cell's slot range).
//
// Wind sway (GoT-style): the tip leans along the wind by strength *
// (0.65 + 0.35 sin(...)) with a per-blade phase and a slower secondary gust,
// scaled by heightFrac^2 so roots stay planted. Distance fade shrinks blade
// height to zero across [fadeStart, fadeEnd] — geometric fade, no blending.
//
// Binding contract (mirrored in 3d_grass.metal):
//   set0 b0  GrassParams (viewProj, cameraPosTime, wind, colors, sun)
//   set0 b1  GrassBladeGpu blades[]

struct GrassBlade {
    vec4 positionAndHeight;  // xyz world base, w height (m)
    vec4 params;             // x sway phase, y facing angle, z tint jitter, w half width (m)
};

layout(std430, set = 0, binding = 0) readonly buffer ParamsBuf {
    mat4 viewProj;
    vec4 cameraPosTime;  // xyz camera, w time (s)
    vec4 wind;           // xy dir, z strength (m), w speed
    vec4 rootColor;      // rgb root, w fadeStart (m)
    vec4 tipColor;       // rgb tip, w fadeEnd (m)
    vec4 sun;            // xyz TOWARD sun, w intensity
    vec4 sunColor;       // rgb
};
layout(std430, set = 0, binding = 1) readonly buffer BladeBuf { GrassBlade blades[]; };

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vHeightFrac;
layout(location = 3) out float vTint;

// Blade profile: segment heights and half-width taper. 15 indices walk the
// two quads (0..5, 6..11) and the tip triangle (12..14).
const float kSegH[4] = float[4](0.0, 0.5, 0.85, 1.0);
const float kSegW[4] = float[4](1.0, 0.7, 0.35, 0.0);
const int kSeg[15]  = int[15](0, 0, 1,  0, 1, 1,  1, 1, 2,  1, 2, 2,  2, 2, 3);
const int kSide[15] = int[15](0, 1, 0,  1, 1, 0,  0, 1, 0,  1, 1, 0,  0, 1, 0);

void main() {
    GrassBlade b = blades[gl_InstanceIndex];
    const vec3 base = b.positionAndHeight.xyz;
    float height = b.positionAndHeight.w;

    // Distance fade: shrink to nothing well before the ring edge streams out.
    const float dist = distance(base.xz, cameraPosTime.xz);
    height *= 1.0 - smoothstep(rootColor.w, tipColor.w, dist);

    const int seg = kSeg[gl_VertexIndex];
    const float hf = kSegH[seg];
    const float side = (kSide[gl_VertexIndex] == 0) ? -1.0 : 1.0;

    // Blade frame from the per-blade facing angle.
    const float ca = cos(b.params.y), sa = sin(b.params.y);
    const vec3 right = vec3(ca, 0.0, sa);

    // Wind: primary sway + slower gust, phase-shifted per blade and drifting
    // with world position so neighbours don't sway in lockstep.
    const float t = cameraPosTime.w;
    const float ph = b.params.x + dot(base.xz, vec2(0.15, 0.11));
    const float sway = wind.z * (0.65 + 0.35 * sin(wind.w * t + ph))
        * sin(wind.w * 0.31 * t + ph * 1.7);
    const vec3 windOfs = vec3(wind.x, 0.0, wind.y) * (sway * hf * hf);

    vec3 pos = base + right * (side * kSegW[seg] * b.params.w) + vec3(0.0, hf * height, 0.0) + windOfs;

    // Lighting normal: mostly up (fields read as a lit carpet), tilted toward
    // the blade's face so individual blades still catch raking sun.
    const vec3 faceN = vec3(-sa, 0.0, ca);
    vWorldPos = pos;
    vNormal = normalize(mix(vec3(0.0, 1.0, 0.0), faceN, 0.35));
    vHeightFrac = hf;
    vTint = b.params.z;
    gl_Position = viewProj * vec4(pos, 1.0);
}
