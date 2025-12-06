#version 450

struct EmitterParams {
    vec3 position;
    float _pad1;
    vec3 direction;
    float emitAngle;
    vec4 startColor;
    vec4 endColor;
    vec3 gravity;
    float damping;
    vec3 attractorPosition;
    float attractorStrength;
    float emitSpeed;
    float lifetime;
    float particleSize;
    uint maxParticles;
    uint startIndex;
    uint usePalette;
    // Depth effects
    uint depthFadeEnabled;
    float depthFadeDistance;
    uint groundClampEnabled;
    float groundOffset;
    float groundFriction;
    float _pad2;
    // Palette
    vec3 paletteA;
    float _pad3;
    vec3 paletteB;
    float _pad4;
    vec3 paletteC;
    float _pad5;
    vec3 paletteD;
    float _pad6;
};

layout(std140, set = 0, binding = 0) uniform CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float near;
    float far;
    vec3 cameraPosition;
    float _pad1;
};

layout(std430, set = 2, binding = 0) readonly buffer EmitterBuffer {
    EmitterParams emitters[];
};

layout(set = 3, binding = 0) uniform sampler2D depthTexture;

layout(push_constant) uniform PushConstants {
    float particleSize;
    float globalTime;
    uint emitterCount;
    uint _pad1;
} pushConstants;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec4 fragClipPos;
layout(location = 3) flat in uint fragEmitterID;

layout(location = 0) out vec4 outColor;

// Linearize depth from [0,1] to view-space distance
float linearizeDepth(float depth) {
    return near * far / (far - depth * (far - near));
}

void main() {
    // Circular particle with soft edges
    vec2 center = fragUV - vec2(0.5);
    float dist = length(center) * 2.0;

    // Soft circle falloff
    float alpha = 1.0 - smoothstep(0.0, 1.0, dist);

    // Additional glow effect
    float glow = exp(-dist * dist * 2.0);

    float finalAlpha = fragColor.a * alpha;

    // Depth fade effect
    if (fragEmitterID < pushConstants.emitterCount) {
        EmitterParams emitter = emitters[fragEmitterID];

        if (emitter.depthFadeEnabled != 0) {
            // Convert clip pos to screen UV
            vec2 screenUV = (fragClipPos.xy / fragClipPos.w) * 0.5 + 0.5;

            // Sample scene depth
            float sceneDepth = texture(depthTexture, screenUV).r;

            // Get particle depth (in NDC)
            float particleDepth = fragClipPos.z / fragClipPos.w;

            // Convert to linear depth for proper distance calculation
            float sceneLinearDepth = linearizeDepth(sceneDepth);
            float particleLinearDepth = linearizeDepth(particleDepth);

            // Calculate depth difference (positive = particle is in front)
            float depthDiff = sceneLinearDepth - particleLinearDepth;

            // Fade based on distance to scene geometry
            float depthFade = smoothstep(0.0, emitter.depthFadeDistance, depthDiff);

            finalAlpha *= depthFade;
        }
    }

    outColor = vec4(fragColor.rgb * (alpha + glow * 0.5), finalAlpha);

    // Discard fully transparent pixels
    if (outColor.a < 0.01) {
        discard;
    }
}
