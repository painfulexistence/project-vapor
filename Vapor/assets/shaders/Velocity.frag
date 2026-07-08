#version 450
// Camera-motion velocity from the depth buffer (motion vectors for a future
// TAA / motion-blur consumer). GLSL port of 3d_velocity.metal, as a fullscreen
// fragment pass (the RHI compute path can't sample a depth texture).
// velocity = (currNDC.xy - prevNDC.xy) * 0.5 in y-up NDC UV-scale units;
// a consumer reprojects with prevUV = yUpUV - velocity.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outVelocity;

layout(set = 2, binding = 0) uniform sampler2D depthTexture;

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
layout(std430, set = 1, binding = 0) readonly buffer PrevBuf { mat4 prevViewProj; };

void main() {
    float depth = texture(depthTexture, tex_uv).r;

    // Current NDC (y-up), reconstruct world position.
    vec2 ndcxy = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 ndc = vec4(ndcxy, depth, 1.0);
    vec4 viewPos = cam.invProj * ndc;
    viewPos /= viewPos.w;
    vec3 worldPos = (cam.invView * viewPos).xyz;

    vec2 velocity = vec2(0.0);
    vec4 prevClip = prevViewProj * vec4(worldPos, 1.0);
    if (prevClip.w > 0.0) {
        vec2 prevNDC = prevClip.xy / prevClip.w;
        velocity = (ndc.xy - prevNDC) * 0.5;
    }
    outVelocity = vec4(velocity, 0.0, 0.0);
}
