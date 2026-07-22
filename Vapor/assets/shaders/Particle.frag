#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragDepth;      // linear view-space depth
layout(location = 3) in vec2 fragScreenUV;    // screen UV for depth sampling

layout(location = 0) out vec4 outColor;

// Per-emitter sprite texture, bound per draw packet. When useTexture == 0 the
// procedural soft-disc path is used and this stays the default white texture.
layout(set = 2, binding = 0) uniform sampler2D particleTexture;

// Scene depth texture for depth fade effect
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;

// Same 16-byte block the vertex stage reads (offset 0 of the push range).
layout(push_constant) uniform PushConstants {
    float particleSize;
    float useTexture;       // > 0.5: sample particleTexture; else procedural disc
    float depthFadeEnabled; // > 0.5: apply depth fade
    float depthFadeDistance;
} pc;

// Camera near/far for depth linearization (from set 0)
layout(std430, set = 0, binding = 0) readonly buffer CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float near;
    float far;
    vec3 cameraPosition;
    float _pad1;
};

float linearizeDepth(float d) {
    return near * far / (far - d * (far - near));
}

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

    // Depth fade: soft fade when particle is close to scene geometry
    if (pc.depthFadeEnabled > 0.5 && pc.depthFadeDistance > 0.0) {
        float sceneDepthSample = texture(sceneDepth, fragScreenUV).r;
        float sceneLinearDepth = linearizeDepth(sceneDepthSample);
        float depthDiff = sceneLinearDepth - fragDepth;

        // Fade when particle is within depthFadeDistance of scene geometry
        float depthFade = clamp(depthDiff / pc.depthFadeDistance, 0.0, 1.0);
        outColor.a *= depthFade;
    }

    // Discard fully transparent pixels
    if (outColor.a < 0.01) {
        discard;
    }
}
