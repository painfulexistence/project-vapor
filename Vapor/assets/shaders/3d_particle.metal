#include "assets/shaders/3d_common.metal"

// GPU Particle structure for compute shader simulation
struct Particle {
    float3 position;
    float3 velocity;
    float3 force;
    float4 color;
};

// Particle simulation parameters
struct ParticleSimParams {
    float2 resolution;
    float2 mousePosition;
    float time;
    float deltaTime;
    uint particleCount;
};

// Attractor data
struct ParticleAttractor {
    float3 position;
    float strength;
};

// ============================================================================
// Compute Kernels
// ============================================================================

kernel void particleForce(
    device Particle* particles [[buffer(0)]],
    constant ParticleSimParams& params [[buffer(1)]],
    constant ParticleAttractor& attractor [[buffer(2)]],
    uint id [[thread_position_in_grid]]
) {
    // Bounds check to avoid out-of-bounds access
    if (id >= params.particleCount) {
        return;
    }

    Particle p = particles[id];

    // Reset force
    p.force = float3(0.0);

    // Attractor force (orbital motion)
    float3 toAttractor = attractor.position - p.position;
    float dist = length(toAttractor);
    if (dist > 0.001) {
        float3 dir = toAttractor / dist;
        // Inverse square law with softening for smooth orbital motion
        float forceMag = attractor.strength / (dist * dist + 0.5);
        p.force += dir * forceMag;
    }

    // Light damping to prevent excessive spiraling (reduced from 0.5 to 0.1)
    p.force -= p.velocity * 0.1;

    particles[id] = p;
}

kernel void particleIntegrate(
    device Particle* particles [[buffer(0)]],
    constant ParticleSimParams& params [[buffer(1)]],
    uint id [[thread_position_in_grid]]
) {
    // Bounds check to avoid out-of-bounds access
    if (id >= params.particleCount) {
        return;
    }

    Particle p = particles[id];

    // Semi-implicit Euler integration
    p.velocity += p.force * params.deltaTime;
    p.position += p.velocity * params.deltaTime;

    // Boundary conditions (keep particles in a sphere)
    float maxDist = 20.0;
    float dist = length(p.position);
    if (dist > maxDist) {
        p.position = normalize(p.position) * maxDist;
        // Reflect velocity
        float3 normal = normalize(p.position);
        p.velocity = reflect(p.velocity, normal) * 0.5;
    }

    particles[id] = p;
}

// ============================================================================
// Render Shaders
// ============================================================================

// Quad vertices (2 triangles forming a quad)
constant float2 quadVertices[6] = {
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0,  1.0)
};

constant float2 quadUVs[6] = {
    float2(0.0, 0.0),
    float2(1.0, 0.0),
    float2(1.0, 1.0),
    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(0.0, 1.0)
};

struct ParticleVertexOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};

struct ParticlePushConstants {
    float particleSize;
    float _pad1;
    float _pad2;
    float _pad3;
};

vertex ParticleVertexOut particleVertex(
    uint vertexID [[vertex_id]],
    uint instanceID [[instance_id]],
    constant CameraData& camera [[buffer(0)]],
    constant ParticlePushConstants& pushConstants [[buffer(1)]],
    device const Particle* particles [[buffer(2)]]
) {
    Particle p = particles[instanceID];

    // Billboard: get camera right and up vectors from view matrix
    float3 cameraRight = float3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    float3 cameraUp = float3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);

    // Calculate billboard vertex position
    float2 quadPos = quadVertices[vertexID];
    float size = pushConstants.particleSize;
    float3 worldPos = p.position
                    + cameraRight * quadPos.x * size
                    + cameraUp * quadPos.y * size;

    ParticleVertexOut out;
    out.position = camera.proj * camera.view * float4(worldPos, 1.0);
    out.uv = quadUVs[vertexID];
    out.color = p.color;

    return out;
}

fragment float4 particleFragment(ParticleVertexOut in [[stage_in]]) {
    // Circular particle with soft edges
    float2 center = in.uv - float2(0.5);
    float dist = length(center) * 2.0;

    // Soft circle falloff
    float alpha = 1.0 - smoothstep(0.0, 1.0, dist);

    // Additional glow effect
    float glow = exp(-dist * dist * 2.0);

    float4 outColor = float4(in.color.rgb * (alpha + glow * 0.5), in.color.a * alpha);

    // Discard fully transparent pixels
    if (outColor.a < 0.01) {
        discard_fragment();
    }

    return outColor;
}
