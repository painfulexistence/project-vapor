#version 450
// Adaptive GPU tessellation — instanced draw path, fragment stage (Vulkan
// twin of tessFragmentMain in 3d_tess_render.metal): LoD debug hash color per
// subdivision depth, or the interpolated terrain palette color for
// TESS_FLAG_TERRAIN instances; simple lambert + ambient either way.

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vWorldPosition;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec3 vTerrainColor;
layout(location = 4) flat in float vTerrainMix;
layout(location = 5) flat in uint vDepth;

layout(location = 0) out vec4 outColor;

vec3 tessHashColor(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x = x ^ (x >> 4u);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return vec3(float(x & 255u), float((x >> 8u) & 255u), float((x >> 16u) & 255u)) / 255.0;
}

void main() {
    vec3 base = mix(tessHashColor(vDepth * 2654435761u), vTerrainColor, vTerrainMix);
    vec3 lightDir = normalize(vec3(0.4, 1.0, 0.3));
    float ndl = max(dot(normalize(vWorldNormal), lightDir), 0.0);
    vec3 color = base * (0.25 + 0.75 * ndl);
    outColor = vec4(color, 1.0);
}
