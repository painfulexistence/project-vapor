#version 450
// Volumetric clouds — upscale + composite (port of Metal cloudUpscaleComposite).
// Bilinearly upsamples the quarter-res resolved clouds and composites over the
// scene: result = scene * cloudTransmittance + cloudScattering.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D cloudTexture;
layout(set = 2, binding = 2) uniform sampler2D sceneDepth;  // reserved for depth-aware upsample

void main() {
    vec4 scene = texture(sceneColor, tex_uv);
    vec4 cloud = texture(cloudTexture, tex_uv);
    outColor = vec4(scene.rgb * cloud.a + cloud.rgb, scene.a);
}
