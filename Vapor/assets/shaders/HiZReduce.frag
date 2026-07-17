#version 450
// Hi-Z pyramid reduction (Vulkan). One 2:1 max-depth downsample step: reads the
// source mip and writes the max of each 2x2 block into the destination mip.
//
// The scene uses standard [0,1] depth (near=0, far=1, CompareOp::Less), so the
// hierarchical occlusion buffer stores the FARTHEST (max) depth per region; an
// instance is later culled when its nearest depth exceeds this. The reduction
// must never under-estimate the max (that would over-cull visible geometry), so
// odd source dimensions get conservative extra taps to cover the dropped
// row/column. Larger max = fewer culls = always safe.
//
// Level 0 samples the depth texture (mip 0); levels >=1 sample a scratch copy of
// the previous Hi-Z level at srcLod. Both are read as a plain sampler2D (.r).

layout(location = 0) in vec2 tex_uv;   // unused; addressing is by integer texel
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D srcTex;

// Renderer sets the source mip via setFragmentBytes(binding=0) -> offset 64.
layout(push_constant) uniform HiZPC {
    layout(offset = 64) int srcLod;
};

void main() {
    ivec2 srcSize = textureSize(srcTex, srcLod);
    ivec2 base = ivec2(gl_FragCoord.xy) * 2;

    float m = 0.0;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            ivec2 c = min(base + ivec2(dx, dy), srcSize - 1);
            m = max(m, texelFetch(srcTex, c, srcLod).r);
        }
    }

    bool oddX = (srcSize.x & 1) != 0;
    bool oddY = (srcSize.y & 1) != 0;
    if (oddX) {
        m = max(m, texelFetch(srcTex, min(ivec2(base.x + 2, base.y),     srcSize - 1), srcLod).r);
        m = max(m, texelFetch(srcTex, min(ivec2(base.x + 2, base.y + 1), srcSize - 1), srcLod).r);
    }
    if (oddY) {
        m = max(m, texelFetch(srcTex, min(ivec2(base.x,     base.y + 2), srcSize - 1), srcLod).r);
        m = max(m, texelFetch(srcTex, min(ivec2(base.x + 1, base.y + 2), srcSize - 1), srcLod).r);
    }
    if (oddX && oddY) {
        m = max(m, texelFetch(srcTex, min(ivec2(base.x + 2, base.y + 2), srcSize - 1), srcLod).r);
    }

    outColor = vec4(m, 0.0, 0.0, 1.0);
}
