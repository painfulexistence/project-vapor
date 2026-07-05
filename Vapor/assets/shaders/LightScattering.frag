#version 450
// Screen-space light scattering (god rays). GLSL port of the Metal
// 3d_light_scattering.metal: radial march from each pixel toward the sun's
// screen position, accumulating luminance of unoccluded (sky) samples.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D colorTexture;
layout(set = 2, binding = 1) uniform sampler2D depthTexture;

// Must match Vapor::LightScatteringRenderData (std430).
layout(std430, set = 1, binding = 0) readonly buffer LSBuf {
    vec2 sunScreenPos;
    vec2 screenSize;
    float density;
    float weight;
    float decay;
    float exposure;
    uint numSamples;
    float maxDistance;
    float sunIntensity;
    float mieG;
    vec3 sunColor;
    float _pad1;
    float depthThreshold;
    float jitter;
    vec2 _pad2;
};

// Frame counter for temporal jitter (RHI::setFragmentBytes binding 7 -> offset 112).
layout(push_constant) uniform PushConstants { layout(offset = 112) uint frameNumber; };

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec2 uv = tex_uv;
    vec2 sunPos = sunScreenPos;

    float margin = 0.3;
    if (sunPos.x < -margin || sunPos.x > 1.0 + margin ||
        sunPos.y < -margin || sunPos.y > 1.0 + margin) {
        outColor = vec4(0.0); return;
    }

    // Sun occluded by geometry? (sky depth is at the far plane)
    vec2 sunUV = clamp(sunPos, vec2(0.001), vec2(0.999));
    float sunDepth = texture(depthTexture, sunUV).r;
    if (step(depthThreshold, sunDepth) < 0.01) { outColor = vec4(0.0); return; }

    vec2 deltaUV = sunPos - uv;
    float distToSun = length(deltaUV);
    if (distToSun < 0.001) { outColor = vec4(0.0); return; }

    vec2 rayDir = deltaUV / distToSun;
    float stepLen = min(distToSun, maxDistance) / float(numSamples);
    vec2 stepDelta = rayDir * stepLen;

    float jitterAmount = jitter * hash(uv * screenSize + vec2(float(frameNumber)));
    vec2 sampleUV = uv + stepDelta * jitterAmount;

    vec3 accumLight = vec3(0.0);
    float illuminationDecay = 1.0;
    for (uint i = 0u; i < numSamples; i++) {
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) break;
        float sampleDepth = texture(depthTexture, sampleUV).r;
        float isUnoccluded = step(depthThreshold, sampleDepth);
        vec3 sampleColor = texture(colorTexture, sampleUV).rgb;
        float luminance = dot(sampleColor, vec3(0.2126, 0.7152, 0.0722));
        accumLight += sunColor * luminance * isUnoccluded * illuminationDecay * weight;
        illuminationDecay *= decay;
        sampleUV += stepDelta;
    }

    vec3 godRays = accumLight * density * exposure * sunIntensity;
    float falloff = 1.0 - clamp(distToSun * 0.8, 0.0, 1.0);
    falloff = falloff * falloff;
    godRays *= falloff;

    outColor = vec4(godRays, 1.0);
}
