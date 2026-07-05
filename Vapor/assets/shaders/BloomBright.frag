#version 450
// Bloom bright-pass: extract HDR pixels above a soft luminance threshold.
// Runs at half resolution, sampling the linear-HDR colorRT.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D texScreen;

void main() {
    vec3 c = texture(texScreen, tex_uv).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    const float threshold = 1.0;
    // Soft knee: fade in contribution above the threshold instead of a hard cut.
    float contrib = max(l - threshold, 0.0) / max(l, 1e-4);
    outColor = vec4(c * contrib, 1.0);
}
