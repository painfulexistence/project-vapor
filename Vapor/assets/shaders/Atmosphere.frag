#version 450
// Physically-based sky: Rayleigh + Mie + Ozone atmospheric scattering.
// GLSL port of the Metal 3d_atmosphere.metal so both backends match.
// Rendered as a fullscreen pass into the HDR colorRT, depth-tested so it only
// fills background pixels (see Sky.vert / the LessOrEqual depth state).

layout(location = 0) in vec2 ndcOut;   // clip-space XY of this pixel
layout(location = 0) out vec4 outColor;

// Must match Vapor::CameraRenderData (same layout as RHIMain.frag CameraData).
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

// Must match Vapor::AtmosphereData (std430).
struct AtmosphereData {
    vec3 sunDirection;
    float _pad1;
    vec3 sunColor;
    float _pad2;
    float sunIntensity;
    float planetRadius;
    float atmosphereRadius;
    float exposure;
    vec3 rayleighCoefficients;
    float _pad3;
    float rayleighScaleHeight;
    float mieCoefficient;
    float mieScaleHeight;
    float miePreferredDirection;
    vec3 groundColor;
    float _pad4;
};

layout(std430, set = 1, binding = 3) readonly buffer CameraBuf { CameraData cam; };
layout(std430, set = 1, binding = 0) readonly buffer AtmosphereBuf { AtmosphereData atmo; };

const float PI = 3.14159265359;
const vec3  OZONE_ABSORPTION = vec3(3.426, 8.298, 0.356) * 0.06 * 1e-5;
const float OZONE_SCALE_HEIGHT = 8000.0;
const float MIE_EXTINCTION_FACTOR = 1.11;

vec2 raySphereIntersect(vec3 ro, vec3 rd, vec3 c, float r) {
    vec3 oc = ro - c;
    float a = dot(rd, rd);
    float b = 2.0 * dot(oc, rd);
    float cc = dot(oc, oc) - r * r;
    float disc = b * b - 4.0 * a * cc;
    if (disc < 0.0) return vec2(-1.0);
    float s = sqrt(disc);
    return vec2((-b - s) / (2.0 * a), (-b + s) / (2.0 * a));
}

float rayleighPhase(float c) { return 3.0 / (16.0 * PI) * (1.0 + c * c); }

float miePhase(float c, float g) {
    float g2 = g * g;
    float num = 3.0 * (1.0 - g2) * (1.0 + c * c);
    float den = 8.0 * PI * (2.0 + g2) * pow(1.0 + g2 - 2.0 * g * c, 1.5);
    return num / den;
}

vec3 computeAtmosphere(vec3 ro, vec3 rd, vec3 sunDir) {
    const int PRIMARY_STEPS = 16;
    const int SECONDARY_STEPS = 4;
    vec3 planetCenter = vec3(0.0, -atmo.planetRadius, 0.0);

    vec2 atmoHit = raySphereIntersect(ro, rd, planetCenter, atmo.atmosphereRadius);
    if (atmoHit.y < 0.0) return vec3(0.0);
    vec2 planetHit = raySphereIntersect(ro, rd, planetCenter, atmo.planetRadius);

    float rayStart = max(atmoHit.x, 0.0);
    float rayEnd = atmoHit.y;
    if (planetHit.x > 0.0) rayEnd = planetHit.x;

    float stepSize = (rayEnd - rayStart) / float(PRIMARY_STEPS);

    vec3 rayleighAccum = vec3(0.0);
    vec3 mieAccum = vec3(0.0);
    float rOD = 0.0, mOD = 0.0, oOD = 0.0;

    for (int i = 0; i < PRIMARY_STEPS; i++) {
        vec3 sp = ro + rd * (rayStart + stepSize * (float(i) + 0.5));
        // Clamp to the surface: samples below it (camera under the ground
        // plane, or precision dips on grazing rays) make exp(-h/H) explode to
        // Inf, and Inf * exp(-Inf) = NaN. Metal's fast-math flushed this; the
        // SPIR-V path keeps IEEE semantics and rendered a giant NaN-black disc
        // that also ate every alpha-blended pass on top (NaN * anything = NaN).
        float h = max(length(sp - planetCenter) - atmo.planetRadius, 0.0);

        float rD = exp(-h / atmo.rayleighScaleHeight) * stepSize;
        float mD = exp(-h / atmo.mieScaleHeight) * stepSize;
        float oD = exp(-h / OZONE_SCALE_HEIGHT) * stepSize;
        rOD += rD; mOD += mD; oOD += oD;

        vec2 sunHit = raySphereIntersect(sp, sunDir, planetCenter, atmo.atmosphereRadius);
        if (sunHit.y > 0.0) {
            float sunStep = sunHit.y / float(SECONDARY_STEPS);
            float rODl = 0.0, mODl = 0.0, oODl = 0.0;
            for (int j = 0; j < SECONDARY_STEPS; j++) {
                vec3 lp = sp + sunDir * sunStep * (float(j) + 0.5);
                float lh = length(lp - planetCenter) - atmo.planetRadius;
                if (lh < 0.0) { rODl = 1e10; break; }
                rODl += exp(-lh / atmo.rayleighScaleHeight) * sunStep;
                mODl += exp(-lh / atmo.mieScaleHeight) * sunStep;
                oODl += exp(-lh / OZONE_SCALE_HEIGHT) * sunStep;
            }
            vec3 att = exp(
                -atmo.rayleighCoefficients * (rOD + rODl)
                - atmo.mieCoefficient * MIE_EXTINCTION_FACTOR * (mOD + mODl)
                - OZONE_ABSORPTION * (oOD + oODl));
            rayleighAccum += rD * att;
            mieAccum += mD * att;
        }
    }

    float cosTheta = dot(rd, sunDir);
    vec3 rayleigh = rayleighAccum * atmo.rayleighCoefficients * rayleighPhase(cosTheta);
    vec3 mie = mieAccum * atmo.mieCoefficient * miePhase(cosTheta, atmo.miePreferredDirection);
    return atmo.sunIntensity * atmo.sunColor * (rayleigh + mie);
}

// --- Night sky: hash-based star field + a simple moon (Atmosphere mode) ---
float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
// One jittered point star per sparse cell of the view-direction lattice.
float starField(vec3 dir) {
    vec3 p = dir * 300.0;               // density (higher = more, smaller stars)
    vec3 cell = floor(p);
    vec3 f = fract(p);
    float h = hash31(cell);
    if (h < 0.972) return 0.0;          // ~2.8% of cells hold a star
    vec3 j = vec3(hash31(cell + 1.3), hash31(cell + 2.7), hash31(cell + 4.1));
    float d = length(f - j);
    return smoothstep(0.12, 0.0, d) * (0.4 + 0.6 * hash31(cell + 5.9));
}

void main() {
    // Reconstruct the world-space view ray for this pixel.
    vec4 clip = vec4(ndcOut, 1.0, 1.0);
    vec4 viewPos = cam.invProj * clip;
    viewPos /= viewPos.w;
    vec3 rayDir = normalize((cam.invView * vec4(viewPos.xyz, 0.0)).xyz);
    vec3 rayOrigin = cam.position;

    vec3 sunDir = normalize(atmo.sunDirection);

    // Below the horizon -> simple lit ground color. Also taken when the
    // camera itself is under the ground plane (inside the planet sphere:
    // planetHit.x < 0 < planetHit.y) — marching the atmosphere from inside
    // the planet produces NaNs on the IEEE-strict Vulkan path.
    vec3 planetCenter = vec3(0.0, -atmo.planetRadius, 0.0);
    vec2 planetHit = raySphereIntersect(rayOrigin, rayDir, planetCenter, atmo.planetRadius);
    if (planetHit.x > 0.0 || (planetHit.x < 0.0 && planetHit.y > 0.0)) {
        float sunDot = max(0.0, dot(vec3(0.0, 1.0, 0.0), sunDir));
        vec3 g = atmo.groundColor * atmo.sunColor * atmo.sunIntensity * sunDot * 0.1;
        g = 1.0 - exp(-atmo.exposure * g);
        outColor = vec4(g, 1.0);
        return;
    }

    vec3 color = computeAtmosphere(rayOrigin, rayDir, sunDir);
    color = 1.0 - exp(-atmo.exposure * color);

    // Sun disk.
    float sd = dot(rayDir, sunDir);
    float sunDisk = smoothstep(0.9995, 0.9999, sd);
    color += atmo.sunColor * atmo.sunIntensity * sunDisk * 0.5;

    // Night sky: stars + moon fade in as the sun drops below the horizon. The
    // moon sits opposite the sun (rises as the sun sets), so it lights the night.
    float night = smoothstep(0.06, -0.06, sunDir.y);
    if (night > 0.0 && rayDir.y > -0.05) {
        vec3 moonDir = normalize(-sunDir);
        float moonUp = smoothstep(-0.05, 0.08, moonDir.y);
        float horizonFade = smoothstep(-0.02, 0.15, rayDir.y);
        color += vec3(0.9, 0.92, 1.0) * starField(rayDir) * night * horizonFade;
        float md = dot(rayDir, moonDir);
        float moonDisk = smoothstep(0.9990, 0.9995, md);
        float halo = smoothstep(0.985, 1.0, md);
        color += vec3(0.92, 0.93, 1.0) * (moonDisk * 1.2 + halo * halo * 0.15) * night * moonUp;
    }

    outColor = vec4(color, 1.0);
}
