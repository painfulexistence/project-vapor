#version 450
// RHI batch renderer fragment shader — vertex color only.
// (The RHI batch path does not bind textures yet; see BatchRenderer::flush.)

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = fragColor;
}
