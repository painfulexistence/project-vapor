#version 450
// RHI batch renderer fragment shader.
// One texture per batch (white texture for untextured quads); the batch is
// flushed whenever the texture changes. See BatchRenderer::setTexture.

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(set = 2, binding = 0) uniform sampler2D batchTexture;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = fragColor * texture(batchTexture, fragUV);
}
