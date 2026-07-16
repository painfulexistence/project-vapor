#version 450
// Meshlet depth+normal pre-pass fragment (Vulkan). MRT twin of MeshletDebug.frag
// for the GPU-driven meshlet pre-pass: output 0 = world normal (normalRT,
// RGBA16F), output 1 = base color (albedoRT, RGBA8). The meshlet path has no PBR
// material bind yet, so albedo carries the per-meshlet debug color — enough for
// the downstream passes that only consume depth + a normal. Mirror of
// 3d_meshlet.metal's fragmentPrePass.

layout(location = 0) in vec3 meshletColor;
layout(location = 1) in vec3 meshletWorldNormal;

layout(location = 0) out vec4 outNormal;
layout(location = 1) out vec4 outAlbedo;

void main() {
    outNormal = vec4(normalize(meshletWorldNormal), 1.0);
    outAlbedo = vec4(meshletColor, 1.0);
}
