#version 450

// Quad vertices (2 triangles forming a quad)
const vec2 quadVertices[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);

const vec2 quadUVs[6] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(1.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(0.0, 1.0)
);

struct Particle {
    vec3 position;
    float _pad1;
    vec3 velocity;
    float _pad2;
    vec3 force;
    float _pad3;
    vec4 color;
    // Multi-emitter support
    float life;
    float age;
    float maxLife;
    uint emitterID;
};

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

layout(std430, set = 1, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

layout(std430, set = 2, binding = 0) readonly buffer EmitterBuffer {
    EmitterParams emitters[];
};

layout(push_constant) uniform PushConstants {
    float particleSize;
    float globalTime;
    uint emitterCount;
    uint _pad1;
} pushConstants;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec4 fragClipPos;
layout(location = 3) flat out uint fragEmitterID;

void main() {
    uint particleIndex = gl_InstanceIndex;
    uint vertexIndex = gl_VertexIndex;

    Particle p = particles[particleIndex];

    // Get per-emitter particle size if available
    float size = pushConstants.particleSize;
    if (p.emitterID < pushConstants.emitterCount) {
        size = emitters[p.emitterID].particleSize;
    }

    // Billboard: get camera right and up vectors from view matrix
    vec3 cameraRight = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 cameraUp = vec3(view[0][1], view[1][1], view[2][1]);

    // Calculate billboard vertex position
    vec2 quadPos = quadVertices[vertexIndex];
    vec3 worldPos = p.position
                  + cameraRight * quadPos.x * size
                  + cameraUp * quadPos.y * size;

    vec4 clipPos = proj * view * vec4(worldPos, 1.0);
    gl_Position = clipPos;

    fragUV = quadUVs[vertexIndex];
    fragColor = p.color;
    fragClipPos = clipPos;
    fragEmitterID = p.emitterID;
}
