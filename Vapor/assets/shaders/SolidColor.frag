#version 450
// #extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec3 fragColor;

layout(location = 0) out vec4 Color;

void main() {
    Color = vec4(fragColor, 1.0);
}