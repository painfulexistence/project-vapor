#include <metal_stdlib>
using namespace metal;

// Grass ring (Metal-via-RHI) — MSL twin of Grass.vert / Grass.frag, kept in
// lockstep line-by-line. One blade = 15 vertices (two tapered quads + tip),
// generated procedurally from vertex_id; instances pull GrassBladeGpu from the
// pool at instance_id (Metal's instance_id includes the draw's baseInstance,
// matching Vulkan's gl_InstanceIndex).
//
// Binding contract (mirrors the GLSL):
//   vertex   buffer(0) GrassParams   buffer(1) GrassBladeGpu blades[]
//   fragment buffer(0) GrassParams

struct GrassBladeGpu {
    float4 positionAndHeight;  // xyz world base, w height (m)
    float4 params;             // x sway phase, y facing angle, z tint jitter, w half width (m)
};

struct GrassParams {
    float4x4 viewProj;
    float4 cameraPosTime;  // xyz camera, w time (s)
    float4 wind;           // xy dir, z strength (m), w speed
    float4 rootColor;      // rgb root, w fadeStart (m)
    float4 tipColor;       // rgb tip, w fadeEnd (m)
    float4 sun;            // xyz TOWARD sun, w intensity
    float4 sunColor;       // rgb
};

struct GrassVSOut {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float heightFrac;
    float tint;
};

// Blade profile: segment heights and half-width taper; 15 indices walk the two
// quads (0..5, 6..11) and the tip triangle (12..14). Program scope (constant
// address space arrays must not be function-local).
constant float kGrassSegH[4] = { 0.0, 0.5, 0.85, 1.0 };
constant float kGrassSegW[4] = { 1.0, 0.7, 0.35, 0.0 };
constant int kGrassSeg[15]  = { 0, 0, 1,  0, 1, 1,  1, 1, 2,  1, 2, 2,  2, 2, 3 };
constant int kGrassSide[15] = { 0, 1, 0,  1, 1, 0,  0, 1, 0,  1, 1, 0,  0, 1, 0 };

vertex GrassVSOut grassVertex(uint vid [[vertex_id]],
                              uint iid [[instance_id]],
                              constant GrassParams& p [[buffer(0)]],
                              const device GrassBladeGpu* blades [[buffer(1)]]) {
    GrassBladeGpu b = blades[iid];
    const float3 base = b.positionAndHeight.xyz;
    float height = b.positionAndHeight.w;

    // Distance fade: shrink to nothing well before the ring edge streams out.
    const float dist = distance(base.xz, p.cameraPosTime.xz);
    height *= 1.0 - smoothstep(p.rootColor.w, p.tipColor.w, dist);

    const int seg = kGrassSeg[vid];
    const float hf = kGrassSegH[seg];
    const float side = (kGrassSide[vid] == 0) ? -1.0 : 1.0;

    const float ca = cos(b.params.y), sa = sin(b.params.y);
    const float3 right = float3(ca, 0.0, sa);

    const float t = p.cameraPosTime.w;
    const float ph = b.params.x + dot(base.xz, float2(0.15, 0.11));
    const float sway = p.wind.z * (0.65 + 0.35 * sin(p.wind.w * t + ph))
        * sin(p.wind.w * 0.31 * t + ph * 1.7);
    const float3 windOfs = float3(p.wind.x, 0.0, p.wind.y) * (sway * hf * hf);

    float3 pos = base + right * (side * kGrassSegW[seg] * b.params.w)
        + float3(0.0, hf * height, 0.0) + windOfs;

    const float3 faceN = float3(-sa, 0.0, ca);
    GrassVSOut out;
    out.worldPos = pos;
    out.normal = normalize(mix(float3(0.0, 1.0, 0.0), faceN, 0.35));
    out.heightFrac = hf;
    out.tint = b.params.z;
    out.position = p.viewProj * float4(pos, 1.0);
    return out;
}

fragment float4 grassFragment(GrassVSOut in [[stage_in]],
                              constant GrassParams& p [[buffer(0)]]) {
    // Root->tip gradient with per-blade jitter; display-space colors are
    // linearized (pow 2.2, same convention as the terrain layers).
    float3 c = mix(pow(p.rootColor.rgb, float3(2.2)), pow(p.tipColor.rgb, float3(2.2)),
                   smoothstep(0.0, 1.0, in.heightFrac));
    c *= 0.85 + 0.3 * in.tint;

    // Soft wrap diffuse with a bounded stylized sun scale (see Grass.frag).
    float ndl = max(dot(normalize(in.normal), normalize(p.sun.xyz)), 0.0);
    float wrap = 0.35 + 0.65 * ndl;
    float sunScale = clamp(p.sun.w * 0.12, 0.15, 1.2);
    float3 lit = c * p.sunColor.rgb * (wrap * sunScale);

    // Root ambient occlusion: blades shadow their own bases.
    lit *= 0.55 + 0.45 * in.heightFrac;

    return float4(lit, 1.0);
}
