#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// ============================================================================
// Sun Flare / Lens Flare - Procedural Implementation
// ============================================================================
// Simulates camera lens optical artifacts:
// - Main glow around sun
// - Halo ring
// - Ghost sprites (mirrored across screen center)
// - Anamorphic streak (horizontal light streak)
// - Starburst pattern
// All elements use procedural textures for maximum flexibility

// ============================================================================
// Data Structures
// ============================================================================

struct SunFlareData {
    float2 sunScreenPos;            // Sun position in screen space [0,1]
    float2 screenSize;              // Screen dimensions
    float2 screenCenter;            // Screen center (0.5, 0.5)
    float2 aspectRatio;             // (1.0, height/width) for aspect correction

    float sunIntensity;             // Overall intensity
    float visibility;               // Sun visibility (0-1, from occlusion test)
    float fadeEdge;                 // Fade when sun near screen edge
    float _pad1;

    float3 sunColor;                // Sun tint color
    float _pad2;

    // Main glow parameters
    float glowIntensity;            // Central glow brightness
    float glowFalloff;              // Glow falloff exponent
    float glowSize;                 // Glow radius
    float _pad3;

    // Halo parameters
    float haloIntensity;            // Halo brightness
    float haloRadius;               // Halo ring radius
    float haloWidth;                // Halo ring width
    float haloFalloff;              // Halo edge softness

    // Ghost parameters
    int ghostCount;                 // Number of ghost sprites (4-8)
    float ghostSpacing;             // Spacing multiplier along axis
    float ghostIntensity;           // Ghost brightness
    float ghostSize;                // Base ghost size

    float ghostChromaticOffset;     // Chromatic aberration for ghosts
    float ghostFalloff;             // Ghost edge falloff
    float2 _pad4;

    // Streak parameters
    float streakIntensity;          // Anamorphic streak brightness
    float streakLength;             // Streak length
    float streakFalloff;            // Streak vertical falloff
    float _pad5;

    // Starburst parameters
    float starburstIntensity;       // Starburst brightness
    float starburstSize;            // Starburst radius
    int starburstPoints;            // Number of rays
    float starburstRotation;        // Rotation angle

    // Dirt/dust parameters
    float dirtIntensity;            // Lens dirt brightness
    float dirtScale;                // Dirt texture scale
    float2 _pad6;

    // Animation
    float time;                     // For subtle animation
    float3 _pad7;
};

// ============================================================================
// Procedural Texture Functions
// ============================================================================

// Simple hash for noise
float flareHash(float2 p) {
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float flareNoise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float2 u = f * f * (3.0 - 2.0 * f);

    return mix(mix(flareHash(i + float2(0.0, 0.0)),
                   flareHash(i + float2(1.0, 0.0)), u.x),
               mix(flareHash(i + float2(0.0, 1.0)),
                   flareHash(i + float2(1.0, 1.0)), u.x), u.y);
}

// Procedural circular gradient (soft circle)
float proceduralCircle(float2 uv, float2 center, float radius, float softness) {
    float dist = length(uv - center);
    return 1.0 - smoothstep(radius * (1.0 - softness), radius, dist);
}

// Procedural ring/halo
float proceduralRing(float2 uv, float2 center, float radius, float width, float softness) {
    float dist = length(uv - center);
    float inner = smoothstep(radius - width * 0.5 - softness, radius - width * 0.5, dist);
    float outer = 1.0 - smoothstep(radius + width * 0.5, radius + width * 0.5 + softness, dist);
    return inner * outer;
}

// Procedural hexagonal bokeh shape
float proceduralHexagon(float2 uv, float2 center, float size, float rotation) {
    float2 p = uv - center;

    // Rotate
    float c = cos(rotation);
    float s = sin(rotation);
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);

    // Hexagon SDF
    float3 k = float3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= float2(clamp(p.x, -k.z * size, k.z * size), size);

    return 1.0 - smoothstep(0.0, size * 0.2, length(p) * sign(p.y));
}

// Procedural polygon (for ghost sprites)
float proceduralPolygon(float2 uv, float2 center, float size, int sides, float rotation) {
    float2 p = uv - center;

    // Rotate
    float c = cos(rotation);
    float s = sin(rotation);
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);

    // Polar coordinates
    float angle = atan2(p.y, p.x);
    float radius = length(p);

    // Polygon shape
    float segmentAngle = 2.0 * PI / float(sides);
    float halfSegment = segmentAngle * 0.5;

    // Distance to edge
    float a = mod(angle + halfSegment, segmentAngle) - halfSegment;
    float r = size / cos(a);

    return 1.0 - smoothstep(r * 0.85, r, radius);
}

// Procedural starburst/sunburst pattern
float proceduralStarburst(float2 uv, float2 center, float size, int rays, float rotation) {
    float2 p = uv - center;

    // Rotate
    float c = cos(rotation);
    float s = sin(rotation);
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);

    float angle = atan2(p.y, p.x);
    float radius = length(p);

    // Ray pattern
    float rayPattern = abs(sin(angle * float(rays) * 0.5));
    rayPattern = pow(rayPattern, 2.0);

    // Distance falloff
    float falloff = 1.0 - smoothstep(0.0, size, radius);
    falloff = pow(falloff, 0.5);

    // Combine
    return rayPattern * falloff;
}

// Procedural anamorphic streak
float proceduralStreak(float2 uv, float2 center, float length, float width) {
    float2 p = uv - center;

    // Horizontal streak (anamorphic lens artifact)
    float horizontal = exp(-abs(p.x) / length) * exp(-abs(p.y) * width * 50.0);

    return horizontal;
}

// Procedural lens dirt (organic noise pattern)
float proceduralDirt(float2 uv, float scale, float time) {
    float2 p = uv * scale;

    // Multi-octave noise for organic look
    float dirt = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < 4; i++) {
        dirt += amplitude * flareNoise(p * frequency + time * 0.1);
        amplitude *= 0.5;
        frequency *= 2.0;
    }

    // Create spotty pattern
    dirt = pow(dirt, 2.0);
    dirt = smoothstep(0.3, 0.8, dirt);

    return dirt;
}

// Chromatic aberration offset for ghosts
float3 chromaticGhost(float2 uv, float2 center, float size, float chromaticOffset, float rotation) {
    float3 result;

    // Sample R, G, B at slightly different positions
    float2 offsetR = float2(chromaticOffset, 0.0);
    float2 offsetB = float2(-chromaticOffset, 0.0);

    result.r = proceduralPolygon(uv + offsetR, center, size * 1.02, 6, rotation);
    result.g = proceduralPolygon(uv, center, size, 6, rotation);
    result.b = proceduralPolygon(uv + offsetB, center, size * 0.98, 6, rotation);

    return result;
}

// ============================================================================
// Main Sun Flare Rendering
// ============================================================================

struct FlareVertexOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 flareTriVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

vertex FlareVertexOut sunFlareVertex(uint vertexID [[vertex_id]]) {
    FlareVertexOut out;
    out.position = float4(flareTriVerts[vertexID], 0.0, 1.0);
    out.uv = flareTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;
    return out;
}

fragment float4 sunFlareFragment(
    FlareVertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],  // Unused with hardware blending
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    constant SunFlareData& data [[buffer(0)]]
) {
    // Early out if sun not visible - return zero (add nothing)
    if (data.visibility < 0.01) {
        return float4(0.0);
    }

    // Check if sun is within valid screen range
    float2 sunPos = data.sunScreenPos;
    float margin = 0.5;
    if (sunPos.x < -margin || sunPos.x > 1.0 + margin ||
        sunPos.y < -margin || sunPos.y > 1.0 + margin) {
        return float4(0.0);
    }

    float2 uv = in.uv;
    float2 center = data.screenCenter;

    // Aspect-corrected UV for circular effects
    float2 aspectUV = uv;
    aspectUV.x *= data.aspectRatio.x;
    float2 aspectSunPos = sunPos;
    aspectSunPos.x *= data.aspectRatio.x;
    float2 aspectCenter = center;
    aspectCenter.x *= data.aspectRatio.x;

    // Direction from sun to center (for ghosts)
    float2 sunToCenter = center - sunPos;
    float distToCenter = length(sunToCenter);

    // Fade at screen edges
    float edgeFade = 1.0;
    edgeFade *= smoothstep(0.0, 0.2, sunPos.x) * smoothstep(1.0, 0.8, sunPos.x);
    edgeFade *= smoothstep(0.0, 0.2, sunPos.y) * smoothstep(1.0, 0.8, sunPos.y);
    edgeFade = mix(1.0, edgeFade, data.fadeEdge);

    float intensity = data.sunIntensity * data.visibility * edgeFade;
    float3 flareColor = float3(0.0);

    // ========================================================================
    // 1. Main Glow (central bright spot)
    // ========================================================================
    {
        float distToSun = length(aspectUV - aspectSunPos);
        float glow = exp(-distToSun * data.glowFalloff / data.glowSize);
        glow = pow(glow, 1.5);
        flareColor += data.sunColor * glow * data.glowIntensity;
    }

    // ========================================================================
    // 2. Halo Ring
    // ========================================================================
    {
        float halo = proceduralRing(aspectUV, aspectSunPos,
                                    data.haloRadius, data.haloWidth, data.haloFalloff);
        // Add rainbow effect to halo
        float haloAngle = atan2(uv.y - sunPos.y, uv.x - sunPos.x);
        float3 haloColor = data.sunColor;
        haloColor.r *= 1.0 + sin(haloAngle * 2.0) * 0.2;
        haloColor.b *= 1.0 + cos(haloAngle * 2.0) * 0.2;

        flareColor += haloColor * halo * data.haloIntensity;
    }

    // ========================================================================
    // 3. Ghost Sprites (lens reflections)
    // ========================================================================
    {
        float3 ghostTotal = float3(0.0);

        for (int i = 0; i < data.ghostCount; i++) {
            // Each ghost is positioned along the line from sun through center
            float t = float(i + 1) * data.ghostSpacing;
            float2 ghostPos = sunPos + sunToCenter * t;

            // Skip ghosts outside screen
            if (ghostPos.x < -0.1 || ghostPos.x > 1.1 ||
                ghostPos.y < -0.1 || ghostPos.y > 1.1) {
                continue;
            }

            // Size varies per ghost (smaller further from sun)
            float ghostSizeVar = data.ghostSize * (1.0 - float(i) * 0.1);

            // Rotation varies per ghost
            float rotation = float(i) * 0.5 + data.time * 0.1;

            // Chromatic ghost with hexagonal shape
            float3 ghost = chromaticGhost(uv, ghostPos, ghostSizeVar,
                                          data.ghostChromaticOffset, rotation);

            // Color tint varies per ghost
            float3 tint = data.sunColor;
            tint.r *= 1.0 + sin(float(i) * 1.2) * 0.3;
            tint.g *= 1.0 + sin(float(i) * 1.5 + 1.0) * 0.3;
            tint.b *= 1.0 + sin(float(i) * 1.8 + 2.0) * 0.3;

            // Distance falloff
            float ghostFalloff = 1.0 - float(i) / float(data.ghostCount);
            ghostFalloff = pow(ghostFalloff, data.ghostFalloff);

            ghostTotal += ghost * tint * ghostFalloff;
        }

        flareColor += ghostTotal * data.ghostIntensity;
    }

    // ========================================================================
    // 4. Anamorphic Streak
    // ========================================================================
    {
        float streak = proceduralStreak(uv, sunPos, data.streakLength, data.streakFalloff);

        // Slight color shift for streak
        float3 streakColor = data.sunColor;
        streakColor.r *= 1.1;
        streakColor.b *= 0.9;

        flareColor += streakColor * streak * data.streakIntensity;
    }

    // ========================================================================
    // 5. Starburst Pattern
    // ========================================================================
    {
        float starburst = proceduralStarburst(aspectUV, aspectSunPos,
                                              data.starburstSize, data.starburstPoints,
                                              data.starburstRotation);
        flareColor += data.sunColor * starburst * data.starburstIntensity;
    }

    // ========================================================================
    // 6. Lens Dirt (optional)
    // ========================================================================
    if (data.dirtIntensity > 0.01) {
        float dirt = proceduralDirt(uv, data.dirtScale, data.time);

        // Dirt only shows where there's bright light
        float dirtMask = proceduralCircle(aspectUV, aspectSunPos, 0.5, 0.8);
        dirtMask += proceduralCircle(aspectUV, aspectCenter, 0.3, 0.9) * 0.5;

        flareColor += data.sunColor * dirt * dirtMask * data.dirtIntensity;
    }

    // Apply overall intensity
    flareColor *= intensity;

    // Output only flare color - hardware additive blending handles compositing
    return float4(flareColor, 0.0);
}

// ============================================================================
// Sun Occlusion Test (Compute shader to check visibility)
// ============================================================================

kernel void sunOcclusionTest(
    constant float2& sunScreenPos [[buffer(0)]],
    texture2d<float, access::sample> depthTexture [[texture(0)]],
    device float& visibility [[buffer(1)]]
) {
    constexpr sampler pointSampler(filter::nearest, address::clamp_to_edge);

    // Sample depth at and around sun position
    float2 texelSize = float2(1.0 / 1920.0, 1.0 / 1080.0);  // Approximate, could be passed in
    float occluded = 0.0;
    float total = 0.0;

    // Sample in a small grid around sun position
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            float2 sampleUV = sunScreenPos + float2(x, y) * texelSize * 4.0;

            if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 &&
                sampleUV.y >= 0.0 && sampleUV.y <= 1.0) {

                float depth = depthTexture.sample(pointSampler, sampleUV).r;

                // If depth is very close to 1.0, it's sky (sun visible)
                if (depth >= 0.9999) {
                    occluded += 1.0;
                }
                total += 1.0;
            }
        }
    }

    // Visibility is ratio of unoccluded samples
    visibility = total > 0.0 ? occluded / total : 0.0;
}

// ============================================================================
// Downsampled Flare Pass (for performance on bright areas)
// ============================================================================

fragment float4 sunFlareDownsample(
    FlareVertexOut in [[stage_in]],
    texture2d<float, access::sample> sourceTexture [[texture(0)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    // 4-tap box filter
    float2 texelSize = 1.0 / float2(sourceTexture.get_width(), sourceTexture.get_height());

    float4 color = float4(0.0);
    color += sourceTexture.sample(linearSampler, in.uv + float2(-1, -1) * texelSize);
    color += sourceTexture.sample(linearSampler, in.uv + float2( 1, -1) * texelSize);
    color += sourceTexture.sample(linearSampler, in.uv + float2(-1,  1) * texelSize);
    color += sourceTexture.sample(linearSampler, in.uv + float2( 1,  1) * texelSize);

    return color * 0.25;
}
