#version 450
// Diffuse irradiance convolution: cosine-weighted hemisphere integral of the
// environment cubemap. GLSL twin of 3d_irradiance_convolution.metal.

layout(location = 0) in vec3 localPos;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform samplerCube environmentMap;

const float PI = 3.14159265359;

void main() {
    vec3 N = normalize(localPos);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    vec3 irradiance = vec3(0.0);
    int samples = 0;
    const float d = 0.025;
    for (float phi = 0.0; phi < 2.0 * PI; phi += d) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += d) {
            vec3 t = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = t.x * right + t.y * up + t.z * N;
            irradiance += texture(environmentMap, sampleVec).rgb * cos(theta) * sin(theta);
            samples++;
        }
    }
    irradiance = PI * irradiance / float(samples);
    outColor = vec4(irradiance, 1.0);
}
