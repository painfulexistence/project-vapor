#version 450
// Edge-aware à-trous wavelet filter for the AO chain — GLSL twin of
// 3d_ao_denoise.metal as a fullscreen fragment pass. src is RGBA16F
// (ao, view-space depth, octahedral normal): everything the edge stops need
// rides in one texture. Run iteratively with stride 1, 2, (4, ...); the final
// iteration may target the single-channel aoRT — extra channels are dropped.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outAO;

layout(set = 2, binding = 0) uniform sampler2D src;

// Renderer stride via setFragmentBytes(binding=0) -> offset 64
layout(push_constant) uniform DenoisePC {
    layout(offset = 64) uint stridePx;
};

const float kAtrousKernel1D[5] = float[5](1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0);

vec3 octDecode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

void main() {
    ivec2 dim = textureSize(src, 0);
    ivec2 tid = ivec2(gl_FragCoord.xy);

    vec4 center = texelFetch(src, tid, 0);
    float centerZ = center.g;
    vec3 centerN = octDecode(center.ba);

    float sum = 0.0;
    float weightSum = 0.0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            ivec2 tap = tid + ivec2(dx, dy) * int(stridePx);
            if (tap.x < 0 || tap.y < 0 || tap.x >= dim.x || tap.y >= dim.y) continue;

            vec4 s = texelFetch(src, tap, 0);
            vec3 n = octDecode(s.ba);

            float wKernel = kAtrousKernel1D[dx + 2] * kAtrousKernel1D[dy + 2];
            float wZ = exp(-abs(s.g - centerZ) / (0.05 * max(abs(centerZ), 1.0)));
            float wN = pow(max(0.0, dot(centerN, n)), 32.0);

            float weight = wKernel * wZ * wN;
            sum += s.r * weight;
            weightSum += weight;
        }
    }
    float ao = weightSum > 1e-6 ? sum / weightSum : center.r;
    outAO = vec4(ao, centerZ, center.ba);
}
