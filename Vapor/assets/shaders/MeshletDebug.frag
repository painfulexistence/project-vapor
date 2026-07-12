#version 450
// Meshlet debug fragment (Vulkan): flat per-meshlet color from the mesh stage.
// Bring-up visualization for meshlet culling + cluster-LOD selection; PBR
// parity for the meshlet path is a follow-up.

layout(location = 0) in vec3 meshletColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(meshletColor, 1.0);
}
