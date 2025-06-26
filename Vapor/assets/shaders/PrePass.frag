#version 450

layout(location = 0) in vec2 tex_uv;

layout(set = 1, binding = 0) uniform sampler2D texAlbedo;

void main() {
	vec4 baseColor = texture(texAlbedo, tex_uv);
	if (baseColor.a < 0.5) {
		discard;
	}
}