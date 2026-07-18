#version 450
// MicroVoxel GI composite. GLSL twin of the fragment in 3d_microvoxel_gi.metal.
//
// Fullscreen: passes the scene color through, adding albedo * gi * ao on
// voxel pixels (the GI stores incident radiance; albedo/AO come from the
// voxel G-buffer, so temporal accumulation blurred lighting, not texture).
// Ping-pongs colorRT -> tempColorRT like the fog pass; the renderer swaps.
//
// Debug: giParams.y >= 0 splits the screen at that x — left shows the RAW
// temporal accumulation, right the denoised result, with a thin divider.
// Debug view 5 (params.y of the volume data) replaces color with the GI
// buffer; that flag arrives via giParams' debug slot here.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D hitTTex;
layout(set = 2, binding = 2) uniform sampler2D albedoAOTex;
layout(set = 2, binding = 3) uniform sampler2D giDenoisedTex;  // half-res, bilinear
layout(set = 2, binding = 4) uniform sampler2D giRawTex;       // half-res, bilinear

// Must match Vapor::MicroVoxelGIRenderData.
layout(std430, set = 1, binding = 0) readonly buffer GIParamsBuf {
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 giCameraPosition;   // xyz; w = frameIndex
    vec4 prevCameraPosition; // xyz; w = temporal blend
    vec4 giParams;           // x = giStrength, y = splitX, z = giWidth, w = giHeight
    vec4 giSigmas;           // x = depth, y = normal, z = luma, w = debugModeGI (1 = show GI buffer)
};

void main() {
    vec4 color = texture(sceneColor, tex_uv);
    float hitT = texture(hitTTex, tex_uv).r;
    if (hitT <= 0.0) {
        outColor = color;
        return;
    }

    // Raw|denoised split compare (dragged from the app): left of the split
    // samples the un-denoised accumulation.
    vec3 gi;
    float splitX = giParams.y;
    if (splitX >= 0.0 && tex_uv.x < splitX) {
        gi = texture(giRawTex, tex_uv).rgb;
    } else {
        gi = texture(giDenoisedTex, tex_uv).rgb;
    }

    vec4 albedoAO = texture(albedoAOTex, tex_uv);
    vec3 indirect = albedoAO.rgb * gi * albedoAO.a * giParams.x;

    vec3 result = color.rgb + indirect;
    if (giSigmas.w > 0.5) result = gi;  // debug view: GI buffer in isolation
    if (splitX >= 0.0 && abs(tex_uv.x - splitX) < 0.0012) result = vec3(1.0);  // divider

    outColor = vec4(result, color.a);
}
