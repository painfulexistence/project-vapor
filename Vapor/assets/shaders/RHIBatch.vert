#version 450
// RHI batch renderer (2D/3D quads, text, debug shapes) — Vulkan backend.
// View-projection arrives via push constants:
//   RHI::setVertexBytes(&viewProj, 64, /*binding=*/0) -> offset (0%4)*16 = 0

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inTexIndex;
layout(location = 4) in float inEntityID;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
};

void main() {
    gl_Position = viewProj * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragUV = inUV;
}
