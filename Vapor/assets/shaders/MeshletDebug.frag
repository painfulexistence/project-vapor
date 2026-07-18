#version 450
// Meshlet debug fragment (Vulkan): per-meshlet color, or a lambertian shade from
// the mesh stage's world normal. Bring-up visualization for meshlet culling +
// cluster-LOD selection; full PBR material binding for the meshlet path is a
// follow-up.

layout(location = 0) in vec3 meshletColor;
layout(location = 1) in vec3 meshletWorldNormal;
layout(location = 0) out vec4 outColor;

// Fragment-stage push constant at offset 64 (RHI setFragmentBytes binding 0; the
// task/mesh MeshletParams occupy [0,64)). 1 = per-meshlet debug hashColor,
// 0 = lambertian from the interpolated world normal.
layout(push_constant) uniform FragParams { layout(offset = 64) uint debugColor; };

void main() {
    if (debugColor != 0u) {
        outColor = vec4(meshletColor, 1.0);
        return;
    }
    vec3 N = normalize(meshletWorldNormal);
    vec3 L = normalize(vec3(0.4, 0.7, 0.5));
    float ndl = max(dot(N, L), 0.0);
    outColor = vec4(vec3(0.8) * (0.12 + 0.88 * ndl), 1.0);
}
