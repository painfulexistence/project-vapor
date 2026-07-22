#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

// Bindless-materials specialization (the ICB draw mode). When the pipeline is
// built with kBindlessMaterials=true (function constant 0, RHI
// ShaderDesc::bindlessMaterials), the fragment reads its six material textures
// from the argument table at buffer(13) indexed by the materialID varying,
// instead of the per-draw bound slots 0-5 — required because a single
// executeCommandsInBuffer can't rebind textures between draws. Unspecialized
// pipelines see the constant as undefined -> false and keep the bound path.
constant bool kBindlessMaterialsSet [[function_constant(0)]];
constant bool kBindlessMaterials = is_function_constant_defined(kBindlessMaterialsSet) && kBindlessMaterialsSet;
constant bool kBoundMaterials = !kBindlessMaterials;

// One entry per material in the bindless table (matches the RHI's
// createTextureArgumentTable(..., texturesPerEntry=6) slot order).
struct MaterialTexs {
    texture2d<float, access::sample> albedo    [[id(0)]];
    texture2d<float, access::sample> normal    [[id(1)]];
    texture2d<float, access::sample> metallic  [[id(2)]];
    texture2d<float, access::sample> roughness [[id(3)]];
    texture2d<float, access::sample> occlusion [[id(4)]];
    texture2d<float, access::sample> emissive  [[id(5)]];
};

// The bindless variant must take EVERY texture through argument buffers: a
// pipeline built with supportIndirectCommandBuffers rejects any direct
// texture/sampler argument on the fragment function ("Fragment shader cannot
// be used with indirect command buffers"). This single-entry table carries the
// per-frame system textures (slot order = the renderer's Metal contract 6-15).
struct SystemTexs {
    texture2d<float, access::sample>     texAO          [[id(0)]];
    texture2d<float, access::sample>     texShadow      [[id(1)]];
    texturecube<float, access::sample>   irradianceMap  [[id(2)]];
    texturecube<float, access::sample>   prefilterMap   [[id(3)]];
    texture2d<float, access::sample>     brdfLUT        [[id(4)]];
    texture2d<float, access::sample>     rectLightVideo [[id(5)]];
    depth2d_array<float, access::sample> pssmShadowMaps [[id(6)]];
    texture2d<float, access::sample>     texPointShadow [[id(7)]];
    texture2d<float, access::sample>     gibsGI         [[id(8)]];
    texture2d<float, access::sample>     texSSCS        [[id(9)]];
    // RT reflection/refraction results (bindless path): the ICB fragment can't
    // take direct texture args, so these join the system table like the rest.
    texture2d<float, access::sample>     texReflection  [[id(10)]];
    texture2d<float, access::sample>     texRefraction  [[id(11)]];
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
    float4 worldPosition;
    float4 worldNormal;
    float4 worldTangent;
    float3 scaledLocalPos;
    float3 localNormal;
    // Material is fetched by id in the fragment (materials[materialID]), NOT
    // passed through inter-stage: the full 112-byte MaterialData overflowed
    // Metal's per-vertex output capacity, corrupting varyings and dropping
    // geometry (the Vulkan RHIMain.frag already fetches by fragMaterialID).
    uint materialID [[flat]];  // bindless table index (ICB mode) + material fetch index
};

// PBR shading helpers (Surface, Cook-Torrance BRDF, analytic lights, IBL) live
// in the shared library so the meshlet path shades identically.
#include "Res/shaders/3d_pbr_lib.metal"

vertex RasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraData& camera [[buffer(0)]],
    constant MaterialData* materials [[buffer(1)]],
    constant InstanceData* instances [[buffer(2)]],
    device const VertexData* in [[buffer(3)]],
    constant uint& instanceID [[buffer(4)]],
    uint baseInstance [[base_instance]]
) {
    RasterizerData vert;
    // Effective instance index. Normal/per-object draws pass it via buffer(4)
    // with baseInstance 0 (no-op); single-call MDI can't set a per-object
    // constant, so it passes instanceID 0 and carries the index in the draw
    // command's baseInstance.
    uint iid = instanceID + baseInstance;
    uint actualVertexID = instances[iid].vertexOffset + vertexID;
    float4x4 model = instances[iid].model;
    float3x3 normalMatrix = transpose(inverse(float3x3(
        model[0].xyz,
        model[1].xyz,
        model[2].xyz
    )));
    // Caution: worldNormal and worldTangent are not normalized yet, and they can be affected by model scaling
    vert.worldNormal = float4(normalMatrix * float3(in[actualVertexID].normal), 0.0);
    vert.worldTangent = float4(normalMatrix * in[actualVertexID].tangent.xyz, in[actualVertexID].tangent.w);
    vert.worldPosition = model * float4(in[actualVertexID].position, 1.0);
    vert.position = camera.proj * camera.view * vert.worldPosition;
    vert.uv = in[actualVertexID].uv;
    // Material fetched in the fragment by this id (not passed through
    // inter-stage — see RasterizerData).
    vert.materialID = instances[iid].materialID;
    
    // Pass scaled local position and local normal for Object Space Triplanar
    float3 scale = float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    vert.scaledLocalPos = float3(in[actualVertexID].position) * scale;
    vert.localNormal = float3(in[actualVertexID].normal);
    
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    // Per-draw material textures — bound path only. The bindless
    // specialization disables these and reads materialTexs instead.
    texture2d<float, access::sample> texAlbedo [[texture(0), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texNormal [[texture(1), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texMetallic [[texture(2), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texRoughness [[texture(3), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texOcclusion [[texture(4), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texEmissive [[texture(5), function_constant(kBoundMaterials)]],
    const device MaterialTexs* materialTexs [[buffer(13), function_constant(kBindlessMaterials)]],
    // System textures (Metal contract slots 6-15). Direct arguments on the
    // bound path only; the bindless path reads the same set from systemTexs
    // (locals with the original names are resolved at the top of the body, so
    // everything below is shared).
    texture2d<float, access::sample> texAOArg [[texture(6), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texShadowArg [[texture(7), function_constant(kBoundMaterials)]],
    texturecube<float, access::sample> irradianceMapArg [[texture(8), function_constant(kBoundMaterials)]],
    texturecube<float, access::sample> prefilterMapArg [[texture(9), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> brdfLUTArg [[texture(10), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> rectLightVideoArg [[texture(11), function_constant(kBoundMaterials)]],
    depth2d_array<float, access::sample> pssmShadowMapsArg [[texture(12), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> texPointShadowArg [[texture(13), function_constant(kBoundMaterials)]],
    texture2d<float, access::sample> gibsGIArg [[texture(14), function_constant(kBoundMaterials)]], // GIBS indirect lighting
    texture2d<float, access::sample> texSSCSArg [[texture(15), function_constant(kBoundMaterials)]], // screen-space contact shadow
    const device SystemTexs* systemTexs [[buffer(14), function_constant(kBindlessMaterials)]],
    // RT reflection/refraction textures: direct args on the bound path,
    // resolved from systemTexs on the bindless (ICB) path — same split as the
    // system textures above. Resolved to locals at the top of the body.
    texture2d<float, access::sample> texReflectionArg [[texture(16), function_constant(kBoundMaterials)]], // RT mirror reflections
    texture2d<float, access::sample> texRefractionArg [[texture(17), function_constant(kBoundMaterials)]], // RT refractions (transmission)
    const device DirLight* directionalLights [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    const device Cluster* clusters [[buffer(2)]],
    constant CameraData& camera [[buffer(3)]],
    constant float2& screenSize [[buffer(4)]],
    constant packed_uint3& gridSize [[buffer(5)]],
    constant float& time [[buffer(6)]],
    const device RectLight* rectLights [[buffer(7)]],
    constant uint& rectLightCount [[buffer(8)]],
    constant PSSMData& pssmData [[buffer(9)]],
    constant uint& gibsEnabled [[buffer(10)]], // GIBS enable flag
    // Material fetched by id here, not passed through inter-stage (the 112-byte
    // MaterialData overflowed Metal's per-vertex output). buffer(19): 0-18 are
    // taken (11 = dirLightCount, 12 = mainDebugFlags, 13/14 = bindless tables,
    // 15/16 = spot, 17/18 = RT params), so materials lives past them.
    const device MaterialData* materials [[buffer(19)]],
    // Perf-isolation debug flags (bit0 = skip point-light loop, bit1 = skip
    // shadow). buffer(11) is dirLightCount (Vulkan-only, unread here), so this
    // takes buffer(12). Mirrors RHIMain.frag's mainDebugFlags.
    constant uint& mainDebugFlags [[buffer(12)]],
    // Weather-driven IBL dimming (buffer 20 — 19 is materials). On Vulkan the
    // same value rides in LightCullData instead. Every pass drawing with this
    // fragment must bind it (main + RTT).
    constant float& iblIntensity [[buffer(20)]],
    // Spot lights at buffer(16): buffer(14) is the bindless systemTexs table,
    // so a plain buffer(14) here fails specialization ("invalid location").
    const device SpotLight* spotLights [[buffer(16)]],
    // x = spot light count, y = shadow-format flags (bit0 = the point-shadow
    // texture carries RGB channels: R point / G rect / B spot; 0 on legacy
    // R16F targets so rect/spot stay unshadowed instead of black).
    constant uint2& spotRectParams [[buffer(15)]],
    // RT reflection/refraction composite params (x = enabled, y = intensity).
    // Plain buffers at 17/18 — free on BOTH the bound and bindless paths (13/14
    // are the bindless argument tables, 16 is spotLights), so the RT composite
    // works in either draw mode. Buffers are legal on ICB fragments (only
    // direct texture/sampler args are rejected).
    constant float2& reflectionParams [[buffer(17)]],
    constant float2& refractionParams [[buffer(18)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);

    MaterialData material = materials[in.materialID];

    // Resolve the material texture set once: bound slots (normal path) or the
    // bindless table entry for this fragment's material (ICB path). The dead
    // branch references a disabled argument, which is legal — it's eliminated
    // when the function constant is folded at pipeline build.
    texture2d<float, access::sample> matAlbedo    = kBindlessMaterials ? materialTexs[in.materialID].albedo    : texAlbedo;
    texture2d<float, access::sample> matNormal    = kBindlessMaterials ? materialTexs[in.materialID].normal    : texNormal;
    texture2d<float, access::sample> matMetallic  = kBindlessMaterials ? materialTexs[in.materialID].metallic  : texMetallic;
    texture2d<float, access::sample> matRoughness = kBindlessMaterials ? materialTexs[in.materialID].roughness : texRoughness;
    texture2d<float, access::sample> matOcclusion = kBindlessMaterials ? materialTexs[in.materialID].occlusion : texOcclusion;
    texture2d<float, access::sample> matEmissive  = kBindlessMaterials ? materialTexs[in.materialID].emissive  : texEmissive;

    // System textures under their original names — the rest of the body (and
    // its lambdas/helper calls) is identical on both paths.
    texture2d<float, access::sample>     texAO          = kBindlessMaterials ? systemTexs->texAO          : texAOArg;
    texture2d<float, access::sample>     texShadow      = kBindlessMaterials ? systemTexs->texShadow      : texShadowArg;
    texturecube<float, access::sample>   irradianceMap  = kBindlessMaterials ? systemTexs->irradianceMap  : irradianceMapArg;
    texturecube<float, access::sample>   prefilterMap   = kBindlessMaterials ? systemTexs->prefilterMap   : prefilterMapArg;
    texture2d<float, access::sample>     brdfLUT        = kBindlessMaterials ? systemTexs->brdfLUT        : brdfLUTArg;
    texture2d<float, access::sample>     rectLightVideo = kBindlessMaterials ? systemTexs->rectLightVideo : rectLightVideoArg;
    depth2d_array<float, access::sample> pssmShadowMaps = kBindlessMaterials ? systemTexs->pssmShadowMaps : pssmShadowMapsArg;
    texture2d<float, access::sample>     texPointShadow = kBindlessMaterials ? systemTexs->texPointShadow : texPointShadowArg;
    texture2d<float, access::sample>     gibsGI         = kBindlessMaterials ? systemTexs->gibsGI         : gibsGIArg;
    texture2d<float, access::sample>     texSSCS        = kBindlessMaterials ? systemTexs->texSSCS        : texSSCSArg;
    texture2d<float, access::sample>     texReflection  = kBindlessMaterials ? systemTexs->texReflection  : texReflectionArg;
    texture2d<float, access::sample>     texRefraction  = kBindlessMaterials ? systemTexs->texRefraction  : texRefractionArg;

    // Prototype UV: triplanar mapping with world space or object space
    // Mode: 0 = Off, 1 = World Space (static objects), 2 = Object Space (dynamic objects)
    if (material.prototypeUVMode > 0.5) {
        float3 pos;
        float3 n;
        if (material.prototypeUVMode > 1.5) {
            // Object Space: position and normal in local space (texture follows object rotation)
            pos = in.scaledLocalPos;
            n = abs(normalize(in.localNormal));
        } else {
            // World Space: position and normal in world space (texture fixed in world)
            pos = in.worldPosition.xyz;
            n = abs(normalize(in.worldNormal.xyz));
        }

        // Select projection plane based on dominant normal axis
        if (n.x > n.y && n.x > n.z) {
            in.uv = pos.yz * material.uvScale;
        } else if (n.y > n.z) {
            in.uv = pos.xz * material.uvScale;
        } else {
            in.uv = pos.xy * material.uvScale;
        }
    }

    float4 baseColor = matAlbedo.sample(s, in.uv);
    // glTF MASK cutout: per-material cutoff (0 = disabled for OPAQUE/BLEND).
    if (material.emissiveFactor.a > 0.0 && baseColor.a * material.baseColorFactor.a < material.emissiveFactor.a) {
        discard_fragment();
    }
    Surface surf;
    surf.color = srgbToLinear(baseColor.rgb * material.baseColorFactor.rgb);
    surf.ao = matOcclusion.sample(s, in.uv).r * material.occlusionStrength;
    surf.roughness = matRoughness.sample(s, in.uv).g * material.roughnessFactor;
    surf.metallic = matMetallic.sample(s, in.uv).b * material.metallicFactor;
    // Emissive is an sRGB-authored colour (like albedo) — linearize it before
    // it joins the linear lighting sum. (Was linearToSRGB, the wrong direction:
    // that re-encodes toward sRGB and brightens; the sRGB->linear encode
    // belongs only at the final output, which PostProcess/the swapchain do.)
    surf.emission = srgbToLinear(matEmissive.sample(s, in.uv).rgb * material.emissiveFactor.rgb) * material.emissiveStrength;
    surf.subsurface = material.subsurface;
    surf.specular = material.specular;
    surf.specular_tint = material.specularTint;
    surf.anisotropic = material.anisotropic;
    surf.sheen = material.sheen;
    surf.sheen_tint = material.sheenTint;
    surf.clearcoat = material.clearcoat;
    surf.clearcoat_gloss = material.clearcoatGloss;

    float3 N = normalize(float3(in.worldNormal));
    float3 T = normalize(float3(in.worldTangent));
    T = normalize(T - dot(T, N) * N);
    float3 B = normalize(cross(N, T) * in.worldTangent.w);
    float3x3 TBN = float3x3(T, B, N);
    float3 norm = normalize(TBN * normalize(matNormal.sample(s, in.uv).rgb * 2.0 - 1.0));
    float3 viewDir = normalize(camera.position - in.worldPosition.xyz);

    float2 screenUV = in.position.xy / screenSize;

    // --- Shadow factor: RT shadow for near region, PSSM for mid/far ---
    constexpr sampler shadowCmpSampler(
        address::clamp_to_edge,
        filter::linear,
        compare_func::less_equal
    );
    constexpr float PSSM_TEXEL = 1.0 / 4096.0;
    constexpr float PSSM_BIAS  = 0.002;

    // abs(): view matrix is RH (visible z is negative); splits are positive distances
    float viewDepth = abs((camera.view * in.worldPosition).z);

    // Helper: sample PSSM shadow with configurable PCF
    auto samplePSSMShadow = [&](int cascadeIndex, float2 shadowUV, float refDepth) -> float {
        float pcf = 0.0;
        uint sampleCount = pssmData.pcfSampleCount;

        if (sampleCount <= 4) {
            // 4-tap Poisson disk
            for (int i = 0; i < 4; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk4[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 4.0;
        } else if (sampleCount <= 8) {
            // 8-tap Poisson disk
            for (int i = 0; i < 8; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk8[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 8.0;
        } else if (sampleCount <= 16) {
            // 16-tap Poisson disk
            for (int i = 0; i < 16; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk16[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 16.0;
        } else {
            // 32-tap: 16-tap Poisson + 16-tap rotated
            for (int i = 0; i < 16; i++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + poissonDisk16[i] * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
                // Rotated samples for better coverage
                float2 rotated = float2(
                    poissonDisk16[i].x * 0.7071 - poissonDisk16[i].y * 0.7071,
                    poissonDisk16[i].x * 0.7071 + poissonDisk16[i].y * 0.7071
                ) * 1.5;
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + rotated * PSSM_TEXEL * 2.0,
                    cascadeIndex, refDepth
                );
            }
            return pcf / 32.0;
        }
    };

    // Direction toward the sun; used for the slope-scaled shadow bias.
    float3 shadowL = normalize(-directionalLights[0].direction);

    // Helper: sample a specific cascade
    auto sampleCascade = [&](int ci) -> float {
        float4 lsPos = pssmData.lightSpaceMatrices[ci] * in.worldPosition;
        float3 proj  = lsPos.xyz / lsPos.w;
        float2 shadowUV = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
        // Per-cascade + slope-scaled depth bias. A flat bias is fine for the
        // near cascade but far cascades cover far more world per texel, and
        // grazing surfaces (small N·L, e.g. a ceiling lit obliquely) self-shadow
        // — the moiré / "z-fighting" acne that also swallows lit regions. Scale
        // the bias with cascade index and inverse N·L to suppress both.
        float ndl = max(dot(N, shadowL), 0.0);
        float slope = clamp(1.0 - ndl, 0.0, 1.0);
        float bias = PSSM_BIAS * float(ci + 1) * (1.0 + 2.0 * slope);
        float refDepth = proj.z - bias;
        return samplePSSMShadow(ci, shadowUV, refDepth);
    };

    // Crisp, non-repeating fetch for the screen-space RT shadow. Reusing the
    // material sampler `s` (address::repeat, mip_filter::linear) can pull a
    // blurred mip / wrap at screen edges, softening the RT shadow right where it
    // has to line up with PSSM.
    constexpr sampler rtShadowSampler(
        address::clamp_to_edge,
        filter::linear,
        mip_filter::none
    );

    // RT↔PSSM cross-fade is centred on rtEnd instead of starting there. The RT
    // shadow has crisp, contact-accurate edges; PSSM is slightly offset
    // (peter-panning from the depth bias + widened ortho range). A one-sided
    // fade lets the accurate RT shadow end abruptly at rtEnd, exposing a lit
    // sliver where the offset PSSM edge has not caught up yet — the bright line.
    // Centring the window keeps RT dominant across the contact region and only
    // hands off to PSSM well past the seam.
    float rtEnd     = pssmData.cascadeSplits.x;
    float halfBlend = pssmData.blendRange * 0.5;
    float blendLo   = rtEnd - halfBlend;
    float blendHi   = rtEnd + halfBlend;

    float shadowFactor;
    int debugCascade = -1; // -1 = RT, 0-2 = PSSM cascades

    if (viewDepth < blendLo) {
        // Fully inside the RT region
        shadowFactor = texShadow.sample(rtShadowSampler, screenUV).r;
        debugCascade = -1;
    } else {
        // At or past the RT boundary: evaluate the PSSM cascade
        int ci = 0;
        if      (viewDepth > pssmData.cascadeSplits.z) ci = 2;
        else if (viewDepth > pssmData.cascadeSplits.y) ci = 1;
        debugCascade = ci;

        shadowFactor = sampleCascade(ci);

        // Cascade blend: smooth transition between cascades
        float cascadeBlend = pssmData.cascadeBlendRange;
        if (cascadeBlend > 0.0 && ci < 2) {
            float cascadeEnd = (ci == 0) ? pssmData.cascadeSplits.y : pssmData.cascadeSplits.z;
            float blendStart = cascadeEnd - cascadeBlend;
            if (viewDepth > blendStart && viewDepth < cascadeEnd) {
                float nextShadow = sampleCascade(ci + 1);
                float t = (viewDepth - blendStart) / cascadeBlend;
                shadowFactor = mix(shadowFactor, nextShadow, smoothstep(0.0, 1.0, t));
            }
        }

        // Symmetric RT↔PSSM cross-fade window [blendLo, blendHi] around rtEnd
        if (viewDepth < blendHi && pssmData.blendRange > 0.0) {
            float rtShadow = texShadow.sample(rtShadowSampler, screenUV).r;
            float t = (viewDepth - blendLo) / pssmData.blendRange; // 0 at blendLo → 1 at blendHi
            shadowFactor = mix(rtShadow, shadowFactor, smoothstep(0.0, 1.0, t));
            if (t < 0.5) debugCascade = -1; // RT-dominant half shows as RT in debug view
        }
    }

    // Debug visualization: show cascade colors
    if (pssmData.debugVisualize > 0) {
        float3 cascadeColors[4] = {
            float3(0.2, 0.8, 0.2), // RT = green
            float3(0.8, 0.2, 0.2), // Cascade 0 = red
            float3(0.2, 0.2, 0.8), // Cascade 1 = blue
            float3(0.8, 0.8, 0.2)  // Cascade 2 = yellow
        };
        float3 cascadeColor = cascadeColors[debugCascade + 1];
        return float4(cascadeColor * shadowFactor, 1.0);
    }

    // Debug bit1: drop the shadow term to isolate its cost.
    if ((mainDebugFlags & 2u) != 0u) shadowFactor = 1.0;

    // Screen-space contact shadows tighten the near contact the RT/PSSM shadow
    // misses. min() = shadowed if either says so (no double-darkening).
    shadowFactor = min(shadowFactor, texSSCS.sample(rtShadowSampler, screenUV).r);

    float3 result = float3(0.0);
    result += CalculateDirectionalLight(directionalLights[0], norm, T, B, viewDir, surf) * shadowFactor;

    uint tileX = uint(screenUV.x * float(gridSize.x));
    uint tileY = uint((1.0 - screenUV.y) * float(gridSize.y));
    // float depthVS = (camera.view * in.worldPosition).z;
    // uint tileZ = uint((log(abs(depthVS) / camera.near) * gridSize.z) / log(camera.far / camera.near));
    // uint clusterIndex = tileX + (tileY * gridSize.x) + (tileZ * gridSize.x * gridSize.y);
    // Cluster cluster = clusters[clusterIndex];
    // uint lightCount = cluster.lightCount;

    // // Debug output - view space depth
    // // return float4((-depthVS - camera.near) / (camera.far - camera.near), 0.0, 0.0, 1.0);

    // // Debug output - tile indices
    // // return float4(tileX / float(gridSize.x), tileY / float(gridSize.y), 0.5, 1.0);
    // // return float4(tileX / float(gridSize.x), tileY / float(gridSize.y), 0.0, 1.0) * (tileZ / float(gridSize.z));

    // // Debug output - tile z
    // // return float4(tileZ / float(gridSize.z), 0.0, 0.0, 1.0);

    // // Debug output - cluster index
    // // return float4(clusterIndex / float(gridSize.x * gridSize.y * gridSize.z), 0.0, 0.0, 1.0);

    // // Debug output - cluster AABB
    // // return float4(
    // //     (cluster.min.x < cluster.max.x ? 1.0 : 0.0),
    // //     (cluster.min.y < cluster.max.y ? 1.0 : 0.0),
    // //     (cluster.min.z < cluster.max.z ? 1.0 : 0.0),
    // //     1.0
    // // );

    // // Debug output - light coverage
    // // for (uint i = 0; i < lightCount; i++) {
    // //     return float4(
    // //         cluster.lightIndices[i] == 0 ? 1.0 : 0.0,
    // //         cluster.lightIndices[i] == 1 ? 1.0 : 0.0,
    // //         cluster.lightIndices[i] == 2 ? 1.0 : 0.0,
    // //         1.0
    // //     );
    // // }

    // // Debug output - light count
    // // return float4(lightCount / 100.0, 0.0, 0.0, 1.0);

    // for (uint i = 0; i < lightCount; i++) {
    //     uint lightIndex = cluster.lightIndices[i];
    //     result += CalculatePointLight(pointLights[lightIndex], norm, T, B, viewDir, surf, in.worldPosition.xyz);
    // }

    uint tileIndex = tileX + tileY * gridSize.x;
    // Reference, not copy: Cluster is ~1KB (lightIndices[256]); copying it per
    // fragment spills to stack and reads the whole struct from device memory.
    const device Cluster& tile = clusters[tileIndex];
    // Debug bit0: skip the whole tiled point-light loop to isolate its cost.
    if ((mainDebugFlags & 1u) == 0u) {
        uint lightCount = tile.lightCount;
        float pointShadow = texPointShadow.sample(s, screenUV).r;
        for (uint i = 0; i < lightCount; i++) {
            uint lightIndex = tile.lightIndices[i];
            result += CalculatePointLight(pointLights[lightIndex], norm, T, B, viewDir, surf, in.worldPosition.xyz) * pointShadow;
        }
    }

    // Rect area lights, shadowed by the stochastic pass's G channel when the
    // RGB shadow format is active (spotRectParams.y bit0); legacy R16F targets
    // read G as 0, so the flag keeps them fully lit there instead of black.
    {
        float rectShadow = (spotRectParams.y & 1u) ? texPointShadow.sample(s, screenUV).g : 1.0;
        for (uint i = 0; i < rectLightCount; i++) {
            result += CalculateRectLight(rectLights[i], norm, in.worldPosition.xyz, viewDir, surf, rectLightVideo) * rectShadow;
        }
    }

    // Spot lights (loop-all; typical scenes carry a handful, so no clustering).
    // Shadowed by the stochastic pass's B channel under the same flag.
    {
        float spotShadow = (spotRectParams.y & 1u) ? texPointShadow.sample(s, screenUV).b : 1.0;
        for (uint i = 0; i < spotRectParams.x; i++) {
            result += CalculateSpotLight(spotLights[i], norm, T, B, viewDir, surf, in.worldPosition.xyz) * spotShadow;
        }
    }

    // Screen-space AO attenuates ambient/indirect light only — multiplying
    // direct light by AO is physically wrong and dirties lit surfaces
    float screenAO = texAO.sample(s, screenUV).r;

    // GIBS Global Illumination or IBL fallback
    if (gibsEnabled > 0) {
        // Sample GIBS indirect lighting at screen position
        float3 giContribution = gibsGI.sample(s, screenUV).rgb;
        // Apply ambient occlusion to indirect lighting
        result += giContribution * surf.ao * screenAO;
    } else if (material.iblEnabled > 0.5) {
        // iblIntensity: weather dims the baked environment under heavy cloud.
        result += CalculateIBL(norm, viewDir, surf, irradianceMap, prefilterMap, brdfLUT) * screenAO * iblIntensity;
    } else {
        result += float3(0.03) * surf.ao * surf.color * screenAO; // minimal ambient fallback
    }

    // RT mirror reflections (half-res, bilinearly upsampled by the sample).
    // Fresnel-weighted like the IBL specular, faded by roughness — the traced
    // ray is a mirror ray, so rough surfaces should not show sharp reflections.
    if (reflectionParams.x > 0.5) {
        float3 refl = texReflection.sample(s, screenUV).rgb;
        float NdotV = max(dot(norm, viewDir), 0.0);
        float3 F0r = mix(float3(0.04), surf.color, surf.metallic);
        float3 Fr = FresnelSchlickRoughness(NdotV, F0r, surf.roughness);
        float roughFade = (1.0 - surf.roughness) * (1.0 - surf.roughness);
        result += refl * Fr * roughFade * reflectionParams.y * screenAO;
    }

    // RT refractions (KHR_materials_transmission): blend the traced
    // transmitted radiance in by the material's transmission factor. What
    // Fresnel reflects cannot transmit (1 - F), the base color tints the
    // transmitted light like glTF's BTDF, and the sharp traced ray fades by
    // roughness exactly like the mirror reflection above. mix() REPLACES the
    // accumulated diffuse/GI response instead of adding — a transmissive
    // surface trades its diffuse lobe for transmission per the spec.
    if (refractionParams.x > 0.5 && material.transmission > 0.0) {
        float3 refr = texRefraction.sample(s, screenUV).rgb;
        float NdotV = max(dot(norm, viewDir), 0.0);
        float3 F0t = mix(float3(0.04), surf.color, surf.metallic);
        float3 Ft = FresnelSchlickRoughness(NdotV, F0t, surf.roughness);
        float roughFade = (1.0 - surf.roughness) * (1.0 - surf.roughness);
        float3 transmitted = refr * surf.color * (1.0 - Ft) * refractionParams.y;
        result = mix(result, transmitted, material.transmission * roughFade);
    }

    result += surf.emission;

    return float4(result, 1.0);
}