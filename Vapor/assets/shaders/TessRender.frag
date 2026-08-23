#version 460
// Tess debug shading — GLSL twin of tessFragmentMain: hash color per
// subdivision depth (the LoD visualization), simple lambert + ambient.
#extension GL_GOOGLE_include_directive : require
#include "include/tess_common.glsl"

layout(location = 0) in vec3 inWorldNormal;
layout(location = 1) in vec3 inWorldPosition;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in uint inDepth;
layout(location = 4) flat in uint inNode;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 base = tessHashColor(inDepth * 2654435761u);
    vec3 lightDir = normalize(vec3(0.4, 1.0, 0.3));
    float ndl = max(dot(normalize(inWorldNormal), lightDir), 0.0);
    vec3 color = base * (0.25 + 0.75 * ndl);
    outColor = vec4(color, 1.0);
}
