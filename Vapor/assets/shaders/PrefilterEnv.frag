#version 450
// Pre-filtered specular environment map: GGX importance-sampled convolution of
// the environment cubemap, one roughness per mip. GLSL twin of
// 3d_prefilter_envmap.metal.

layout(location = 0) in vec3 localPos;
layout(location = 0) out vec4 outColor;

// roughness for this mip (fragment reads the capture slot, like the Metal path)
layout(std430, set = 1, binding = 0) readonly buffer CaptureBuf {
    mat4  viewProj;
    uint  faceIndex;
    float roughness;
    vec2  _pad;
} capture;

layout(set = 2, binding = 0) uniform samplerCube environmentMap;

const float PI = 3.14159265359;

float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverseVdC(i)); }

vec3 importanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosT = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / (PI * d * d);
}

void main() {
    vec3 N = normalize(localPos);
    vec3 V = N;                         // split-sum: view == reflection == N
    float roughness = capture.roughness;

    const uint SAMPLES = 1024u;
    vec3 prefiltered = vec3(0.0);
    float totalW = 0.0;
    for (uint i = 0u; i < SAMPLES; ++i) {
        vec2 Xi = hammersley(i, SAMPLES);
        vec3 H = importanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float D = distributionGGX(NdotH, roughness);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;
            float resolution = 512.0;
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLES) * pdf + 0.0001);
            float mip = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);
            prefiltered += textureLod(environmentMap, L, mip).rgb * NdotL;
            totalW += NdotL;
        }
    }
    outColor = vec4(prefiltered / totalW, 1.0);
}
