#include "Res/shaders/3d_common.metal"

// GPU Particle structure for compute shader simulation.
// Matches render_data.hpp GPUParticleData (64 bytes).
//
// IMPORTANT: Metal float3 in a struct aligns to 16 bytes — its .w component
// is inaccessible implicit padding. Using float4 instead makes the lifetime
// and age fields explicitly readable, and keeps byte offsets identical to C++:
//   posLifetime.xyz = position (offset 0),  .w = lifetime (offset 12)
//   velAge.xyz      = velocity (offset 16), .w = age      (offset 28)
//   forcePad.xyz    = force    (offset 32), .w = _pad3    (offset 44)
//   color                                             offset 48
struct Particle {
    float4 posLifetime; // xyz=position, w=lifetime (-1=immortal)
    float4 velAge;      // xyz=velocity, w=age (seconds since spawn)
    float4 forcePad;    // xyz=force, w=_pad3
    float4 color;
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
    float3 pos      = p.posLifetime.xyz;
    float  lifetime = p.posLifetime.w;
    float3 vel      = p.velAge.xyz;
    float3 force    = p.forcePad.xyz;

    // Skip dead particles (force pass mirrors GLSL)
    if (lifetime >= 0.0 && p.velAge.w >= lifetime) {
        particles[id] = p;
        return;
    }

    // Reset force
    force = float3(0.0);

    // Accumulate attractor forces (inverse-square with softening)
    for (uint i = 0; i < params.attractorCount; i++) {
        float3 toAttr = attractors[i].position - pos;
        float  dist   = length(toAttr);
        if (dist > 0.001) {
            float3 dir      = toAttr / dist;
            float  forceMag = attractors[i].strength / (dist * dist + 0.1);
            force += dir * forceMag;
        }
    }

    // Wind
    if (params.wind.w > 0.0) {
        force += params.wind.xyz * params.wind.w;
    }

    // Divergence-free curl noise turbulence
    if (params.turbulence.w > 0.0) {
        force += curlNoise(pos, params.time) * params.turbulence.w;
    }

    // Velocity-dependent drag — matches Vulkan (0.5)
    force -= vel * 0.5;

    p.forcePad = float4(force, p.forcePad.w);
    particles[id] = p;
}

kernel void particleIntegrate(
    device Particle*            particles [[buffer(0)]],
    constant ParticleSimParams& params    [[buffer(1)]],
    uint id [[thread_position_in_grid]]
) {
    if (id >= params.particleCount) return;

    Particle p = particles[id];
    float3 pos      = p.posLifetime.xyz;
    float  lifetime = p.posLifetime.w;
    float  age      = p.velAge.w;
    float3 vel      = p.velAge.xyz;
    float3 force    = p.forcePad.xyz;

    // Advance age; fade and kill finite-lifetime particles (mirrors GLSL)
    if (lifetime >= 0.0) {
        age += params.deltaTime;
        p.velAge.w = age;
        if (age >= lifetime) {
            particles[id] = p;
            return;
        }
        // Fade alpha in last 30% of life
        float frac = age / lifetime;
        if (frac > 0.7) p.color.a = 1.0 - (frac - 0.7) / 0.3;
    }

    // Semi-implicit Euler integration
    vel   += force * params.deltaTime;
    pos   += vel   * params.deltaTime;

    // Boundary conditions for immortal orbital demo particles
    if (lifetime < 0.0) {
        float maxDist = 20.0;
        float dist = length(pos);
        if (dist > maxDist) {
            pos = normalize(pos) * maxDist;
            float3 normal = normalize(pos);
            vel = reflect(vel, normal) * 0.5;
        }
    }

    p.posLifetime = float4(pos, lifetime);
    p.velAge      = float4(vel, age);
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
    float  linearDepth;  // for depth fade
    float2 screenUV;     // for depth sampling
};

// Extended push constants for depth effects (32 bytes, matches Vulkan layout)
struct ParticlePushConstants {
    float particleSize;
    float useTexture;         // > 0.5: sample the per-emitter texture; else procedural disc
    float depthFadeEnabled;   // > 0.5: apply depth fade
    float depthFadeDistance;
    float groundClampEnabled; // > 0.5: clamp to depth surface
    float groundClampOffset;  // height above surface
    float _pad0;
    float _pad1;
};

float linearizeDepthMetal(float d, float nearPlane, float farPlane) {
    return nearPlane * farPlane / (farPlane - d * (farPlane - nearPlane));
}

float3 reconstructWorldPositionMetal(float2 screenUV, float depth, float4x4 invProj, float4x4 invView) {
    float2 ndc = screenUV * 2.0 - 1.0;
    // Metal uses reverse-Z, so depth 1.0 is near, 0.0 is far
    float4 clipPos = float4(ndc.x, -ndc.y, depth, 1.0);
    float4 viewPos = invProj * clipPos;
    viewPos /= viewPos.w;
    float4 worldPos = invView * viewPos;
    return worldPos.xyz;
}

vertex ParticleVertexOut particleVertex(
    uint vertexID   [[vertex_id]],
    uint instanceID [[instance_id]],
    constant CameraData&              camera        [[buffer(0)]],
    constant ParticlePushConstants&   pushConstants [[buffer(1)]],
    device const Particle*            particles     [[buffer(2)]],
    depth2d<float>                    sceneDepth    [[texture(1)]]
) {
    Particle p = particles[instanceID];
    float3 worldPos = p.posLifetime.xyz;

    // Ground clamping: project particle to screen, sample depth, clamp to surface
    if (pushConstants.groundClampEnabled > 0.5) {
        float4 clipPos = camera.proj * camera.view * float4(worldPos, 1.0);
        if (clipPos.w > 0.0) {
            float2 ndc = clipPos.xy / clipPos.w;
            // Metal NDC Y is flipped vs clip space
            float2 screenUV = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);

            // Only clamp if on screen
            if (screenUV.x >= 0.0 && screenUV.x <= 1.0 &&
                screenUV.y >= 0.0 && screenUV.y <= 1.0) {
                constexpr sampler depthSampler(filter::linear, address::clamp_to_edge);
                float sceneDepthSample = sceneDepth.sample(depthSampler, screenUV);
                float3 surfacePos = reconstructWorldPositionMetal(
                    screenUV, sceneDepthSample, camera.invProj, camera.invView);

                if (worldPos.y < surfacePos.y + pushConstants.groundClampOffset) {
                    worldPos.y = surfacePos.y + pushConstants.groundClampOffset;
                }
            }
        }
    }

    float3 cameraRight = float3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    float3 cameraUp    = float3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);

    float2 quadPos  = quadVertices[vertexID];
    float  size     = pushConstants.particleSize;
    float3 billboardPos = worldPos
                        + cameraRight * quadPos.x * size
                        + cameraUp    * quadPos.y * size;

    float4 viewPos = camera.view * float4(billboardPos, 1.0);
    float4 clipPos = camera.proj * viewPos;

    ParticleVertexOut out;
    out.position    = clipPos;
    out.uv          = quadUVs[vertexID];
    out.color       = p.color;
    out.linearDepth = -viewPos.z; // negated because view Z is negative forward
    // Compute screen UV for depth sampling
    float2 ndc = clipPos.xy / clipPos.w;
    out.screenUV = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    return out;
}

fragment float4 particleFragment(
    ParticleVertexOut               in              [[stage_in]],
    constant ParticlePushConstants& pc              [[buffer(0)]],
    constant CameraData&            camera          [[buffer(1)]],
    texture2d<float>                particleTexture [[texture(0)]],
    depth2d<float>                  sceneDepth      [[texture(1)]],
    sampler                         particleSampler [[sampler(0)]]
) {
    float4 outColor;
    if (pc.useTexture > 0.5) {
        // Textured sprite, modulated by per-particle color (carries the age fade).
        outColor = particleTexture.sample(particleSampler, in.uv) * in.color;
    } else {
        // Procedural: soft circular falloff + additive glow core.
        float2 center = in.uv - float2(0.5);
        float  dist   = length(center) * 2.0;

        float alpha = 1.0 - smoothstep(0.0, 1.0, dist);
        float glow  = exp(-dist * dist * 2.0);

        outColor = float4(in.color.rgb * (alpha + glow * 0.5), in.color.a * alpha);
    }

    // Depth fade: soft fade when particle is close to scene geometry
    if (pc.depthFadeEnabled > 0.5 && pc.depthFadeDistance > 0.0) {
        constexpr sampler depthSampler(filter::linear, address::clamp_to_edge);
        float sceneDepthSample = sceneDepth.sample(depthSampler, in.screenUV);
        float sceneLinearDepth = linearizeDepthMetal(sceneDepthSample, camera.near, camera.far);
        float depthDiff = sceneLinearDepth - in.linearDepth;

        float depthFade = saturate(depthDiff / pc.depthFadeDistance);
        outColor.a *= depthFade;
    }

    if (outColor.a < 0.01) discard_fragment();
    return outColor;
}
