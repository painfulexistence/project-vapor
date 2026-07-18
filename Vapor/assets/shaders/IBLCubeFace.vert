#version 450
// Shared IBL capture vertex shader (Vulkan): fullscreen triangle whose per-vertex
// output is the world-space direction for the current cubemap face. Used by the
// equirect->cubemap, irradiance-convolution and prefilter passes.

layout(location = 0) out vec3 localPos;

// Matches IBLCaptureRenderData (C++) / IBLCaptureData (MSL). Bound with a
// per-face range (set 0 binding 0), so it holds one entry.
struct IBLCaptureData {
    mat4 viewProj;
    uint faceIndex;
    float roughness;
    uint _pad0;
    uint _pad1;
};
layout(std430, set = 0, binding = 0) readonly buffer CaptureBuf {
    IBLCaptureData capture;
};

const vec2 ndcVerts[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

// Cubemap face UV -> world direction (same convention as the MSL capture shaders).
vec3 uvToDirection(vec2 uv, uint face) {
    vec2 st = uv * 2.0 - 1.0;
    vec3 dir;
    if      (face == 0u) dir = vec3( 1.0, -st.y, -st.x); // +X
    else if (face == 1u) dir = vec3(-1.0, -st.y,  st.x); // -X
    else if (face == 2u) dir = vec3( st.x,  1.0,  st.y); // +Y
    else if (face == 3u) dir = vec3( st.x, -1.0, -st.y); // -Y
    else if (face == 4u) dir = vec3( st.x, -st.y,  1.0); // +Z
    else                 dir = vec3(-st.x, -st.y, -1.0); // -Z
    // Return the UN-normalized direction. It is affine in uv, so the rasterizer
    // interpolates it EXACTLY across the fullscreen triangle; normalizing here
    // (per vertex) and then interpolating would lerp normalized corners and bend
    // the per-pixel direction (~24deg off at a face centre), which distorted the
    // whole captured cubemap. Every consumer fragment re-normalizes localPos.
    return dir;
}

void main() {
    vec2 v = ndcVerts[gl_VertexIndex];
    gl_Position = vec4(v, 0.0, 1.0);
    vec2 uv = v * 0.5 + 0.5;
    localPos = uvToDirection(uv, capture.faceIndex);
}
