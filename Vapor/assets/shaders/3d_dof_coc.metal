#include <metal_stdlib>
using namespace metal;

// Tilt-Shift DOF - Circle of Confusion calculation
// Creates Octopath Traveler style miniature/diorama effect

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

struct DOFParams {
    float focusCenter;      // Y position of focus band center (0-1, default 0.5)
    float focusWidth;       // Width of the in-focus band (default 0.15)
    float focusFalloff;     // How quickly blur increases outside focus (default 2.0)
    float maxBlur;          // Maximum blur radius (default 1.0)
    float tiltAngle;        // Tilt angle in radians (0 = horizontal band)
    float bokehRoundness;   // Bokeh shape: 0 = hexagonal, 1 = circular
    float padding1;
    float padding2;
};

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]]) {
    RasterizerData vert;
    vert.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    vert.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    vert.uv.y = 1.0 - vert.uv.y;
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texColor [[texture(0)]],
    texture2d<float, access::sample> texDepth [[texture(1)]],
    constant DOFParams& params [[buffer(0)]]
) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);

    float3 color = texColor.sample(s, in.uv).rgb;
    float depth = texDepth.sample(s, in.uv).r;

    // Calculate tilt-shift focus position
    // Apply tilt angle to create angled focus band
    float2 center = float2(0.5, params.focusCenter);
    float2 toPixel = in.uv - center;

    // Rotate by tilt angle
    float cosAngle = cos(params.tiltAngle);
    float sinAngle = sin(params.tiltAngle);
    float rotatedY = -toPixel.x * sinAngle + toPixel.y * cosAngle;

    // Calculate distance from focus band
    float distFromFocus = abs(rotatedY);

    // Smooth transition from focus to blur
    float focusEdge = params.focusWidth * 0.5;
    float blurAmount = smoothstep(focusEdge, focusEdge + params.focusFalloff * 0.5, distFromFocus);

    // Apply power curve for more dramatic falloff
    blurAmount = pow(blurAmount, 1.5);

    // Scale to max blur
    float coc = blurAmount * params.maxBlur;

    // Optional: mix with depth-based DOF for hybrid effect
    // float depthBlur = ... calculate from depth ...
    // coc = mix(coc, depthBlur, depthInfluence);

    // Output: RGB = color, A = CoC (circle of confusion)
    return float4(color, coc);
}
