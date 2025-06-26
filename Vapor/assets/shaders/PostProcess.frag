#version 450

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 Color;

layout(set = 2, binding = 0) uniform sampler2D texScreen;

const float GAMMA = 2.2;
const float INV_GAMMA = 1.0 / GAMMA;


vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 linearToSRGB(vec3 linear) {
    // return pow(linear, vec3(INV_GAMMA));
    return mix(
        linear * 12.92,
        1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
        step(0.0031308, linear)
    );
}

void main() {
    vec3 color = texture(texScreen, tex_uv).rgb;

    color = aces(color);

    // color = linearToSRGB(color); // Already handled by swapchain

    Color = vec4(color, 1.0);
}
