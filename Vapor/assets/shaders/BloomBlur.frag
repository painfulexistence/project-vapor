#version 450
// Bloom blur: 9x9 gaussian at half resolution. Single 2D pass (kept simple;
// a separable H/V version can replace this later for efficiency).

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D texBloom;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(texBloom, 0));
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -4; y <= 4; ++y) {
        for (int x = -4; x <= 4; ++x) {
            float w = exp(-float(x * x + y * y) / 8.0);
            sum += texture(texBloom, tex_uv + vec2(x, y) * texel).rgb * w;
            wsum += w;
        }
    }
    outColor = vec4(sum / wsum, 1.0);
}
