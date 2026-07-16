#version 450
// Fullscreen triangle for the BRDF integration LUT. x = NdotV, y = roughness.
// The engine's negative-height viewport matches the GL/Metal NDC convention, so
// the UV orientation lines up with the Metal bake (which flips Y in-shader).

layout(location = 0) out vec2 uv;

vec2 ndcVerts[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

void main() {
    vec2 v = ndcVerts[gl_VertexIndex];
    gl_Position = vec4(v, 0.0, 1.0);
    uv = v * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
}
