#version 450
// MicroVoxel primary pass, vertex stage. GLSL twin of 3d_microvoxel.metal's
// microVoxelVertex — keep the binding contracts mirrored.
//
// Draws one volume's world-space AABB as 36 shader-generated cube vertices
// (no vertex buffer); only covered pixels run the fragment DDA, which is what
// keeps many small volumes affordable. The pipeline culls FRONT faces, so only
// the box's far (back) faces rasterize — one fragment per covered pixel, and
// the footprint is still covered when the camera sits inside the box (the near
// faces it would otherwise see are the culled ones). The cube winding below is
// CCW-from-outside for exactly this reason.
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
    vec4 extra0;           // (unused here; keeps the 256-byte layout aligned with the fragment)
    vec4 rotationQuat;     // volume orientation (x,y,z,w); identity = axis-aligned
    vec4 boundsMin;        // tight solid bounds, LOCAL grid frame (meters)
    vec4 boundsMax;
};

vec3 quatRotate(vec4 q, vec3 v) {  // active rotation, q = (x,y,z,w)
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

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
    // Rasterize the TIGHT box (boundsMin..boundsMax in the local grid frame),
    // then rotate about the volume pivot (min corner + half the x/z extent) so a
    // physics-driven orientation turns the whole box. Full bounds + identity
    // rotation give exactly volumeOrigin + vert*extent, the axis-aligned box.
    vec3 lp = mix(boundsMin.xyz, boundsMax.xyz, cubeVerts[gl_VertexIndex]);
    vec3 cXZ = vec3(extent.x * 0.5, 0.0, extent.z * 0.5);
    vec3 pivot = volumeOrigin.xyz + cXZ;
    vec3 wp = pivot + quatRotate(rotationQuat, lp - cXZ);
    v_worldPos = wp;
    gl_Position = viewProj * vec4(wp, 1.0);
}
