#version 450

layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 0) out vec2 tex_uv;
layout(location = 1) out vec3 frag_pos;
layout(location = 2) out vec3 frag_normal;
layout(binding = 0) uniform UBO {
    mat4 model;
    mat4 view;
    mat4 proj;
};

void main() {
	tex_uv = uv;
    frag_pos = vec3(model * vec4(pos, 1.0));
    frag_normal = normalize(vec3(model * vec4(normal, 0.0)));
	gl_Position = proj * view * model * vec4(pos, 1.0);
}