#version 450

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 Color;

void main() {
    Color = vec4(uv, 1.0, 1.0);
}