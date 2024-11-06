#version 450

layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 tex_uv;
layout(binding = 0) uniform UBO {
    mat4 model;
    mat4 view;
    mat4 proj;
};

void main() {
	tex_uv = uv;
	gl_Position = proj * view * model * vec4(pos, 1.0);
}