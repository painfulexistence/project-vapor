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

layout(push_constant) uniform PushConstants {
    float particleSize;
    float _pad1;
    float _pad2;
    float _pad3;
} pushConstants;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    uint particleIndex = gl_InstanceIndex;
    uint vertexIndex = gl_VertexIndex;

    Particle p = particles[particleIndex];

    // Billboard: get camera right and up vectors from view matrix
    vec3 cameraRight = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 cameraUp = vec3(view[0][1], view[1][1], view[2][1]);

    // Calculate billboard vertex position
    vec2 quadPos = quadVertices[vertexIndex];
    float size = pushConstants.particleSize;
    vec3 worldPos = p.position
                  + cameraRight * quadPos.x * size
                  + cameraUp * quadPos.y * size;

    gl_Position = proj * view * vec4(worldPos, 1.0);

    fragUV = quadUVs[vertexIndex];
    fragColor = p.color;
}
