#include "Res/shaders/3d_common.metal"

// GPU Particle structure for compute shader simulation.
// Matches render_data.hpp GPUParticleData (64 bytes).
// Metal float3 occupies 16 bytes (implicit 4-byte pad after each field),
// which aligns with C++ (glm::vec3 + float pad = 16 bytes).
struct Particle {
    float3 position;  // bytes  0-15 (12 data + 4 implicit pad = lifetime slot)
    float3 velocity;  // bytes 16-31 (12 data + 4 implicit pad = age slot)
    float3 force;     // bytes 32-47 (12 data + 4 implicit pad)
    float4 color;     // bytes 48-63
};

// Canonical sim params (64 bytes) — matches C++ ParticleSimParams in render_data.hpp.
struct ParticleSimParams {
    float2 resolution;    // offset 0
    float2 mousePosition; // offset 8
    float  time;          // offset 16
    float  deltaTime;     // offset 20
    uint   particleCount; // offset 24
    uint   attractorCount;// offset 28
    float4 wind;          // offset 32 — xyz=dir, w=strength
    float4 turbulence;    // offset 48 — w=strength
};

// Attractor (32 bytes) — matches C++ ParticleAttractor in render_data.hpp.
// Metal float3 has 16-byte stride, so strength is at byte offset 16 inside
// the struct — same as C++ (glm::vec3 + _pad0 + float strength + _pad1[3]).
struct Attractor {
    float3 position; // 16 bytes (implicit pad)
    float  strength; // offset 16
};

// ── Divergence-free curl noise ──────────────────────────────────────────────
float hash_p(float n) { return fract(sin(n) * 43758.5453); }

float vnoise(float3 p) {
    float3 i = floor(p);
    float3 f = fract(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    float n = i.x + i.y * 57.0 + i.z * 113.0;
    return mix(mix(mix(hash_p(n),         hash_p(n + 1.0),   u.x),
                   mix(hash_p(n + 57.0),  hash_p(n + 58.0),  u.x), u.y),
               mix(mix(hash_p(n + 113.0), hash_p(n + 114.0), u.x),
                   mix(hash_p(n + 170.0), hash_p(n + 171.0), u.x), u.y), u.z);
}

float3 curlNoise(float3 p, float t) {
    const float e  = 0.1;
    const float3 o1 = float3(1.7, 9.2, 3.5);
    const float3 o2 = float3(5.4, 2.1, 7.8);
    float3 q = p * 0.5 + float3(t * 0.07);

    float dFzdy = vnoise(q + o1 + float3(0, e, 0)) - vnoise(q + o1 - float3(0, e, 0));
    float dFydz = vnoise(q + o2 + float3(0, 0, e)) - vnoise(q + o2 - float3(0, 0, e));
    float dFxdz = vnoise(q + o1 + float3(0, 0, e)) - vnoise(q + o1 - float3(0, 0, e));
    float dFzdx = vnoise(q + o2 + float3(e, 0, 0)) - vnoise(q + o2 - float3(e, 0, 0));
    float dFydx = vnoise(q + o1 + float3(e, 0, 0)) - vnoise(q + o1 - float3(e, 0, 0));
    float dFxdy = vnoise(q + o2 + float3(0, e, 0)) - vnoise(q + o2 - float3(0, e, 0));

    return float3(dFzdy - dFydz, dFxdz - dFzdx, dFydx - dFxdy) / (2.0 * e);
}

// ============================================================================
// Compute Kernels
// ============================================================================

kernel void particleForce(
    device Particle*               particles  [[buffer(0)]],
    constant ParticleSimParams&    params     [[buffer(1)]],
    constant Attractor*            attractors [[buffer(2)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= params.particleCount) return;

    Particle p = particles[id];

    // Reset force
    p.force = float3(0.0);

    // Accumulate attractor forces (inverse-square with softening)
    for (uint i = 0; i < params.attractorCount; i++) {
        float3 toAttr = attractors[i].position - p.position;
        float  dist   = length(toAttr);
        if (dist > 0.001) {
            float3 dir      = toAttr / dist;
            float  forceMag = attractors[i].strength / (dist * dist + 0.1);
            p.force += dir * forceMag;
        }
    }

    // Wind
    if (params.wind.w > 0.0) {
        p.force += params.wind.xyz * params.wind.w;
    }

    // Divergence-free curl noise turbulence
    if (params.turbulence.w > 0.0) {
        p.force += curlNoise(p.position, params.time) * params.turbulence.w;
    }

    // Velocity-dependent drag — matches Vulkan (0.5)
    p.force -= p.velocity * 0.5;

    particles[id] = p;
}

kernel void particleIntegrate(
    device Particle*            particles [[buffer(0)]],
    constant ParticleSimParams& params    [[buffer(1)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= params.particleCount) return;

    Particle p = particles[id];

    // Semi-implicit Euler integration
    p.velocity += p.force * params.deltaTime;
    p.position += p.velocity * params.deltaTime;

    // Boundary conditions for orbital demo particles (no lifetime)
    float maxDist = 20.0;
    float dist = length(p.position);
    if (dist > maxDist) {
        p.position = normalize(p.position) * maxDist;
        float3 normal = normalize(p.position);
        p.velocity = reflect(p.velocity, normal) * 0.5;
    }

    particles[id] = p;
}

// ============================================================================
// Render Shaders
// ============================================================================

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
    uint vertexID   [[vertex_id]],
    uint instanceID [[instance_id]],
    constant CameraData&              camera        [[buffer(0)]],
    constant ParticlePushConstants&   pushConstants [[buffer(1)]],
    device const Particle*            particles     [[buffer(2)]]
) {
    Particle p = particles[instanceID];

    float3 cameraRight = float3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    float3 cameraUp    = float3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);

    float2 quadPos = quadVertices[vertexID];
    float  size    = pushConstants.particleSize;
    float3 worldPos = p.position
                    + cameraRight * quadPos.x * size
                    + cameraUp    * quadPos.y * size;

    ParticleVertexOut out;
    out.position = camera.proj * camera.view * float4(worldPos, 1.0);
    out.uv       = quadUVs[vertexID];
    out.color    = p.color;
    return out;
}

fragment float4 particleFragment(ParticleVertexOut in [[stage_in]]) {
    float2 center = in.uv - float2(0.5);
    float  dist   = length(center) * 2.0;

    float alpha = 1.0 - smoothstep(0.0, 1.0, dist);
    float glow  = exp(-dist * dist * 2.0);

    float4 outColor = float4(in.color.rgb * (alpha + glow * 0.5), in.color.a * alpha);
    if (outColor.a < 0.01) discard_fragment();
    return outColor;
}
