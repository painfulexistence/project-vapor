#version 450
// Cheap gradient sky (SkyType::Gradient): a zenith/horizon/ground vertical
// gradient driven by the world-space view direction. Shares Sky.vert (fullscreen
// triangle at z = 1.0) and is depth-tested (LessOrEqual) so it only fills
// background pixels. Reconstructs the world view ray exactly like Skybox.frag /
// Atmosphere.frag.

layout(location = 0) in vec2 ndcOut;   // clip-space XY of this pixel
layout(location = 0) out vec4 outColor;

// Zenith/horizon/ground colors. vec4-padded so the std430 layout is identical on
// every backend (matches Vapor::GradientRenderData / 3d_gradient.metal).
struct GradientData {
    vec4 zenith;
    vec4 horizon;
    vec4 ground;
};
layout(std430, set = 0, binding = 0) readonly buffer GradientBuf { GradientData grad; };

// Must match Vapor::CameraRenderData (same layout as Skybox.frag's CameraData).
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

void main() {
    // Reconstruct the world-space view ray for this pixel (same as Skybox.frag).
    vec4 clip = vec4(ndcOut, 1.0, 1.0);
    vec4 viewPos = cam.invProj * clip;
    viewPos /= viewPos.w;
    vec3 dir = normalize((cam.invView * vec4(viewPos.xyz, 0.0)).xyz);

    // Blend horizon->zenith above the horizon and horizon->ground below it. The
    // pow tightens the band near the horizon so the zenith/ground fill most of
    // the dome.
    vec3 sky;
    if (dir.y >= 0.0) {
        sky = mix(grad.horizon.rgb, grad.zenith.rgb, pow(clamp(dir.y, 0.0, 1.0), 0.5));
    } else {
        sky = mix(grad.horizon.rgb, grad.ground.rgb, pow(clamp(-dir.y, 0.0, 1.0), 0.5));
    }
    outColor = vec4(sky, 1.0);
}
