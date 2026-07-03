#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 model;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    gl_Position = pc.projection * pc.model * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    fragUV = inUV;
}
