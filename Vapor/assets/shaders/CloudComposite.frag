#version 450
// Volumetric clouds — depth-aware upscale + composite (twin of Metal
// cloudUpscaleComposite). Bilaterally upsamples the quarter-res resolved
// clouds — the four nearest coarse texels weighted by bilinear weight x depth
// similarity — so cloud values don't bleed across geometry edges (the halo
// that plain bilinear left around eaves/columns against the sky). Then
// composites over the scene: result = scene * cloudTransmittance + scattering.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D cloudTexture;
layout(set = 2, binding = 2) uniform sampler2D sceneDepth;

// Must match Vapor::CameraRenderData.
struct CameraData {
    mat4 proj; mat4 view; mat4 invProj; mat4 invView;
    float nearPlane; float farPlane; vec2 _pad;
    vec3 position; float _pad2;
    vec4 frustumPlanes[6];
};
layout(std430, set = 1, binding = 3) readonly buffer CameraBuf { CameraData cam; };

float linearizeDepth(float d) {
    return cam.nearPlane * cam.farPlane /
           (cam.farPlane - d * (cam.farPlane - cam.nearPlane));
}

void main() {
    vec4 scene = texture(sceneColor, tex_uv);

    float dCenter = linearizeDepth(texture(sceneDepth, tex_uv).r);
    vec2 cloudSize = vec2(textureSize(cloudTexture, 0));
    vec2 coord = tex_uv * cloudSize - 0.5;
    vec2 base = floor(coord);
    vec2 f = coord - base;

    vec4 cloudSum = vec4(0.0);
    float wSum = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 off = vec2(float(i & 1), float(i >> 1));
        // Coarse texel center → linear filter returns the point value.
        vec2 uvTap = (base + off + 0.5) / cloudSize;
        float wBilin = mix(1.0 - f.x, f.x, off.x) * mix(1.0 - f.y, f.y, off.y);
        float dTap = linearizeDepth(texture(sceneDepth, uvTap).r);
        // ~10%-of-depth tolerance: taps behind a different surface get ~zero
        // weight, keeping foreground silhouettes crisp against distant clouds.
        float wDepth = exp(-abs(dTap - dCenter) / (0.1 * dCenter + 0.5));
        float w = wBilin * wDepth;
        cloudSum += texture(cloudTexture, uvTap) * w;
        wSum += w;
    }
    // Every tap rejected (deep depth discontinuity on all sides): fall back to
    // plain bilinear rather than dividing by ~zero.
    vec4 cloud = wSum > 1e-4 ? cloudSum / wSum : texture(cloudTexture, tex_uv);

    outColor = vec4(scene.rgb * cloud.a + cloud.rgb, scene.a);
}
