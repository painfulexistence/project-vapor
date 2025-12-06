#include "assets/shaders/3d_common.metal"

// GPU Particle structure for compute shader simulation
struct Particle {
    float3 position;
    float _pad1;
    float3 velocity;
    float _pad2;
    float3 force;
    float _pad3;
    float4 color;
    // Multi-emitter support
    float life;
    float age;
    float maxLife;
    uint emitterID;
};

// GPU-side emitter parameters
struct EmitterParams {
    float3 position;
    float _pad1;
    float3 direction;
    float emitAngle;
    float4 startColor;
    float4 endColor;
    float3 gravity;
    float damping;
    float3 attractorPosition;
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
    float3 paletteA;
    float _pad3;
    float3 paletteB;
    float _pad4;
    float3 paletteC;
    float _pad5;
    float3 paletteD;
    float _pad6;
};

// Particle simulation parameters
struct ParticleSimParams {
    float2 resolution;
    float2 _simPad1;
    float2 mousePosition;
    float2 _simPad2;
    float time;
    float deltaTime;
    uint emitterCount;
    float _simPad3;
};

// Attractor data (legacy, for backward compatibility)
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

// Reconstruct world position from screen UV and depth
float3 reconstructWorldPosition(float2 screenUV, float depth, constant CameraData& camera) {
    // Convert UV to NDC
    float2 ndc = screenUV * 2.0 - 1.0;

    // Reconstruct clip position
    float4 clipPos = float4(ndc, depth, 1.0);

    // Transform to view space
    float4 viewPos = camera.invProj * clipPos;
    viewPos /= viewPos.w;

    // Transform to world space
    float4 worldPos = camera.invView * viewPos;

    return worldPos.xyz;
}

kernel void particleIntegrate(
    device Particle* particles [[buffer(0)]],
    constant ParticleSimParams& params [[buffer(1)]],
    constant CameraData& camera [[buffer(2)]],
    constant EmitterParams* emitters [[buffer(3)]],
    texture2d<float, access::sample> depthTexture [[texture(0)]],
    uint id [[thread_position_in_grid]]
) {
    // Bounds check
    if (id >= 50000) {
        return;
    }

    Particle p = particles[id];

    // Get emitter params if available
    bool hasEmitter = p.emitterID < params.emitterCount;

    // Semi-implicit Euler integration
    p.velocity += p.force * params.deltaTime;
    p.position += p.velocity * params.deltaTime;

    // Ground clamping (falling leaves effect)
    if (hasEmitter) {
        EmitterParams emitter = emitters[p.emitterID];

        if (emitter.groundClampEnabled != 0) {
            // Project particle position to screen space
            float4 clipPos = camera.proj * camera.view * float4(p.position, 1.0);
            float3 ndc = clipPos.xyz / clipPos.w;
            float2 screenUV = ndc.xy * 0.5 + 0.5;

            // Check if particle is within screen bounds
            if (screenUV.x >= 0.0 && screenUV.x <= 1.0 &&
                screenUV.y >= 0.0 && screenUV.y <= 1.0 &&
                ndc.z >= 0.0 && ndc.z <= 1.0) {

                // Sample scene depth
                constexpr sampler depthSampler(filter::linear, address::clamp_to_edge);
                float sceneDepth = depthTexture.sample(depthSampler, screenUV).r;

                // Reconstruct ground position in world space
                float3 groundPos = reconstructWorldPosition(screenUV, sceneDepth, camera);

                // Check if particle is below ground level
                float groundY = groundPos.y + emitter.groundOffset;

                if (p.position.y < groundY) {
                    // Clamp to ground
                    p.position.y = groundY;

                    // Apply friction to horizontal velocity
                    p.velocity.xz *= (1.0 - emitter.groundFriction);

                    // Stop vertical movement
                    p.velocity.y = 0.0;
                }
            }
        }
    }

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
    float4 clipPos;
    uint emitterID [[flat]];
};

struct ParticlePushConstants {
    float particleSize;
    float globalTime;
    uint emitterCount;
    uint _pad1;
};

vertex ParticleVertexOut particleVertex(
    uint vertexID [[vertex_id]],
    uint instanceID [[instance_id]],
    constant CameraData& camera [[buffer(0)]],
    constant ParticlePushConstants& pushConstants [[buffer(1)]],
    device const Particle* particles [[buffer(2)]],
    constant EmitterParams* emitters [[buffer(3)]]
) {
    Particle p = particles[instanceID];

    // Get per-emitter particle size if available
    float size = pushConstants.particleSize;
    if (p.emitterID < pushConstants.emitterCount) {
        size = emitters[p.emitterID].particleSize;
    }

    // Billboard: get camera right and up vectors from view matrix
    float3 cameraRight = float3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    float3 cameraUp = float3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);

    // Calculate billboard vertex position
    float2 quadPos = quadVertices[vertexID];
    float3 worldPos = p.position
                    + cameraRight * quadPos.x * size
                    + cameraUp * quadPos.y * size;

    float4 clipPos = camera.proj * camera.view * float4(worldPos, 1.0);

    ParticleVertexOut out;
    out.position = clipPos;
    out.uv = quadUVs[vertexID];
    out.color = p.color;
    out.clipPos = clipPos;
    out.emitterID = p.emitterID;

    return out;
}

// Linearize depth from [0,1] to view-space distance
float linearizeDepth(float depth, float near, float far) {
    return near * far / (far - depth * (far - near));
}

fragment float4 particleFragment(
    ParticleVertexOut in [[stage_in]],
    constant CameraData& camera [[buffer(0)]],
    constant ParticlePushConstants& pushConstants [[buffer(1)]],
    constant EmitterParams* emitters [[buffer(2)]],
    texture2d<float, access::sample> depthTexture [[texture(0)]]
) {
    // Circular particle with soft edges
    float2 center = in.uv - float2(0.5);
    float dist = length(center) * 2.0;

    // Soft circle falloff
    float alpha = 1.0 - smoothstep(0.0, 1.0, dist);

    // Additional glow effect
    float glow = exp(-dist * dist * 2.0);

    float finalAlpha = in.color.a * alpha;

    // Depth fade effect
    if (in.emitterID < pushConstants.emitterCount) {
        EmitterParams emitter = emitters[in.emitterID];

        if (emitter.depthFadeEnabled != 0) {
            // Convert clip pos to screen UV
            float2 screenUV = (in.clipPos.xy / in.clipPos.w) * 0.5 + 0.5;

            // Sample scene depth
            constexpr sampler depthSampler(filter::linear, address::clamp_to_edge);
            float sceneDepth = depthTexture.sample(depthSampler, screenUV).r;

            // Get particle depth (in NDC)
            float particleDepth = in.clipPos.z / in.clipPos.w;

            // Convert to linear depth for proper distance calculation
            float sceneLinearDepth = linearizeDepth(sceneDepth, camera.near, camera.far);
            float particleLinearDepth = linearizeDepth(particleDepth, camera.near, camera.far);

            // Calculate depth difference (positive = particle is in front)
            float depthDiff = sceneLinearDepth - particleLinearDepth;

            // Fade based on distance to scene geometry
            float depthFade = smoothstep(0.0, emitter.depthFadeDistance, depthDiff);

            finalAlpha *= depthFade;
        }
    }

    float4 outColor = float4(in.color.rgb * (alpha + glow * 0.5), finalAlpha);

    // Discard fully transparent pixels
    if (outColor.a < 0.01) {
        discard_fragment();
    }

    return outColor;
}
