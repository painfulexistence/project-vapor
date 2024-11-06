#version 450

layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 tex_uv;

void main() {
	tex_uv = uv;
	gl_Position = vec4(pos, 1.0);
}