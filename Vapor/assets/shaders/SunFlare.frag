#version 450
// Sun/lens flare — clean-room redesign, identical algorithm to
// sunflare_rhi.metal. Restrained by design: a tight gaussian core, one thin
// spectral halo ring, four chromatic ghosts along the sun->center axis and a
// subtle anamorphic streak. Output is HDR radiance rendered additively into
// colorRT before PostProcess, so bloom widens the core and ACES rolls it off.
// Sun occlusion is a smooth 5-tap depth-disk test (no binary popping).

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneDepth;

// Must match Vapor::SunFlareRenderData (std430).
layout(std430, set = 1, binding = 0) readonly buffer FlareBuf {
    vec2 sunScreenPos;
    vec2 aspectRatio;
    // Offsets mirror the MSL FlareData (float3 = 16 bytes there).
    vec3 sunColor;
    float _sunColorPad;
    float intensity;
    float glowSize;
    float haloRadius;
    float ghostSpacing;
    float streakIntensity;
    float _pad0;
    float _pad1;
    float _pad2;
};

float softDisk(vec2 p, vec2 c, float r) {
    // Smooth circular falloff: 1 at center -> 0 at radius.
    float d = length(p - c);
    return smoothstep(r, r * 0.35, d);
}

void main() {
    // Smooth sun visibility: fraction of a small depth-sample disk at the sun
    // position that still sees the far plane (sky).
    vec2 sunUV = clamp(sunScreenPos, vec2(0.001), vec2(0.999));
    float vis = 0.0;
    const vec2 taps[5] = vec2[5](
        vec2(0.0, 0.0), vec2(0.008, 0.0), vec2(-0.008, 0.0),
        vec2(0.0, 0.008), vec2(0.0, -0.008));
    for (int i = 0; i < 5; i++) {
        float d = texture(sceneDepth, clamp(sunUV + taps[i], vec2(0.001), vec2(0.999))).r;
        vis += step(0.9999, d);
    }
    vis *= 0.2;

    // Fade the whole effect out as the sun leaves the screen.
    vec2 sp = sunScreenPos;
    float edge = smoothstep(-0.1, 0.1, sp.x) * smoothstep(1.1, 0.9, sp.x)
               * smoothstep(-0.1, 0.1, sp.y) * smoothstep(1.1, 0.9, sp.y);
    float I = intensity * vis * edge;
    if (I < 0.001) { outColor = vec4(0.0); return; }

    // Aspect-corrected coordinates so every element stays circular.
    vec2 uv = tex_uv * aspectRatio;
    vec2 sun = sp * aspectRatio;
    vec2 center = vec2(0.5) * aspectRatio;

    vec3 flare = vec3(0.0);

    // 1) Core glow: tight gaussian, slightly warm.
    {
        float d = length(uv - sun);
        flare += sunColor * exp(-(d * d) / (glowSize * glowSize)) * 2.0;
    }

    // 2) Anamorphic streak: long horizontal, thin vertical, cool tint.
    {
        vec2 d = uv - sun;
        float streak = exp(-(d.y * d.y) / (0.0004)) * exp(-(d.x * d.x) / 0.08);
        flare += vec3(0.6, 0.75, 1.0) * sunColor * streak * streakIntensity;
    }

    // 3) Thin spectral halo ring around the sun.
    {
        float d = length(uv - sun);
        float ring = exp(-pow((d - haloRadius) / 0.012, 2.0));
        // Subtle radial rainbow: hue drifts with the angle around the sun.
        float a = atan(uv.y - sun.y, uv.x - sun.x);
        vec3 tint = vec3(0.9 + 0.1 * sin(a), 0.8 + 0.2 * sin(a + 2.1), 1.0);
        flare += tint * sunColor * ring * 0.15;
    }

    // 4) Four chromatic ghosts mirrored along the sun->center axis. Each is a
    //    soft disk sampled at three per-channel scales (dispersion).
    {
        vec2 axis = center - sun;
        for (int i = 1; i <= 4; i++) {
            vec2 gpos = sun + axis * (ghostSpacing * float(i) * 1.35);
            float gsize = glowSize * (1.6 - float(i) * 0.25);
            float r = softDisk(uv, gpos, gsize * 1.06);
            float g = softDisk(uv, gpos, gsize);
            float b = softDisk(uv, gpos, gsize * 0.94);
            float fade = 1.0 - float(i - 1) * 0.22;
            flare += vec3(r, g, b) * sunColor * (0.10 * fade);
        }
    }

    outColor = vec4(flare * I, 1.0);
}
