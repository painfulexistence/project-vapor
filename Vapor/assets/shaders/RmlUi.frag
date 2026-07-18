#version 450
// RmlUI geometry fragment (RHI renderer, Vulkan backend). Texture lives in the
// RHI's fragment-texture set (set 2), bound via RHI::setTexture(0, 0, ...).

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(set = 2, binding = 0) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = fragColor * texture(texSampler, fragUV);
}
