#version 450
// MicroVoxel primary pass, vertex stage. GLSL twin of 3d_microvoxel.metal's
// microVoxelVertex — keep the binding contracts mirrored.
//
// Draws one volume's world-space AABB as 36 shader-generated cube vertices
// (no vertex buffer); only covered pixels run the fragment DDA, which is what
// keeps many small volumes affordable. Culling is off in the pipeline so the
// box still covers its footprint when the camera is inside it.
//
// Binding convention (see rhi_vulkan.cpp): set 0 = vertex-stage buffers.
// The params buffer is the same 256-byte-strided MicroVoxelRenderData slice
// the fragment reads at set 1, bound per volume with an offset.

layout(location = 0) out vec3 v_worldPos;

layout(std430, set = 0, binding = 0) readonly buffer ParamsBuf {
    mat4 viewProj;
    vec4 cameraPosition;   // xyz; w = maxRaySteps
    vec4 volumeOrigin;     // xyz = world min corner; w = voxelSize
    vec4 gridDim;          // xyz = voxel counts; w = emissiveStrength
    vec4 sunDirection;     // xyz toward the sun; w = shadowEnabled
    vec4 sunColor;         // xyz; w = sunIntensity
    vec4 ambientSky;       // xyz; w = ambientIntensity
    vec4 ambientGround;    // xyz; w = albedo hash variation strength
    vec4 params;           // x = aoStrength, y = debugMode, z = reflectionsEnabled
};

// Unit cube [0,1]^3 as a 36-vertex triangle list.
const vec3 cubeVerts[36] = vec3[](
    // -Z face
    vec3(0, 0, 0), vec3(1, 1, 0), vec3(1, 0, 0),
    vec3(0, 0, 0), vec3(0, 1, 0), vec3(1, 1, 0),
    // +Z face
    vec3(0, 0, 1), vec3(1, 0, 1), vec3(1, 1, 1),
    vec3(0, 0, 1), vec3(1, 1, 1), vec3(0, 1, 1),
    // -X face
    vec3(0, 0, 0), vec3(0, 0, 1), vec3(0, 1, 1),
    vec3(0, 0, 0), vec3(0, 1, 1), vec3(0, 1, 0),
    // +X face
    vec3(1, 0, 0), vec3(1, 1, 1), vec3(1, 0, 1),
    vec3(1, 0, 0), vec3(1, 1, 0), vec3(1, 1, 1),
    // -Y face
    vec3(0, 0, 0), vec3(1, 0, 0), vec3(1, 0, 1),
    vec3(0, 0, 0), vec3(1, 0, 1), vec3(0, 0, 1),
    // +Y face
    vec3(0, 1, 0), vec3(1, 1, 1), vec3(1, 1, 0),
    vec3(0, 1, 0), vec3(0, 1, 1), vec3(1, 1, 1)
);

void main() {
    vec3 extent = gridDim.xyz * volumeOrigin.w;
    vec3 wp = volumeOrigin.xyz + cubeVerts[gl_VertexIndex] * extent;
    v_worldPos = wp;
    gl_Position = viewProj * vec4(wp, 1.0);
}
