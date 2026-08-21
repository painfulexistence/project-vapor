#version 460
// Adaptive GPU tessellation, instanced draw path — GLSL twin of
// 3d_tess_render.metal's tessVertexMain. One instance per CBT leaf
// (instanceCount GPU-written, consumed via drawIndexedIndirect). Pulls a grid
// vertex's barycentrics from the SSBO at b0 and lerps the leaf corners the
// TessLeafPrep kernel cached.
//
// Bindings: 0 = grid barycentrics (vec2: w1, w2), 1 = CameraData,
//           2 = TessLeafData[], 3 = TessParams (host-visible buffer on
//           Vulkan; Metal uses setVertexBytes).
#extension GL_GOOGLE_include_directive : require
#include "include/tess_common.glsl"

layout(std430, set = 0, binding = 0) readonly buffer GridBuf { vec2 gridVerts[]; };
layout(std430, set = 0, binding = 1) readonly buffer CameraBuf { TessCameraData cam; };

struct TessLeafData {
    vec4 posU[3];
    vec4 nrmV[3];
    uint visible;
    uint depth;
    uint node;
    uint pad;
};
layout(std430, set = 0, binding = 2) readonly buffer LeafBuf { TessLeafData leaves[]; };
layout(std430, set = 0, binding = 3) readonly buffer ParamsBuf { TessParams params; };

layout(location = 0) out vec3 outWorldNormal;
layout(location = 1) out vec3 outWorldPosition;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out uint outDepth;
layout(location = 4) flat out uint outNode;

void main() {
    TessLeafData leaf = leaves[gl_InstanceIndex];
    if (leaf.visible == 0u) {
        // Frustum-culled leaf: collapse the whole instance to one point so
        // the rasterizer drops it (kept instead of compacted — the leaf list
        // stays in deterministic heap order).
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outWorldNormal = vec3(0, 1, 0);
        outWorldPosition = vec3(0);
        outUV = vec2(0);
        outDepth = 0u;
        outNode = 0u;
        return;
    }

    vec2 g = gridVerts[gl_VertexIndex];
    vec3 w = vec3(1.0 - g.x - g.y, g.x, g.y);  // (w0, w1, w2)

    vec3 pos = w.x * leaf.posU[0].xyz + w.y * leaf.posU[1].xyz + w.z * leaf.posU[2].xyz;
    vec3 nrm = w.x * leaf.nrmV[0].xyz + w.y * leaf.nrmV[1].xyz + w.z * leaf.nrmV[2].xyz;
    vec2 uv = w.x * vec2(leaf.posU[0].w, leaf.nrmV[0].w) +
              w.y * vec2(leaf.posU[1].w, leaf.nrmV[1].w) +
              w.z * vec2(leaf.posU[2].w, leaf.nrmV[2].w);

    // Displacement is a function of the undisplaced object-space position
    // only, so leaves sharing an edge displace its vertices identically.
    nrm = normalize(nrm);
    pos += nrm * tessDisplaceAmount(pos, params.displacementScale);

    vec4 world = params.model * vec4(pos, 1.0);
    gl_Position = cam.proj * cam.view * world;
    outWorldPosition = world.xyz;
    // Uniform-scale assumption for the normal (matches the debug shading).
    outWorldNormal = normalize((params.model * vec4(nrm, 0.0)).xyz);
    outUV = uv;
    outDepth = leaf.depth;
    outNode = leaf.node;
}
