#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in flat int fragTexIndex;
layout(location = 3) in flat float fragEntityID;

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) out vec4 outColor;

void main() {
    int idx = clamp(fragTexIndex, 0, 15);
    vec4 texColor = texture(textures[nonuniformEXT(idx)], fragUV);
    outColor = fragColor * texColor;
}
