#version 450
// Temporal accumulation for the AO chain — GLSL twin of 3d_ao_temporal.metal
// as a fullscreen fragment pass into the RGBA16F history target:
// R = accumulated AO, G = view-space depth, BA = octahedral world normal
// (carried for the à-trous edge stops, which then never touch the full-res
// normal RT). Disocclusion check: reproject the current world position into
// the previous view and compare against the stored depth.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outHistory;

layout(set = 2, binding = 0) uniform sampler2D aoRaw;          // half-res
layout(set = 2, binding = 1) uniform sampler2D historyIn;      // half-res RGBA16F
layout(set = 2, binding = 2) uniform sampler2D velocityTexture;// full-res
layout(set = 2, binding = 3) uniform sampler2D depthTexture;   // full-res
layout(set = 2, binding = 4) uniform sampler2D normalTexture;  // full-res

struct CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float nearPlane;
    float farPlane;
    vec2 _pad;
    vec3 position;
    float _pad2;
    vec4 frustumPlanes[6];
};
layout(std430, set = 1, binding = 3) readonly buffer CameraBuf { CameraData cam; };

// Must match Vapor::AOTemporalRenderData
layout(std430, set = 1, binding = 0) readonly buffer TemporalBuf {
    mat4 prevView;
    uint historyValid;
};

vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(n.yx)) * vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    }
    return e;
}

void main() {
    float ao = texture(aoRaw, tex_uv).r;
    float depth = texture(depthTexture, tex_uv).r;

    // Sky: unoccluded, park the history at far depth
    if (depth >= 0.999999) {
        outHistory = vec4(1.0, -cam.farPlane, 0.0, 0.0);
        return;
    }

    vec3 worldNormal = normalize(texture(normalTexture, tex_uv).xyz);

    // Reconstruct world position (same convention as SSAO.frag / Velocity.frag)
    vec2 uvYUp = vec2(tex_uv.x, 1.0 - tex_uv.y);
    vec4 ndc = vec4(uvYUp * 2.0 - 1.0, depth, 1.0);
    vec4 viewPos = cam.invProj * ndc;
    viewPos /= viewPos.w;
    vec3 worldPos = (cam.invView * viewPos).xyz;
    float viewZ = viewPos.z;

    float blended = ao;
    if (historyValid != 0u) {
        // Velocity.frag stores (currNDC - prevNDC) * 0.5 in y-up UV-scale units;
        // reprojection is prevUV = yUpUV - velocity (its documented contract).
        vec2 vel = texture(velocityTexture, tex_uv).rg;
        vec2 prevUVyUp = uvYUp - vel;
        vec2 prevTexUV = vec2(prevUVyUp.x, 1.0 - prevUVyUp.y);

        bool inBounds = all(equal(prevTexUV, clamp(prevTexUV, 0.0, 1.0)));
        if (inBounds) {
            vec2 hist = texture(historyIn, prevTexUV).rg;  // linear-filtered

            // Where should this surface have been in the previous view?
            float expectedPrevZ = (prevView * vec4(worldPos, 1.0)).z;
            bool depthMatches = abs(hist.g - expectedPrevZ) < 0.05 * max(abs(expectedPrevZ), 1.0);
            if (depthMatches) {
                blended = mix(hist.r, ao, 0.1);
            }
        }
    }
    outHistory = vec4(blended, viewZ, octEncode(worldNormal));
}
