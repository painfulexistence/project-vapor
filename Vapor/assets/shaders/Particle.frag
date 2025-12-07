#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Circular particle with soft edges
    vec2 center = fragUV - vec2(0.5);
    float dist = length(center) * 2.0;

    // Soft circle falloff
    float alpha = 1.0 - smoothstep(0.0, 1.0, dist);

    // Additional glow effect
    float glow = exp(-dist * dist * 2.0);

    outColor = vec4(fragColor.rgb * (alpha + glow * 0.5), fragColor.a * alpha);

    // Discard fully transparent pixels
    if (outColor.a < 0.01) {
        discard;
    }
}
