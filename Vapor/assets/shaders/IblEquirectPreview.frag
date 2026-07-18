#version 450
// IBL debug: unwrap the environment cubemap into an equirectangular 2D image so
// it can be shown in ImGui (a cubemap's own view can't go into ImGui::Image).
// Uses FullScreen.vert (tex_uv in [0,1]).

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform samplerCube envCubemap;

const float PI = 3.14159265359;

void main() {
    float phi   = (tex_uv.x * 2.0 - 1.0) * PI;   // longitude, -PI..PI
    float theta = tex_uv.y * PI;                  // latitude, 0..PI (0 = +Y up)
    vec3 dir = vec3(sin(theta) * sin(phi), cos(theta), -sin(theta) * cos(phi));
    outColor = vec4(texture(envCubemap, dir).rgb, 1.0);
}
