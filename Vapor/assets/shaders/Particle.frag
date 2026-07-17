#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

// Per-emitter sprite texture, bound per draw packet. When useTexture == 0 the
// procedural soft-disc path is used and this stays the default white texture.
layout(set = 2, binding = 0) uniform sampler2D particleTexture;

// Same 16-byte block the vertex stage reads (offset 0 of the push range).
layout(push_constant) uniform PushConstants {
    float particleSize;
    float useTexture;   // > 0.5: sample particleTexture; else procedural disc
    float _pad2;
    float _pad3;
} pc;

void main() {
    if (pc.useTexture > 0.5) {
        // Textured sprite, modulated by per-particle color (carries the age fade).
        outColor = texture(particleTexture, fragUV) * fragColor;
    } else {
        // Procedural: soft circular falloff + additive glow core.
        vec2 center = fragUV - vec2(0.5);
        float dist = length(center) * 2.0;

        float alpha = 1.0 - smoothstep(0.0, 1.0, dist);
        float glow = exp(-dist * dist * 2.0);

        outColor = vec4(fragColor.rgb * (alpha + glow * 0.5), fragColor.a * alpha);
    }

    // Discard fully transparent pixels
    if (outColor.a < 0.01) {
        discard;
    }
}
