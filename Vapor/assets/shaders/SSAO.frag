#version 450
// Screen-space AO — GLSL twin of 3d_ssao.metal, as a fullscreen fragment pass
// (the RHI compute path cannot sample a depth texture on Vulkan; same pattern
// as Velocity.frag). Writes noisy single-channel AO into the half-res aoRawRT;
// the temporal + à-trous passes produce the aoRT the lighting consumes.
//
// Reconstruction follows the proven Vulkan convention from VolumetricFog.frag /
// Velocity.frag: y-up NDC = (u*2-1, 1-v*2), ZO depth.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outAO;

layout(set = 2, binding = 0) uniform sampler2D depthTexture;   // full-res
layout(set = 2, binding = 1) uniform sampler2D normalTexture;  // full-res

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

// Renderer::frameCounter via setFragmentBytes(binding=0) -> offset 64
layout(push_constant) uniform SSAOPC {
    layout(offset = 64) uint frameNumber;
};

const uint  NUM_SAMPLES = 32u;
const float SSAO_RADIUS = 0.3;   // meters (matches 3d_ssao.metal)
const float SSAO_BIAS   = 0.02;  // view-space depth bias
const float PI = 3.14159265359;

// PCG-ish hash, transliterated from 3d_common.metal random()
vec2 rand2(uint seed) {
    uint s = seed * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return vec2(float(w >> 22u) / 4294967295.0,
                float(w & 0x003FFFFFu) / 4194304.0);
}

vec3 sampleSphere(vec2 s) {
    float z = 2.0 * s.x - 1.0;
    float zz = sqrt(max(1.0 - z * z, 0.0));
    float theta = 2.0 * PI * s.y;
    return vec3(zz * cos(theta), zz * sin(theta), z);
}

vec3 sampleHemisphere(vec2 s, vec3 normal) {
    vec3 dir = sampleSphere(s);
    return dot(dir, normal) >= 0.0 ? dir : -dir;
}

void main() {
    float depth = texture(depthTexture, tex_uv).r;
    // Sky / far plane: unoccluded (its normal is zero — normalize would NaN)
    if (depth >= 0.999999) { outAO = vec4(1.0); return; }

    // Reconstruct world position (y-up NDC, Vulkan ZO depth)
    vec2 ndcxy = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 ndc = vec4(ndcxy, depth, 1.0);
    vec4 viewPos = cam.invProj * ndc;
    viewPos /= viewPos.w;
    vec3 worldPos = (cam.invView * viewPos).xyz;
    float centerViewZ = viewPos.z;
    vec3 worldNormal = normalize(texture(normalTexture, tex_uv).xyz);

    // Output-resolution pixel id for the noise seed (matches the Metal kernel's
    // tid-based seeding closely enough for the temporal filter to converge).
    ivec2 outDim = ivec2(textureSize(depthTexture, 0)); // seed only; exact dims irrelevant
    uvec2 tid = uvec2(gl_FragCoord.xy);
    uint w = uint(outDim.x);

    float occlusion = 0.0;
    uint count = 0u;
    for (uint i = 0u; i < NUM_SAMPLES; i++) {
        vec2 xi = rand2(tid.x + tid.y * w + i * 7919u + frameNumber * 26699u);
        vec3 dir = sampleHemisphere(xi, worldNormal);
        // Distribute samples through the hemisphere volume, denser near the surface
        float t = (float(i) + 0.5) / float(NUM_SAMPLES);
        vec3 samplePos = worldPos + worldNormal * 0.01 + dir * (SSAO_RADIUS * t * t);

        vec4 clip = cam.proj * cam.view * vec4(samplePos, 1.0);
        if (clip.w <= 0.0) continue; // behind the camera
        vec3 sampleNdc = clip.xyz / clip.w;
        vec2 sampleUVyUp = sampleNdc.xy * 0.5 + 0.5;
        if (any(notEqual(sampleUVyUp, clamp(sampleUVyUp, 0.0, 1.0)))) continue; // off-screen
        vec2 sampleTexUV = vec2(sampleUVyUp.x, 1.0 - sampleUVyUp.y);
        float sceneDepth = texture(depthTexture, sampleTexUV).r;

        // Compare linear view-space depths (NDC z is non-linear). z is
        // independent of xy for a perspective projection, so (0,0,z,1) suffices.
        vec4 sceneView = cam.invProj * vec4(0.0, 0.0, sceneDepth, 1.0);
        float sceneViewZ = sceneView.z / sceneView.w;
        vec4 sampleView = cam.invProj * vec4(0.0, 0.0, sampleNdc.z, 1.0);
        float sampleViewZ = sampleView.z / sampleView.w;

        // RH view space looks down -z: the scene surface occludes the sample
        // when it is nearer the camera (greater view z) than the sample point.
        if (sceneViewZ >= sampleViewZ + SSAO_BIAS) {
            // Range check (classic silhouette-halo fix)
            float rangeCheck = smoothstep(0.0, 1.0, SSAO_RADIUS / max(abs(centerViewZ - sceneViewZ), 1e-4));
            occlusion += rangeCheck;
        }
        count++;
    }
    float result = count > 0u ? 1.0 - occlusion / float(count) : 1.0;
    outAO = vec4(result, 0.0, 0.0, 1.0);
}
