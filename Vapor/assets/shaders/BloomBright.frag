#version 450
// Bloom brightness extraction (matches Metal 3d_bloom_brightness.metal).
// Extracts pixels above a luminance threshold with a soft knee, at half res.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D texScreen;

const float threshold = 1.0;  // matches Renderer::bloomThreshold

void main() {
    vec3 color = texture(texScreen, tex_uv).rgb;

    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));

    float softThreshold = threshold * 0.8;
    float contribution = max(0.0, brightness - softThreshold) / max(brightness, 0.0001);

    if (brightness > threshold) {
        outColor = vec4(color * contribution, 1.0);
    } else {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
