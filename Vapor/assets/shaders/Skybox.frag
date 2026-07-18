#version 450
// Visible sky sampled from the captured environment cubemap (SkyType::HDRI).
// Shares Sky.vert (fullscreen triangle at z = 1.0) with the atmosphere pass and
// is depth-tested (LessOrEqual) so it only fills background pixels. Reconstructs
// the world-space view direction exactly like Atmosphere.frag, then samples the
// environment cubemap that the IBL capture produced (HDRI equirect -> cubemap,
// or the sky capture).

layout(location = 0) in vec2 ndcOut;   // clip-space XY of this pixel
layout(location = 0) out vec4 outColor;

// Must match Vapor::CameraRenderData (same layout as Atmosphere.frag's CameraData).
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
layout(set = 2, binding = 0) uniform samplerCube envCubemap;

void main() {
    // Reconstruct the world-space view ray for this pixel (same as Atmosphere.frag).
    vec4 clip = vec4(ndcOut, 1.0, 1.0);
    vec4 viewPos = cam.invProj * clip;
    viewPos /= viewPos.w;
    vec3 dir = normalize((cam.invView * vec4(viewPos.xyz, 0.0)).xyz);

    outColor = vec4(texture(envCubemap, dir).rgb, 1.0);
}
