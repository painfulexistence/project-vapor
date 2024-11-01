#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec3 fragColor;

vec2 positions[6] = vec2[](
    vec2(0.0f, -1.0f),
    vec2(1.0f, 1.0f),
    vec2(-1.0f, 1.0f),
    vec2(0.5f, 0.0f),
    vec2(0.0f, 1.0f),
    vec2(-0.5f, 0.0f)
);

vec3 colors[6] = vec3[](
    vec3(0.75f, 0.75f, 0.0f),
    vec3(0.5f, 0.5f, 0.0f),
    vec3(1.0f, 1.0f, 0.0f),
    vec3(0.0f, 0.0f, 0.0f),
    vec3(0.0f, 0.0f, 0.0f),
    vec3(0.0f, 0.0f, 0.0f)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex] / 2.0f, 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}
