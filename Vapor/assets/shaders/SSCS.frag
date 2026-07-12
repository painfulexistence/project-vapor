#version 450
// Screen-Space Contact Shadows — GLSL twin of 3d_sscs.metal, as a fullscreen
// fragment pass (the RHI compute path cannot sample a depth texture on Vulkan;
// same pattern as SSAO.frag / Velocity.frag). Provides the near-field contact
// shadow the coarse PSSM cascades miss, WITHOUT ray tracing, so the Vulkan
// backend reaches parity with the Metal RT near-field shadow.
//
// Reconstruction follows the proven Vulkan convention (SSAO.frag / Velocity.frag):
// y-up NDC = (u*2-1, 1-v*2), ZO depth. Output: R (0 = shadowed, 1 = lit).

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outShadow;

layout(set = 2, binding = 0) uniform sampler2D depthTexture;   // full-res scene depth

struct CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float nearPlane;
    float farPlane;
    vec2 _pad;
    vec3 position;
    float _pad2;
    vec4 frustumPlanes[6];
};
layout(std430, set = 1, binding = 3) readonly buffer CameraBuf { CameraData cam; };

// Params via setFragmentBytes(binding=0) -> push offset 64. lightDirVS is the
// direction TOWARD the light, already transformed into view space on the CPU.
layout(push_constant) uniform SSCSPC {
    layout(offset = 64) vec4 lightDirVS;   // xyz = view-space dir to light
    layout(offset = 80) float rayLength;   // view-space marching distance
    layout(offset = 84) float thickness;   // occluder depth window
    layout(offset = 88) uint  stepCount;
    layout(offset = 92) float bias;
};

// Linear view-space z from a ZO depth sample. z is xy-independent for a
// perspective projection, so (0,0,depth,1) suffices (same trick as SSAO.frag).
float viewZFromDepth(float depth) {
    vec4 v = cam.invProj * vec4(0.0, 0.0, depth, 1.0);
    return v.z / v.w;
}

// Full view-space position (needed for the ray origin).
vec3 viewPosFromDepth(vec2 uv, float depth) {
    vec2 ndcxy = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec4 v = cam.invProj * vec4(ndcxy, depth, 1.0);
    return v.xyz / v.w;
}

void main() {
    float depth = texture(depthTexture, tex_uv).r;
    if (depth >= 0.999999) { outShadow = vec4(1.0); return; } // sky: lit

    vec3 originVS = viewPosFromDepth(tex_uv, depth);
    vec3 L = normalize(lightDirVS.xyz);

    float stepLen = rayLength / float(max(stepCount, 1u));
    vec3 rayPos = originVS + L * bias;

    float occlusion = 0.0;
    for (uint i = 0u; i < stepCount; ++i) {
        rayPos += L * stepLen;

        vec4 clip = cam.proj * vec4(rayPos, 1.0);
        if (clip.w <= 0.0) break;
        vec3 ndc = clip.xyz / clip.w;
        vec2 uvYUp = ndc.xy * 0.5 + 0.5;
        if (any(notEqual(uvYUp, clamp(uvYUp, 0.0, 1.0)))) break; // off-screen
        vec2 sampleUV = vec2(uvYUp.x, 1.0 - uvYUp.y);

        float sceneDepth = texture(depthTexture, sampleUV).r;
        float sceneZ = viewZFromDepth(sceneDepth);

        // RH view space looks down -z: the scene surface occludes the ray when
        // it sits in front of (greater z than) the ray sample, but only within a
        // thickness window so distant background is not treated as a contact.
        float diff = sceneZ - rayPos.z;
        if (diff > 0.0 && diff < thickness) {
            occlusion = 1.0 - float(i) / float(stepCount); // fade toward ray end
            break;
        }
    }

    outShadow = vec4(1.0 - occlusion); // visibility: 1 lit, 0 shadowed
}
