#include <metal_stdlib>
using namespace metal;

// Maximum texture slots for batch rendering
constant int MAX_TEXTURE_SLOTS = 16;

// Batch 2D vertex data
struct Batch2DVertexIn {
    packed_float3 position;
    packed_float4 color;
    packed_float2 uv;
    float texIndex;
    float entityID;
    float _pad;
};

// Uniforms for 2D batch rendering
struct Batch2DUniforms {
    float4x4 projectionMatrix;
};

// Rasterizer output
struct Batch2DVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float texIndex;
    float entityID;
};

// Vertex shader for 2D batch rendering
vertex Batch2DVertexOut batch2d_vertex(
    uint vertexID [[vertex_id]],
    device const Batch2DVertexIn* vertices [[buffer(0)]],
    constant Batch2DUniforms& uniforms [[buffer(1)]]
) {
    Batch2DVertexOut out;

    device const Batch2DVertexIn& vert = vertices[vertexID];
    out.position = uniforms.projectionMatrix * float4(vert.position, 1.0);
    out.color = float4(vert.color);
    out.uv = float2(vert.uv);
    out.texIndex = vert.texIndex;
    out.entityID = vert.entityID;

    return out;
}

// Fragment shader with texture array support
fragment float4 batch2d_fragment(
    Batch2DVertexOut in [[stage_in]],
    array<texture2d<float, access::sample>, MAX_TEXTURE_SLOTS> textures [[texture(0)]]
) {
    constexpr sampler texSampler(coord::normalized,
                                  address::clamp_to_edge,
                                  filter::linear,
                                  mip_filter::linear);

    int texIdx = int(in.texIndex + 0.5); // Round to nearest integer
    texIdx = clamp(texIdx, 0, MAX_TEXTURE_SLOTS - 1);

    float4 texColor = textures[texIdx].sample(texSampler, in.uv);
    return in.color * texColor;
}

// Fragment shader for entity picking (outputs entity ID to render target)
fragment float4 batch2d_fragment_picking(
    Batch2DVertexOut in [[stage_in]],
    array<texture2d<float, access::sample>, MAX_TEXTURE_SLOTS> textures [[texture(0)]]
) {
    constexpr sampler texSampler(coord::normalized,
                                  address::clamp_to_edge,
                                  filter::linear);

    int texIdx = int(in.texIndex + 0.5);
    texIdx = clamp(texIdx, 0, MAX_TEXTURE_SLOTS - 1);

    float4 texColor = textures[texIdx].sample(texSampler, in.uv);

    // Discard fully transparent pixels
    if (texColor.a < 0.01) {
        discard_fragment();
    }

    // Output entity ID encoded as color (for picking)
    // Entity ID is stored as a float, convert to normalized color
    float id = in.entityID;
    float r = fmod(id, 256.0) / 255.0;
    float g = fmod(floor(id / 256.0), 256.0) / 255.0;
    float b = fmod(floor(id / 65536.0), 256.0) / 255.0;

    return float4(r, g, b, 1.0);
}

// Simple fragment shader without texture array (single texture)
fragment float4 batch2d_fragment_simple(
    Batch2DVertexOut in [[stage_in]],
    texture2d<float, access::sample> tex [[texture(0)]]
) {
    constexpr sampler texSampler(coord::normalized,
                                  address::clamp_to_edge,
                                  filter::linear);

    float4 texColor = tex.sample(texSampler, in.uv);
    return in.color * texColor;
}
