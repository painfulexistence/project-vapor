#version 450
// Shared IBL-bake vertex: a fullscreen triangle whose interpolated output is
// the world-space direction for the cubemap FACE being rendered (skyCapture /
// irradiance / prefilter all use this). GLSL twin of the Metal IBL vertexMain.
// The engine's negative-height viewport already matches the GL/Metal NDC
// convention, so the face orientation lines up with the Metal bake with no flip.

layout(std430, set = 0, binding = 0) readonly buffer CaptureBuf {
    mat4  viewProj;
    uint  faceIndex;
    float roughness;
    vec2  _pad;
} capture;

layout(location = 0) out vec3 localPos;

vec2 ndcVerts[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

vec3 uvToDirection(vec2 uv, uint face) {
    vec2 st = uv * 2.0 - 1.0;
    vec3 d;
    if      (face == 0u) d = vec3( 1.0, -st.y, -st.x);   // +X
    else if (face == 1u) d = vec3(-1.0, -st.y,  st.x);   // -X
    else if (face == 2u) d = vec3( st.x,  1.0,  st.y);   // +Y
    else if (face == 3u) d = vec3( st.x, -1.0, -st.y);   // -Y
    else if (face == 4u) d = vec3( st.x, -st.y,  1.0);   // +Z
    else                 d = vec3(-st.x, -st.y, -1.0);   // -Z
    return normalize(d);
}

void main() {
    vec2 v = ndcVerts[gl_VertexIndex];
    gl_Position = vec4(v, 0.0, 1.0);
    localPos = uvToDirection(v * 0.5 + 0.5, capture.faceIndex);
}
