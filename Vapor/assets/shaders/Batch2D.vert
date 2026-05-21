#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inTexIndex;
layout(location = 4) in float inEntityID;

layout(set = 0, binding = 0) uniform Batch2DUniforms {
    mat4 projectionMatrix;
} uniforms;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out flat int fragTexIndex;
layout(location = 3) out flat float fragEntityID;

void main() {
    gl_Position = uniforms.projectionMatrix * vec4(inPosition, 1.0);
    fragColor    = inColor;
    fragUV       = inUV;
    fragTexIndex = int(inTexIndex + 0.5);
    fragEntityID = inEntityID;
}
