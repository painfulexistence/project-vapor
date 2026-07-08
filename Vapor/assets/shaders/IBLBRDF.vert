#version 450
// BRDF integration LUT (Vulkan) — fullscreen triangle. UV.x = NdotV, UV.y = roughness.

layout(location = 0) out vec2 uv;

const vec2 ndcVerts[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

void main() {
    vec2 v = ndcVerts[gl_VertexIndex];
    gl_Position = vec4(v, 0.0, 1.0);
    uv = v * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
}
