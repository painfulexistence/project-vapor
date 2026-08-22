#version 450
// Grass ring fragment shader (Vulkan). Stylized root->tip gradient lit by the
// sun with a soft wrap term (matching the original demo's look, not the full
// PBR path — grass is a carpet, not a surface). Root darkening grounds blades
// in the terrain's grass detail layer. Outputs LINEAR HDR into the RGBA16F
// colorRT; PostProcess tonemaps later. Binding contract mirrored in
// 3d_grass.metal: set1 b0 = the same GrassParams block as the vertex stage.

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vHeightFrac;
layout(location = 3) in float vTint;

layout(location = 0) out vec4 outColor;

layout(std430, set = 1, binding = 0) readonly buffer ParamsBuf {
    mat4 viewProj;
    vec4 cameraPosTime;
    vec4 wind;
    vec4 rootColor;  // rgb root, w fadeStart
    vec4 tipColor;   // rgb tip, w fadeEnd
    vec4 sun;        // xyz TOWARD sun, w intensity
    vec4 sunColor;   // rgb
};

void main() {
    // Root->tip gradient with per-blade jitter; colors are authored in display
    // space — linearize (pow 2.2, same convention as the terrain layers).
    vec3 c = mix(pow(rootColor.rgb, vec3(2.2)), pow(tipColor.rgb, vec3(2.2)),
                 smoothstep(0.0, 1.0, vHeightFrac));
    c *= 0.85 + 0.3 * vTint;

    // Soft wrap diffuse: fields never go pitch black at grazing sun. Sun
    // intensity is normalized into a bounded stylized range so the atmosphere
    // panel's physical intensities don't blow the grass out.
    float ndl = max(dot(normalize(vNormal), normalize(sun.xyz)), 0.0);
    float wrap = 0.35 + 0.65 * ndl;
    float sunScale = clamp(sun.w * 0.12, 0.15, 1.2);
    vec3 lit = c * sunColor.rgb * (wrap * sunScale);

    // Root ambient occlusion: blades shadow their own bases.
    lit *= 0.55 + 0.45 * vHeightFrac;

    outColor = vec4(lit, 1.0);
}
