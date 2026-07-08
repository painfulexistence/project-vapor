#version 450
// Fullscreen triangle for the sky/atmosphere pass. Emits z = 1.0 (far plane in
// Vulkan ZO) so a depth test of LessOrEqual against the scene depth only lets
// the sky through where no geometry was drawn (depth still == 1.0).

layout(location = 0) out vec2 ndcOut;

const vec2 ndcVerts[3] = vec2[3](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main() {
    ndcOut = ndcVerts[gl_VertexIndex];
    gl_Position = vec4(ndcVerts[gl_VertexIndex], 1.0, 1.0);  // z = 1.0 far plane
}
