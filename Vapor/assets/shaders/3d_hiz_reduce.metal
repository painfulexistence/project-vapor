#include <metal_stdlib>
using namespace metal;

// Hi-Z pyramid reduction (Metal). Mirrors HiZReduce.frag: one 2:1 max-depth
// downsample step. Standard [0,1] depth, so store the FARTHEST (max) depth per
// 2x2 block; odd source dims get conservative extra taps so the max is never
// under-estimated (which would over-cull). Reads the depth RT (level 0) or a
// scratch copy of the previous Hi-Z level (levels >=1) as texture2d<float>.

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]]) {
    RasterizerData v;
    v.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    v.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    v.uv.y = 1.0 - v.uv.y;
    return v;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> srcTex [[texture(0)]],
    constant uint& srcLod [[buffer(0)]]
) {
    uint w = srcTex.get_width(srcLod);
    uint h = srcTex.get_height(srcLod);
    uint2 hi = uint2(w - 1u, h - 1u);
    uint2 base = uint2(in.position.xy) * 2u;

    float m = 0.0;
    for (uint dy = 0u; dy < 2u; ++dy) {
        for (uint dx = 0u; dx < 2u; ++dx) {
            m = max(m, srcTex.read(min(base + uint2(dx, dy), hi), srcLod).r);
        }
    }

    bool oddX = (w & 1u) != 0u;
    bool oddY = (h & 1u) != 0u;
    if (oddX) {
        m = max(m, srcTex.read(min(uint2(base.x + 2u, base.y),      hi), srcLod).r);
        m = max(m, srcTex.read(min(uint2(base.x + 2u, base.y + 1u), hi), srcLod).r);
    }
    if (oddY) {
        m = max(m, srcTex.read(min(uint2(base.x,      base.y + 2u), hi), srcLod).r);
        m = max(m, srcTex.read(min(uint2(base.x + 1u, base.y + 2u), hi), srcLod).r);
    }
    if (oddX && oddY) {
        m = max(m, srcTex.read(min(uint2(base.x + 2u, base.y + 2u), hi), srcLod).r);
    }

    return float4(m, 0.0, 0.0, 1.0);
}
