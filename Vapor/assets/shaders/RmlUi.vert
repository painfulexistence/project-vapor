#version 450
// RmlUI geometry (RHI renderer, Vulkan backend). The full projection *
// transform * translation matrix is premultiplied on the CPU and pushed as a
// single mat4 — it fits the RHI's vertex push-constant range [0, 64) exactly
// (RHI::setVertexBytes(&mvp, 64, /*binding=*/0)).

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;   // RGBA8_UNORM attribute, normalized
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform PushConstants {
    layout(offset = 0) mat4 mvp;
};

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    gl_Position = mvp * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    fragUV = inUV;
}
