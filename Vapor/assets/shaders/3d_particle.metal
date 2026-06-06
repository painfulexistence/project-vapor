#include "Res/shaders/3d_common.metal"

#define MAX_ATTRACTORS 8

// In Metal device address space float3 = 12 bytes; the following float fills
// the gap to 16, matching the CPU GPUParticle layout exactly.
struct Particle {
    float3 position;
    float  lifetime;   // 0 = immortal
    float3 velocity;
    float  age;        // incremented by particleIntegrate each frame
    float3 force;
    float  _pad3;
    float4 color;
};

// Matches ParticleSimulationParams (CPU, 64 bytes).
// In Metal constant address space float3 is 16 bytes, so wind/turbulence use float4.
struct ParticleSimParams {
    float2 resolution;
    float2 mousePosition;
    float  time;
    float  deltaTime;
    uint   particleCount;
    uint   attractorCount;
    float4 wind;       // xyz = direction, w = strength
    float4 turbulence; // w = strength (xyz reserved)
};

// ---- Curl noise ----
float hash(float n) { return fract(sin(n) * 43758.5453123f); }

float vnoise(float3 p) {
    float3 i = floor(p); float3 f = fract(p);
    f = f * f * (3.0f - 2.0f * f);
    float n = i.x + i.y * 57.0f + i.z * 113.0f;
    return mix(
        mix(mix(hash(n),       hash(n+1.0f),   f.x),
            mix(hash(n+57.0f), hash(n+58.0f),  f.x), f.y),
        mix(mix(hash(n+113.0f),hash(n+114.0f), f.x),
            mix(hash(n+170.0f),hash(n+171.0f), f.x), f.y), f.z);
}

float3 curlNoise(float3 p, float t) {
    const float e  = 0.1f;
    const float3 o1 = float3(1.7f, 9.2f, 3.5f);
    const float3 o2 = float3(5.4f, 2.1f, 7.8f);
    float3 q = p * 0.5f + float3(t * 0.07f);

    float dFzdy = vnoise(q+o2+float3(0,e,0)) - vnoise(q+o2-float3(0,e,0));
    float dFydz = vnoise(q+o1+float3(0,0,e)) - vnoise(q+o1-float3(0,0,e));
    float dFxdz = vnoise(q   +float3(0,0,e)) - vnoise(q   -float3(0,0,e));
    float dFzdx = vnoise(q+o2+float3(e,0,0)) - vnoise(q+o2-float3(e,0,0));
    float dFydx = vnoise(q+o1+float3(e,0,0)) - vnoise(q+o1-float3(e,0,0));
    float dFxdy = vnoise(q   +float3(0,e,0)) - vnoise(q   -float3(0,e,0));

    return float3(dFzdy-dFydz, dFxdz-dFzdx, dFydx-dFxdy) / (2.0f * e);
}

// ============================================================================
// Compute Kernels
// ============================================================================

kernel void particleForce(
    device Particle*           particles  [[buffer(0)]],
    constant ParticleSimParams& params    [[buffer(1)]],
    constant float4*           attractors [[buffer(2)]],  // xyz=pos, w=strength
    uint id [[thread_position_in_grid]]
) {
    if (id >= params.particleCount) return;

    Particle p = particles[id];

    // Dead particles: skip force calculation
    if (p.lifetime > 0.0 && p.age >= p.lifetime) {
        particles[id] = p;
        return;
    }

    p.force = float3(0.0);

    // Attractor forces
    for (uint i = 0u; i < params.attractorCount; i++) {
        float3 aPos      = attractors[i].xyz;
        float  aStrength = attractors[i].w;
        float3 toA       = aPos - p.position;
        float  dist      = length(toA);
        if (dist > 0.001) {
            float forceMag = aStrength / (dist * dist + 0.1);
            p.force += (toA / dist) * forceMag;
        }
    }

    // Wind force
    p.force += params.wind.xyz * params.wind.w;

    // Curl-noise turbulence
    if (params.turbulence.w > 0.0f)
        p.force += curlNoise(p.position, params.time) * params.turbulence.w;

    // Damping
    p.force -= p.velocity * 0.5f;

    particles[id] = p;
}

kernel void particleIntegrate(
    device Particle*           particles [[buffer(0)]],
    constant ParticleSimParams& params   [[buffer(1)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= params.particleCount) return;

    Particle p = particles[id];

    // Dead: hide and stop
    if (p.lifetime > 0.0 && p.age >= p.lifetime) {
        p.color.a = 0.0;
        particles[id] = p;
        return;
    }

    p.age += params.deltaTime;

    // Fade alpha in the last 30% of lifetime
    if (p.lifetime > 0.0) {
        float t = p.age / p.lifetime;
        p.color.a = clamp(1.0 - smoothstep(0.7, 1.0, t), 0.0, 1.0);
    }

    // Semi-implicit Euler
    p.velocity += p.force * params.deltaTime;
    p.position += p.velocity * params.deltaTime;

    // Soft boundary sphere
    float maxDist = 20.0;
    float d = length(p.position);
    if (d > maxDist) {
        p.position = normalize(p.position) * maxDist;
        p.velocity = reflect(p.velocity, normalize(p.position)) * 0.5;
    }

    particles[id] = p;
}

// ============================================================================
// Render Shaders
// ============================================================================

constant float2 quadVertices[6] = {
    float2(-1.0, -1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
    float2(-1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0)
};

constant float2 quadUVs[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0)
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
    constant CameraData&            camera        [[buffer(0)]],
    constant ParticlePushConstants& pushConstants [[buffer(1)]],
    device const Particle*          particles     [[buffer(2)]]
) {
    Particle p = particles[instanceID];

    float3 cameraRight = float3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    float3 cameraUp    = float3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);

    float2 quadPos  = quadVertices[vertexID];
    float  size     = pushConstants.particleSize;
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
