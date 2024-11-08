#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec3 tangent;
layout(location = 4) in vec3 bitangent;
layout(location = 0) out vec3 frag_pos;
layout(location = 1) out vec2 tex_uv;
layout(location = 2) out mat3 TBN;
layout(binding = 0) uniform UBO {
    mat4 model;
    mat4 view;
    mat4 proj;
};

void main()
{
    vec3 T = normalize(vec3(model * vec4(tangent, 0.0)));
    vec3 B = normalize(vec3(model * vec4(bitangent, 0.0)));
    vec3 N = normalize(vec3(model * vec4(normal, 0.0)));
    frag_pos = vec3(model * vec4(position, 1.0));
    tex_uv = uv;
    TBN = mat3(T, B, N);

    gl_Position = proj * view * vec4(frag_pos, 1.0);
}