#version 450
// Heterogeneous volume raymarch (EmberGen-style density grids). GLSL twin of
// 3d_volume_raymarch.metal. One axis-aligned box volume per pass: slab-test the
// view ray against the box, clamp to the scene depth, then march the density
// texture accumulating single-scattered sun light (PSSM-shadowed, with a short
// secondary march toward the sun for volume self-shadowing) plus an ambient
// floor, under Beer-Lambert transmittance and Henyey-Greenstein phase.
//
// The density grid is intended to come from an EmberGen export (import lives
// in a separate PR); until then the renderer feeds a procedural test volume.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;
layout(set = 2, binding = 2) uniform sampler2DArray pssmShadowMaps;
layout(set = 2, binding = 3) uniform sampler3D densityVolume;

// Must match Vapor::VolumeRenderData (vec4-only layout, identical in MSL).
layout(std430, set = 1, binding = 0) readonly buffer VolumeBuf {
    mat4 invViewProj;
    vec4 cameraPosition;  // xyz
    vec4 boxMin;          // xyz; w = densityScale
    vec4 boxMax;          // xyz; w = anisotropy
    vec4 albedo;          // xyz = scattering albedo; w = ambientIntensity
    vec4 sunDirection;    // xyz (normalized, pointing FROM the sun)
    vec4 sunColor;        // xyz; w = sunIntensity
    vec4 params;          // x = primary steps, y = self-shadow steps
};

// Must match Vapor::PSSMRenderData (same layout RHIMain.frag reads).
layout(std430, set = 1, binding = 1) readonly buffer PSSMBuf {
    mat4 shadowLightSpaceMatrices[3];
    vec4 cascadeSplits;
    float shadowBlendRange;
};

const float INV_4PI = 0.07957747;

float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float d = 1.0 + g2 - 2.0 * g * cosTheta;
    return INV_4PI * (1.0 - g2) / pow(d, 1.5);
}

// One-tap PSSM visibility (VolumetricFog.frag's conventions).
float sunVisibilityAt(vec3 p, float viewDepth) {
    int ci = 0;
    if      (viewDepth > cascadeSplits.z) ci = 2;
    else if (viewDepth > cascadeSplits.y) ci = 1;
    vec4 lp = shadowLightSpaceMatrices[ci] * vec4(p, 1.0);
    vec3 proj = lp.xyz / lp.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0) {
        return 1.0;
    }
    float d = texture(pssmShadowMaps, vec3(uv, float(ci))).r;
    return (proj.z - 0.002) <= d ? 1.0 : 0.0;
}

// Slab-test a ray against the volume box; returns (tEnter, tExit).
vec2 intersectBox(vec3 ro, vec3 rd) {
    vec3 inv = 1.0 / rd;  // IEEE inf on axis-parallel rays works with min/max
    vec3 t0 = (boxMin.xyz - ro) * inv;
    vec3 t1 = (boxMax.xyz - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float densityAt(vec3 p) {
    vec3 uvw = (p - boxMin.xyz) / (boxMax.xyz - boxMin.xyz);
    return texture(densityVolume, uvw).r * boxMin.w;
}

void main() {
    vec4 color = texture(sceneColor, tex_uv);
    float depth = texture(sceneDepth, tex_uv).r;

    // Reconstruct the view ray (GL-convention +Y-up NDC, Vulkan ZO depth). Sky
    // pixels (depth ~1) still intersect the volume: reconstruct at the far
    // plane and march to the box exit.
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 wp = invViewProj * vec4(ndc, min(depth, 0.9999), 1.0);
    vec3 worldPos = wp.xyz / wp.w;
    vec3 cam = cameraPosition.xyz;
    vec3 rayDir = normalize(worldPos - cam);
    float sceneDist = length(worldPos - cam);

    vec2 hit = intersectBox(cam, rayDir);
    float tEnter = max(hit.x, 0.0);
    float tExit = min(hit.y, sceneDist);
    if (tExit <= tEnter) { outColor = color; return; }

    int steps = int(params.x);
    int shadowSteps = int(params.y);
    float stepLen = (tExit - tEnter) / float(steps);
    float g = boxMax.w;
    float sunPhase = phaseHG(dot(rayDir, sunDirection.xyz), g);
    // Fixed self-shadow march length: half the box diagonal covers the volume.
    vec3 boxSize = boxMax.xyz - boxMin.xyz;
    float shadowLen = 0.5 * length(boxSize);
    float shadowStepLen = shadowLen / float(max(shadowSteps, 1));

    vec3 scatter = vec3(0.0);
    float trans = 1.0;
    for (int st = 0; st < steps; ++st) {
        float t = tEnter + (float(st) + 0.5) * stepLen;
        vec3 p = cam + rayDir * t;
        float dens = densityAt(p);
        if (dens < 1e-4) continue;

        // Self-shadowing: short march toward the sun through the grid.
        float sunOD = 0.0;
        for (int ss = 0; ss < shadowSteps; ++ss) {
            vec3 sp = p + sunDirection.xyz * ((float(ss) + 0.5) * shadowStepLen);
            sunOD += densityAt(sp);
        }
        float sunTrans = exp(-sunOD * shadowStepLen);

        vec3 L = sunColor.xyz * sunColor.w * sunPhase * sunTrans * sunVisibilityAt(p, t);
        L += vec3(0.5, 0.6, 0.7) * albedo.w;  // ambient floor

        scatter += trans * albedo.xyz * L * dens * stepLen;
        trans *= exp(-dens * stepLen);
        if (trans < 0.005) break;
    }

    outColor = vec4(color.rgb * trans + scatter, color.a);
}
