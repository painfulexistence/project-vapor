#version 450

layout(location = 0) out vec2 tex_uv;

const vec2 ndcVerts[3] = {
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
};

void main() {
    tex_uv = ndcVerts[gl_VertexIndex] * 0.5 + 0.5;
    tex_uv.y = 1.0 - tex_uv.y; // flip Y axis
    gl_Position = vec4(ndcVerts[gl_VertexIndex], 0.0, 1.0);
}