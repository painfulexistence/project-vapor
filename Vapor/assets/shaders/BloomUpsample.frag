#version 450
// Bloom pyramid upsample (matches Metal 3d_bloom_upsample.metal).
// 3x3 tent filter of the lower-res level. Metal reads the current level as a
// second texture and outputs col+blend; Vulkan cannot sample an attachment it
// is writing, so this pass instead outputs only the tent-filtered contribution
// and the pipeline uses ADDITIVE blending to accumulate onto the current
// level's existing (downsampled) content — the same col+blend result.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D texInput;  // lower-res level

const float weights[9] = float[9](
    1.0, 2.0, 1.0,
    2.0, 4.0, 2.0,
    1.0, 2.0, 1.0
);

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(texInput, 0));
    vec3 col = vec3(0.0);
    int idx = 0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            col += texture(texInput, tex_uv + vec2(x, y) * texelSize).rgb * weights[idx++];
        }
    }
    col /= 16.0;
    outColor = vec4(col, 1.0);  // additive blend adds this onto the current level
}
