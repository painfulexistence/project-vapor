#version 450
// Renders the physical atmosphere into a cubemap face (IBL environment source).
// GLSL twin of 3d_sky_capture.metal — same Rayleigh/Mie/Ozone raymarch as the
// on-screen Atmosphere pass, kept HDR for the downstream irradiance/prefilter.

layout(location = 0) in vec3 localPos;
layout(location = 0) out vec4 outColor;

// Must match Vapor::AtmosphereRenderData (std430).
layout(std430, set = 1, binding = 0) readonly buffer AtmoBuf {
    vec3  sunDirection;   float _p1;
    vec3  sunColor;       float _p2;
    float sunIntensity;
    float planetRadius;
    float atmosphereRadius;
    float exposure;
    vec3  rayleighCoefficients; float _p3;
    float rayleighScaleHeight;
    float mieCoefficient;
    float mieScaleHeight;
    float miePreferredDirection;
    vec3  groundColor;    float _p4;
};

const float PI = 3.14159265359;
const vec3  OZONE_ABSORPTION = vec3(3.426, 8.298, 0.356) * 0.06 * 1e-5;
const float OZONE_SCALE_HEIGHT = 8000.0;
const float MIE_EXTINCTION_FACTOR = 1.11;

vec2 raySphere(vec3 ro, vec3 rd, vec3 c, float r) {
    vec3 oc = ro - c;
    float a = dot(rd, rd);
    float b = 2.0 * dot(oc, rd);
    float cc = dot(oc, oc) - r * r;
    float disc = b * b - 4.0 * a * cc;
    if (disc < 0.0) return vec2(-1.0);
    float s = sqrt(disc);
    return vec2((-b - s) / (2.0 * a), (-b + s) / (2.0 * a));
}

float rayleighPhase(float ct) { return 3.0 / (16.0 * PI) * (1.0 + ct * ct); }
float miePhase(float ct, float g) {
    float g2 = g * g;
    return 3.0 * (1.0 - g2) * (1.0 + ct * ct)
         / (8.0 * PI * (2.0 + g2) * pow(1.0 + g2 - 2.0 * g * ct, 1.5));
}

vec3 computeAtmosphere(vec3 ro, vec3 rd, vec3 sunDir) {
    const int PRIMARY = 16;
    const int SECONDARY = 8;
    vec3 planetCenter = vec3(0.0, -planetRadius, 0.0);
    vec2 atmo = raySphere(ro, rd, planetCenter, atmosphereRadius);
    if (atmo.y < 0.0) return vec3(0.0);
    vec2 planet = raySphere(ro, rd, planetCenter, planetRadius);
    float rayStart = max(atmo.x, 0.0);
    float rayEnd = (planet.x > 0.0) ? planet.x : atmo.y;
    float stepSize = (rayEnd - rayStart) / float(PRIMARY);

    vec3 rAccum = vec3(0.0), mAccum = vec3(0.0);
    float rOD = 0.0, mOD = 0.0, oOD = 0.0;
    for (int i = 0; i < PRIMARY; i++) {
        vec3 p = ro + rd * (rayStart + stepSize * (float(i) + 0.5));
        float h = max(length(p - planetCenter) - planetRadius, 0.0);
        float rD = exp(-h / rayleighScaleHeight) * stepSize;
        float mD = exp(-h / mieScaleHeight) * stepSize;
        float oD = exp(-h / OZONE_SCALE_HEIGHT) * stepSize;
        rOD += rD; mOD += mD; oOD += oD;
        vec2 sh = raySphere(p, sunDir, planetCenter, atmosphereRadius);
        if (sh.y > 0.0) {
            float ss = sh.y / float(SECONDARY);
            float rODL = 0.0, mODL = 0.0, oODL = 0.0;
            for (int j = 0; j < SECONDARY; j++) {
                vec3 lp = p + sunDir * ss * (float(j) + 0.5);
                float lh = length(lp - planetCenter) - planetRadius;
                if (lh < 0.0) { rODL = 1e10; break; }
                rODL += exp(-lh / rayleighScaleHeight) * ss;
                mODL += exp(-lh / mieScaleHeight) * ss;
                oODL += exp(-lh / OZONE_SCALE_HEIGHT) * ss;
            }
            vec3 att = exp(-rayleighCoefficients * (rOD + rODL)
                           - mieCoefficient * MIE_EXTINCTION_FACTOR * (mOD + mODL)
                           - OZONE_ABSORPTION * (oOD + oODL));
            rAccum += rD * att;
            mAccum += mD * att;
        }
    }
    float ct = dot(rd, sunDir);
    vec3 rayleigh = rAccum * rayleighCoefficients * rayleighPhase(ct);
    vec3 mie = mAccum * mieCoefficient * miePhase(ct, miePreferredDirection);
    return sunIntensity * sunColor * (rayleigh + mie);
}

void main() {
    vec3 rd = normalize(localPos);
    vec3 ro = vec3(0.0, 1.0, 0.0);
    vec3 sunDir = normalize(sunDirection);
    vec3 color = computeAtmosphere(ro, rd, sunDir);
    color = 1.0 - exp(-exposure * color);           // exposure, keep HDR-ish
    float sunDisk = smoothstep(0.9995, 0.9999, dot(rd, sunDir));
    color += sunColor * sunIntensity * sunDisk * 0.5;
    outColor = vec4(color, 1.0);
}
