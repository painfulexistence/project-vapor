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

// SSBO (not UBO): the RHI set-0 buffer set is all storage buffers. Binds the
// engine CameraRenderData; proj/view offsets are identical under std430.
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

layout(std430, set = 1, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

// Scene depth texture for ground clamping (bound at set 2)
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;

// Per-emitter parameters (32 bytes total for alignment)
layout(push_constant) uniform PushConstants {
    float particleSize;
    float useTexture;         // > 0.5: sample particleTexture; else procedural disc
    float depthFadeEnabled;   // > 0.5: apply depth fade in fragment
    float depthFadeDistance;
    float groundClampEnabled; // > 0.5: clamp to depth surface
    float groundClampOffset;  // height above surface
    float _pad0;
    float _pad1;
} pushConstants;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragDepth;      // linear view-space depth for depth fade
layout(location = 3) out vec2 fragScreenUV;    // screen UV for depth texture sampling

float linearizeDepth(float d) {
    return near * far / (far - d * (far - near));
}

vec3 reconstructWorldPosition(vec2 screenUV, float depth) {
    vec2 ndc = screenUV * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = invProj * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos = invView * viewPos;
    return worldPos.xyz;
}

void main() {
    uint particleIndex = gl_InstanceIndex;
    uint vertexIndex = gl_VertexIndex;

    Particle p = particles[particleIndex];
    vec3 worldPos = p.position;

    // Ground clamping: project particle to screen, sample depth, clamp to surface
    if (pushConstants.groundClampEnabled > 0.5) {
        vec4 clipPos = proj * view * vec4(worldPos, 1.0);
        if (clipPos.w > 0.0) {
            vec2 ndc = clipPos.xy / clipPos.w;
            vec2 screenUV = ndc * 0.5 + 0.5;

            // Only clamp if particle is on screen
            if (screenUV.x >= 0.0 && screenUV.x <= 1.0 &&
                screenUV.y >= 0.0 && screenUV.y <= 1.0) {
                float sceneDepthSample = texture(sceneDepth, screenUV).r;
                vec3 surfacePos = reconstructWorldPosition(screenUV, sceneDepthSample);

                // Clamp particle Y to surface + offset
                if (worldPos.y < surfacePos.y + pushConstants.groundClampOffset) {
                    worldPos.y = surfacePos.y + pushConstants.groundClampOffset;
                }
            }
        }
    }

    // Billboard: get camera right and up vectors from view matrix
    vec3 cameraRight = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 cameraUp = vec3(view[0][1], view[1][1], view[2][1]);

    // Calculate billboard vertex position
    vec2 quadPos = quadVertices[vertexIndex];
    float size = pushConstants.particleSize;
    vec3 billboardPos = worldPos
                      + cameraRight * quadPos.x * size
                      + cameraUp * quadPos.y * size;

    vec4 viewPos = view * vec4(billboardPos, 1.0);
    gl_Position = proj * viewPos;

    fragUV = quadUVs[vertexIndex];
    fragColor = p.color;

    // Pass linear depth (negated because view space Z is negative forward)
    fragDepth = -viewPos.z;

    // Compute screen UV for depth sampling (convert from clip to [0,1] range)
    vec2 ndc = gl_Position.xy / gl_Position.w;
    fragScreenUV = ndc * 0.5 + 0.5;
}
