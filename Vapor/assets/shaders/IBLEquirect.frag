#version 450
// Equirectangular HDRI -> cubemap face (Vulkan). Samples the equirect map along
// the per-pixel world direction produced by IBLCubeFace.vert.

layout(location = 0) in vec3 localPos;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D equirectMap;

const float PI = 3.14159265359;
const vec2 INV_ATAN = vec2(0.1591, 0.3183); // (1/2pi, 1/pi)

void main() {
    vec3 dir = normalize(localPos);
    vec2 uv = vec2(atan(dir.z, dir.x), asin(clamp(dir.y, -1.0, 1.0)));
    uv *= INV_ATAN;
    uv += 0.5;
    uv.y = 1.0 - uv.y; // image top = sky (matches the MSL convention)
    outColor = vec4(texture(equirectMap, uv).rgb, 1.0);
}
