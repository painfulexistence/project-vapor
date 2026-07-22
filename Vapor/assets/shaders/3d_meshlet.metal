#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"  // shared CameraData/InstanceData/VertexData + inverse()
#include "Res/shaders/3d_pbr_lib.metal" // shared Surface/BRDF/analytic-light/IBL helpers

// Bring-up debug probes (the negative-errorThreshold "probe ladder" in meshMain
// + the mesh-only meshSynthetic pipeline) that were used to chase the blank-
// screen heisenbug. Compiled OUT by default — flip to 1 (and the renderer's
// kMeshletDebugProbes) to re-enable the probe ladder + its UI. Kept in-tree
// because they isolate object/mesh-stage faults nothing else can.
#define MESHLET_DEBUG_PROBES 0

// Meshlet task/mesh pipeline — Metal backend. Mirror of Meshlet.task /
// Meshlet.mesh / MeshletDebug.frag (Vulkan). objectMain culls per meshlet
// (frustum + backface cone + two-sphere cluster-LOD cut) and forwards survivor
// indices via the payload; meshMain expands the meshlet's triangles from the
// merged scene vertex buffer; fragmentMain shows a per-meshlet debug color.
//
// Buffer indices (RHI setVertexBuffer routes to BOTH object and mesh stages for
// mesh pipelines): 0=camera, 2=instances, 3=meshlets, 4=meshletVertices,
// 5=meshletTriangles(u8), 6=meshletBounds, 7=merged VB, 8=params (setVertexBytes).

// CameraData / InstanceData come from 3d_common.metal (identical layout).

// Must match Vapor::Meshlet (16 bytes).
struct Meshlet {
    uint vertexOffset;
    uint triangleOffset;  // byte index into the u8 triangle buffer
    uint vertexCount;
    uint triangleCount;
};

// Must match Vapor::MeshletBounds (112 bytes, static_assert'd in meshlet.hpp).
struct MeshletBounds {
    float4 cullSphere;
    float4 coneApex;
    float4 coneAxisCutoff;
    float4 lodSphere;
    float4 parentSphere;
    float lodError;
    float parentError;
    int group_;
    int refined;
    int depth;
    int _p0;
    int _p1;
    int _p2;
};

// VertexData comes from 3d_common.metal (identical layout).

// Mirror of the Vulkan push constants (setVertexBytes binding 8).
struct MeshletParams {
    uint instanceID;
    uint meshletOffset;
    uint meshletCount;
    float errorThreshold;  // screen fraction (pixel error / screen height)
};

struct MeshletPayload {
    uint meshletIndices[32];
};

// Rotationally-invariant screen projection of a mesh-local error sphere
// (clusterlod.h's recommended formula). Returns a screen fraction.
static float projectError(float4 sphere, float error, float4x4 model, float maxScale,
                          device const CameraData& cam) {
    float3 c = (model * float4(sphere.xyz, 1.0)).xyz;
    float r = sphere.w * maxScale;
    float e = error * maxScale;
    float d = max(distance(c, cam.position) - r, cam.near);
    return e / d * (cam.proj[1][1] * 0.5);
}

// Hi-Z occlusion params (object buffer 10). Mirror of 3d_gpu_cull.metal's
// OccParams — same max-depth pyramid + conventions, so the meshlet cull occludes
// against the exact Hi-Z the instance cull uses.
struct MeshletOccParams {
    uint  occlusionEnabled;
    uint  hizMipCount;
    float2 hizSize;   // mip-0 dimensions in texels
};

// Per-meshlet Hi-Z occlusion test — a verbatim port of 3d_gpu_cull.metal's
// occludedByHiZ, fed the meshlet's world-space bounding box (sphere AABB).
// Standard [0,1] depth; compares the box's nearest depth against the farthest
// occluder over its screen footprint. Conservative: any doubt keeps the meshlet.
static bool meshletOccludedByHiZ(device const CameraData& cam, float3 aabbMin, float3 aabbMax,
                                 texture2d<float> hiz, sampler hizSampler, MeshletOccParams occ) {
    float4x4 vp = cam.proj * cam.view;
    float2 uvMin = float2(1.0);
    float2 uvMax = float2(0.0);
    float minDepth = 1.0;
    for (int i = 0; i < 8; ++i) {
        float3 corner = float3((i & 1) != 0 ? aabbMax.x : aabbMin.x,
                               (i & 2) != 0 ? aabbMax.y : aabbMin.y,
                               (i & 4) != 0 ? aabbMax.z : aabbMin.z);
        float4 clip = vp * float4(corner, 1.0);
        if (clip.w <= 0.0) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * 0.5 + 0.5;
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        minDepth = min(minDepth, ndc.z);
    }
    if (uvMax.x < 0.0 || uvMin.x > 1.0 || uvMax.y < 0.0 || uvMin.y > 1.0) return false;
    uvMin = clamp(uvMin, 0.0, 1.0);
    uvMax = clamp(uvMax, 0.0, 1.0);
    float2 hMin = float2(uvMin.x, 1.0 - uvMax.y);
    float2 hMax = float2(uvMax.x, 1.0 - uvMin.y);
    float2 sizePx = (hMax - hMin) * occ.hizSize;
    float lvl = ceil(log2(max(max(sizePx.x, sizePx.y), 1.0)));
    lvl = clamp(lvl, 0.0, float(occ.hizMipCount - 1u));
    float o = 0.0;
    o = max(o, hiz.sample(hizSampler, float2(hMin.x, hMin.y), level(lvl)).r);
    o = max(o, hiz.sample(hizSampler, float2(hMax.x, hMin.y), level(lvl)).r);
    o = max(o, hiz.sample(hizSampler, float2(hMin.x, hMax.y), level(lvl)).r);
    o = max(o, hiz.sample(hizSampler, float2(hMax.x, hMax.y), level(lvl)).r);
    return minDepth > o;
}

[[object]] void objectMain(
    object_data MeshletPayload& payload [[payload]],
    mesh_grid_properties grid,
    device const CameraData&    cam       [[buffer(0)]],
    device const InstanceData*  instances [[buffer(2)]],
    device const MeshletBounds* bounds    [[buffer(6)]],
    constant MeshletParams&     params    [[buffer(8)]],
    // Hi-Z occlusion (main pass only; the pre-pass binds occ with
    // occlusionEnabled 0 so the pyramid it FEEDS is complete). buffer(9) is left
    // free for the survivor-stats counter on its own branch.
    constant MeshletOccParams&  occ        [[buffer(10)]],
    texture2d<float>            hiz        [[texture(0)]],
    sampler                     hizSampler [[sampler(0)]],
    uint tid [[thread_position_in_threadgroup]],
    uint gid [[threadgroup_position_in_grid]]
) {
    // Per-lane visibility test — one meshlet per thread.
    uint local = gid * 32u + tid;
    uint mi = params.meshletOffset + local;
    bool visible = false;
    if (local < params.meshletCount) {
        MeshletBounds b = bounds[mi];
        float4x4 model = instances[params.instanceID].model;
        float maxScale = max(length(model[0].xyz), max(length(model[1].xyz), length(model[2].xyz)));

        // Debug bypass: a negative threshold means "emit everything" — skips
        // frustum, cone, AND the LOD cut so raster problems can be isolated
        // from culling problems.
        bool cullBypass = params.errorThreshold < 0.0;

        // Frustum: world-space sphere vs the camera planes.
        float3 wc = (model * float4(b.cullSphere.xyz, 1.0)).xyz;
        float wr = b.cullSphere.w * maxScale;
        visible = true;
        for (int i = 0; i < 6 && !cullBypass; ++i) {
            float4 plane = cam.frustumPlanes[i];
            if (dot(plane.xyz, wc) + plane.w < -wr) { visible = false; break; }
        }

        // Backface cone (meshopt): cull when the whole cluster faces away.
        if (visible && !cullBypass && b.coneAxisCutoff.w < 1.0) {
            float3 apexW = (model * float4(b.coneApex.xyz, 1.0)).xyz;
            float3 axisW = normalize(float3x3(model[0].xyz, model[1].xyz, model[2].xyz) * b.coneAxisCutoff.xyz);
            if (dot(normalize(apexW - cam.position), axisW) >= b.coneAxisCutoff.w) {
                visible = false;
            }
        }

        // Two-sphere LOD cut: parent too coarse AND this cluster fine enough.
        if (visible && !cullBypass) {
            bool parentTooCoarse =
                projectError(b.parentSphere, b.parentError, model, maxScale, cam) > params.errorThreshold;
            bool thisFineEnough = (b.refined < 0) ||
                (projectError(b.lodSphere, b.lodError, model, maxScale, cam) <= params.errorThreshold);
            visible = parentTooCoarse && thisFineEnough;
        }

        // Hi-Z occlusion: cull the cluster if its world bounding box is entirely
        // behind the depth pyramid the pre-pass built. Main pass only (the
        // pre-pass passes occlusionEnabled 0 so it draws every frustum-visible
        // meshlet, keeping the pyramid complete). This is the layer that made the
        // "Hi-Z occlusion" toggle actually affect the meshlet path.
        if (visible && !cullBypass && occ.occlusionEnabled != 0u) {
            float3 aabbMin = wc - wr;
            float3 aabbMax = wc + wr;
            if (meshletOccludedByHiZ(cam, aabbMin, aabbMax, hiz, hizSampler, occ)) {
                visible = false;
            }
        }
    }

    // Compact survivors into the payload with SIMD-group ops (matches
    // metal-by-example/MetalMeshletCulling). The object threadgroup is exactly
    // one 32-wide SIMD-group (taskThreadgroupSize == 32), so simd_prefix_exclusive_sum
    // assigns each survivor a dense payload slot IN-LANE, with implicit SIMD
    // synchronization — no threadgroup atomic, no barrier. The previous
    // atomic-counter + threadgroup_barrier version relied on threadgroup/payload
    // memory ordering that only held under Metal Shader Validation (which
    // serializes + zero-fills that memory): without it, the cooperatively filled
    // object_data payload did not reliably land before the spawned mesh grid read
    // it, so meshMain read stale indices and nothing rasterized (blank unless
    // validation was on). SIMD prefix-sum removes that shared-memory hazard.
    uint vote = visible ? 1u : 0u;
    uint slot = simd_prefix_exclusive_sum(vote);
    if (visible) {
        payload.meshletIndices[slot] = mi;
    }
    uint count = simd_sum(vote);
    if (tid == 0u) {
        grid.set_threadgroups_per_grid(uint3(count, 1u, 1u));
    }
}

struct MeshletVertexOut {
    float4 position [[position]];
    float3 color;
    float3 worldNormal;  // for the depth+normal pre-pass MRT (real path only)
    float2 uv;           // material-table sampling (real path only)
    float3 worldPosition;// PBR shade: view dir + point/spot/rect lights (real path)
    float4 worldTangent; // PBR shade: TBN for normal mapping (.w = handedness)
    float3 scaledLocalPos;// prototype/triplanar UV (object-space mode)
    float3 localNormal;   // prototype/triplanar UV (object-space mode)
    uint   materialID [[flat]];  // bindless material-table index (real path only)
};

// One entry per material in the shared bindless table (SAME layout as
// 3d_pbr_normal_mapped.metal's MaterialTexs — the renderer binds the identical
// bindlessMaterialTable buffer, created at fragment buffer(13) with 6 texture
// slots per material, indexed by materialID). Only `albedo` is read for now;
// the rest are declared so the argument-buffer layout matches the table the
// encoder wrote (a truncated struct would misread later slots).
struct MeshletMaterialTexs {
    texture2d<float, access::sample> albedo    [[id(0)]];
    texture2d<float, access::sample> normal    [[id(1)]];
    texture2d<float, access::sample> metallic  [[id(2)]];
    texture2d<float, access::sample> roughness [[id(3)]];
    texture2d<float, access::sample> occlusion [[id(4)]];
    texture2d<float, access::sample> emissive  [[id(5)]];
};

// Per-primitive output. The working reference (metal-by-example/
// MetalMeshletCulling) declares a real primitive data type and calls
// set_primitive() for every counted primitive; our previous `void` primitive
// type had only ever been proven at primitive_count==1 (every probe), never
// at real triangleCounts. Matching the reference removes the last structural
// difference from a known-good implementation.
struct MeshletPrimOut {
    float3 primColor [[flat]];
};

using MeshletMeshT = metal::mesh<MeshletVertexOut, MeshletPrimOut, 64, 128, metal::topology::triangle>;

// Distinct-ish color per meshlet (integer hash).
static float3 hashColor(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x = x ^ (x >> 4u);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return float3(float(x & 255u), float((x >> 8u) & 255u), float((x >> 16u) & 255u)) / 255.0;
}

[[mesh]] void meshMain(
    MeshletMeshT output,
    const object_data MeshletPayload& payload [[payload]],
    device const CameraData&   cam              [[buffer(0)]],
    device const InstanceData* instances        [[buffer(2)]],
    device const Meshlet*      meshlets         [[buffer(3)]],
    device const uint*         meshletVertices  [[buffer(4)]],
    device const uchar*        meshletTriangles [[buffer(5)]],
    device const VertexData*   vertices         [[buffer(7)]],
    constant MeshletParams&    params           [[buffer(8)]],
    uint tid [[thread_position_in_threadgroup]],
    uint gid [[threadgroup_position_in_grid]]
) {
#if MESHLET_DEBUG_PROBES
    // Data probe (errorThreshold <= -2.5): read the REAL payload + meshlet
    // record, then emit one FIXED-position triangle colored by what was read —
    // R = vertexCount/64, G = triangleCount/128, B = (mi & 255)/255. This
    // isolates "buffers/payload read garbage" (black or wild colors, or no
    // triangle if primitive_count is garbage) from "geometry/transform wrong"
    // (sane yellowish triangle, ~0.6-1.0 R+G for real clusters). Fixed position
    // so it can't be pushed offscreen by a bad transform.
    // Topology probe (errorThreshold <= -6.5): the vertex loop is proven good,
    // so read the one untested mesh-stage buffer — meshletTriangles(5), u8 —
    // and validate the first triangle's three local indices against the
    // meshlet's vertexCount:
    //   R = all three indices < vertexCount   (in range)
    //   G = the three indices are not all equal (non-degenerate)
    //   B = first index, scaled              (shows a real, varying value)
    // Legend:
    //   white/varied -> indices read fine; bug is set_index mechanics / count
    //   no R         -> indices out of range: buffer 5 unbound or garbage bytes
    //   no G (+dark) -> all-zero read: buffer 5 not reaching the mesh stage
    if (params.errorThreshold <= -6.5) {
        uint mi = payload.meshletIndices[gid];
        Meshlet m = meshlets[mi];
        uint i0 = uint(meshletTriangles[m.triangleOffset + 0u]);
        uint i1 = uint(meshletTriangles[m.triangleOffset + 1u]);
        uint i2 = uint(meshletTriangles[m.triangleOffset + 2u]);
        bool inRange = (i0 < m.vertexCount) && (i1 < m.vertexCount) && (i2 < m.vertexCount);
        bool nonDegen = !(i0 == i1 && i1 == i2);
        float3 c = float3(inRange ? 1.0 : 0.0,
                          nonDegen ? 1.0 : 0.0,
                          float(i0) / float(max(m.vertexCount, 1u)));
        if (tid == 0u) {
            MeshletVertexOut a, b, cc;
            a.position = float4(-0.9, -0.9, 0.5, 1.0); a.color = c;
            b.position = float4( 0.9, -0.9, 0.5, 1.0); b.color = c;
            cc.position = float4( 0.0,  0.9, 0.5, 1.0); cc.color = c;
            output.set_vertex(0, a);
            output.set_vertex(1, b);
            output.set_vertex(2, cc);
            output.set_index(0, 0);
            output.set_index(1, 1);
            output.set_index(2, 2);
            { MeshletPrimOut pr; pr.primColor = float3(1.0); output.set_primitive(0, pr); }
            output.set_primitive_count(1);
        }
        return;
    }

    // Emission probe (errorThreshold <= -5.5): run the REAL multi-threaded
    // vertex loop (transform + set_vertex across all 64 threads — untested by
    // every tid==0-only probe above), but with HARDCODED topology (just the
    // meshlet's first triangle, indices 0/1/2). This splits the two remaining
    // suspects:
    //   cyan triangles appear -> the vertex loop + set_vertex work; the bug is
    //     the INDEX loop (set_index / meshletTriangles buffer 5).
    //   blank                 -> the multi-thread vertex emission itself is the
    //     fault (set_vertex in a strided loop, or output completeness).
    if (params.errorThreshold <= -5.5) {
        uint mi = payload.meshletIndices[gid];
        Meshlet m = meshlets[mi];
        float4x4 model = instances[params.instanceID].model;
        float4x4 viewProj = cam.proj * cam.view;
        for (uint v = tid; v < m.vertexCount; v += 64u) {
            uint vi = meshletVertices[m.vertexOffset + v];
            MeshletVertexOut vout;
            vout.position = viewProj * (model * float4(float3(vertices[vi].position), 1.0));
            vout.color = float3(0.0, 1.0, 1.0);
            output.set_vertex(v, vout);
        }
        if (tid == 0u) {
            output.set_index(0, 0);
            output.set_index(1, 1);
            output.set_index(2, 2);
            { MeshletPrimOut pr; pr.primColor = float3(1.0); output.set_primitive(0, pr); }
            output.set_primitive_count(1);  // just the first triangle
        }
        return;
    }

    // Transform probe (errorThreshold <= -4.5): position reads are proven good,
    // so test the last untested reads — camera(0) and instances(2) in the MESH
    // stage — plus the transform math. Compute the real clip position of the
    // first vertex and report where it lands:
    //   R = clip.w > 0            (vertex is in front of the camera)
    //   G = |ndc.xy| <= 1         (inside the screen rectangle)
    //   B = 0 <= ndc.z <= 1       (inside the depth range)
    // Legend:
    //   white  (R+G+B) -> transform is correct; the bug is TOPOLOGY (set_index /
    //                     meshletTriangles buffer 5)
    //   no R           -> behind camera: model or view is wrong (bad instanceID,
    //                     or camera/instance buffer not reaching the mesh stage)
    //   R, no G        -> in front but off-screen: projection or model scale wrong
    //   R+G, no B      -> on-screen XY but depth out of range: proj Z convention
    if (params.errorThreshold <= -4.5) {
        uint mi = payload.meshletIndices[gid];
        Meshlet m = meshlets[mi];
        uint vi = meshletVertices[m.vertexOffset];
        float4x4 model = instances[params.instanceID].model;
        float4 clip = cam.proj * cam.view * model * float4(float3(vertices[vi].position), 1.0);
        float3 ndc = clip.xyz / clip.w;
        bool front = clip.w > 0.0;
        bool inXY = front && abs(ndc.x) <= 1.0 && abs(ndc.y) <= 1.0;
        bool inZ  = front && ndc.z >= 0.0 && ndc.z <= 1.0;
        float3 c = float3(front ? 1.0 : 0.0, inXY ? 1.0 : 0.0, inZ ? 1.0 : 0.0);
        if (tid == 0u) {
            MeshletVertexOut a, b, cc;
            a.position = float4(-0.9, -0.9, 0.5, 1.0); a.color = c;
            b.position = float4( 0.9, -0.9, 0.5, 1.0); b.color = c;
            cc.position = float4( 0.0,  0.9, 0.5, 1.0); cc.color = c;
            output.set_vertex(0, a);
            output.set_vertex(1, b);
            output.set_vertex(2, cc);
            output.set_index(0, 0);
            output.set_index(1, 1);
            output.set_index(2, 2);
            { MeshletPrimOut pr; pr.primColor = float3(1.0); output.set_primitive(0, pr); }
            output.set_primitive_count(1);
        }
        return;
    }

    // Vertex-read probe (errorThreshold <= -3.5): counts are proven valid, so
    // now test whether the mesh stage actually reads real vertex POSITIONS
    // through the full chain meshletVertices[4] -> mergedVB[7]. Emit the fixed
    // triangle colored by predicates on the first vertex's position:
    //   R = position is non-zero        (read returned data, not silent 0)
    //   G = position magnitude is sane  (|comp| < 1e5 -> not garbage/NaN)
    //   B = the vi index varies         (vi & 255)
    // Legend:
    //   white  (R+G+B) -> positions read fine; bug is the TRANSFORM or set_index
    //   cyan   (G+B, no R) -> position is all-zero: mergedVB(7) or meshletVertices(4) unbound
    //   magenta(R+B, no G) -> position huge/NaN: VertexData stride/layout wrong
    //   black          -> vi or the whole chain is broken
    if (params.errorThreshold <= -3.5) {
        uint mi = payload.meshletIndices[gid];
        Meshlet m = meshlets[mi];
        uint vi = meshletVertices[m.vertexOffset];  // first vertex of the meshlet
        float3 p = float3(vertices[vi].position);
        float mag = abs(p.x) + abs(p.y) + abs(p.z);
        bool nonZero = mag > 1e-4;
        bool sane = isfinite(mag) && mag < 1e5;
        float3 c = float3(nonZero ? 1.0 : 0.0, sane ? 1.0 : 0.0, float(vi & 255u) / 255.0);
        if (tid == 0u) {
            MeshletVertexOut a, b, cc;
            a.position = float4(-0.9, -0.9, 0.5, 1.0); a.color = c;
            b.position = float4( 0.9, -0.9, 0.5, 1.0); b.color = c;
            cc.position = float4( 0.0,  0.9, 0.5, 1.0); cc.color = c;
            output.set_vertex(0, a);
            output.set_vertex(1, b);
            output.set_vertex(2, cc);
            output.set_index(0, 0);
            output.set_index(1, 1);
            output.set_index(2, 2);
            { MeshletPrimOut pr; pr.primColor = float3(1.0); output.set_primitive(0, pr); }
            output.set_primitive_count(1);
        }
        return;
    }

    if (params.errorThreshold <= -2.5) {
        uint mi = payload.meshletIndices[gid];
        Meshlet m = meshlets[mi];
        // Decisive binary predicates (removes the normalized-color ambiguity):
        //   R = vertexCount in valid range  [1, 64]
        //   G = triangleCount in valid range [1, 128]
        //   B = triangleCount is EXACTLY zero
        // Legend:
        //   yellow  (R+G)  -> both fields valid; bug is downstream geometry/index
        //   magenta (R+B)  -> vertexCount valid, triangleCount HARD ZERO
        //   red     (R)    -> vertexCount valid, triangleCount garbage-large
        //   black          -> vertexCount also bad (binding / mi / struct)
        bool vOk = (m.vertexCount >= 1u && m.vertexCount <= 64u);
        bool tOk = (m.triangleCount >= 1u && m.triangleCount <= 128u);
        bool tZero = (m.triangleCount == 0u);
        float3 c = float3(vOk ? 1.0 : 0.0, tOk ? 1.0 : 0.0, tZero ? 1.0 : 0.0);
        if (tid == 0u) {
            MeshletVertexOut a, b, cc;
            a.position = float4(-0.9, -0.9, 0.5, 1.0); a.color = c;
            b.position = float4( 0.9, -0.9, 0.5, 1.0); b.color = c;
            cc.position = float4( 0.0,  0.9, 0.5, 1.0); cc.color = c;
            output.set_vertex(0, a);
            output.set_vertex(1, b);
            output.set_vertex(2, cc);
            output.set_index(0, 0);
            output.set_index(1, 1);
            output.set_index(2, 2);
            { MeshletPrimOut pr; pr.primColor = float3(1.0); output.set_primitive(0, pr); }
            output.set_primitive_count(1);
        }
        return;
    }

    // Synthetic-triangle probe (errorThreshold <= -1.5): emit one hardcoded
    // clip-space triangle and read NO buffers at all. If this shows on screen,
    // the pipeline/dispatch/raster chain is healthy and the fault is in the
    // object/mesh-stage buffer bindings (zero reads -> degenerate triangles).
    if (params.errorThreshold <= -1.5) {
        if (tid == 0u) {
            MeshletVertexOut a, b, c;
            a.position = float4(-0.9, -0.9, 0.5, 1.0); a.color = float3(1.0, 0.0, 1.0);
            b.position = float4( 0.9, -0.9, 0.5, 1.0); b.color = float3(1.0, 1.0, 0.0);
            c.position = float4( 0.0,  0.9, 0.5, 1.0); c.color = float3(0.0, 1.0, 1.0);
            output.set_vertex(0, a);
            output.set_vertex(1, b);
            output.set_vertex(2, c);
            output.set_index(0, 0);
            output.set_index(1, 1);
            output.set_index(2, 2);
            { MeshletPrimOut pr; pr.primColor = float3(1.0); output.set_primitive(0, pr); }
            output.set_primitive_count(1);
        }
        return;
    }
#endif // MESHLET_DEBUG_PROBES

    uint mi = payload.meshletIndices[gid];
    Meshlet m = meshlets[mi];

    // ONE thread per vertex / per triangle — the pattern Apple's mesh-shader
    // samples use. The mesh threadgroup is sized to MAX(maxVertexCount,
    // maxTriangleCount) = 128 threads (see meshThreadgroupSize in the pipeline),
    // matching the reference implementation's emission pattern exactly.

    // Transform in the SAME associativity as the pre-pass / main vertex shader:
    // world = model * pos, clip = proj * view * world (a fused MVP rounds
    // differently and can trip the main pass's LessOrEqual depth test).
    float4x4 model = instances[params.instanceID].model;
    float4x4 viewProj = cam.proj * cam.view;
    float3x3 model33 = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    float3x3 normalMatrix = transpose(inverse(model33));
    float3 color = hashColor(mi);

    if (tid < m.vertexCount) {
        uint vi = meshletVertices[m.vertexOffset + tid];  // merged-VB vertex index
        MeshletVertexOut vout;
        float4 wp = model * float4(float3(vertices[vi].position), 1.0);
        vout.position = viewProj * wp;
        vout.color = color;
        vout.worldNormal = normalMatrix * float3(vertices[vi].normal.xyz);
        // Tangent: same normalMatrix transform as the PBR vertex shader; carry
        // the glTF handedness sign in .w for the bitangent.
        float4 tan = vertices[vi].tangent;
        vout.worldTangent = float4(normalMatrix * tan.xyz, tan.w);
        vout.worldPosition = wp.xyz;
        vout.uv = float2(vertices[vi].uv);
        // Prototype/triplanar UV inputs (object-space mode): scaled local pos +
        // untransformed local normal. Same as the PBR vertex shader.
        float3 lscale = float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
        vout.scaledLocalPos = float3(vertices[vi].position) * lscale;
        vout.localNormal = float3(vertices[vi].normal.xyz);
        // Same materialID the PBR path reads (instances[iid].materialID). Constant
        // across the meshlet's instance, so [[flat]] on the varying.
        vout.materialID = instances[params.instanceID].materialID;
        output.set_vertex(tid, vout);
    }
    if (tid < m.triangleCount) {
        uint i = tid * 3u;  // this thread owns triangle `tid`, indices i..i+2
        // Defensive clamp: an out-of-range index in the emitted set is
        // undefined behavior that can drop the whole meshlet. All probed data
        // is in range, but the clamp turns any residual stray into a visible
        // distorted triangle instead of a silent blank.
        uint maxIdx = m.vertexCount - 1u;
        output.set_index(i + 0u, min(uint(meshletTriangles[m.triangleOffset + i + 0u]), maxIdx));
        output.set_index(i + 1u, min(uint(meshletTriangles[m.triangleOffset + i + 1u]), maxIdx));
        output.set_index(i + 2u, min(uint(meshletTriangles[m.triangleOffset + i + 2u]), maxIdx));
        MeshletPrimOut pr;
        pr.primColor = color;
        output.set_primitive(tid, pr);
    }
    if (tid == 0u) {
        output.set_primitive_count(m.triangleCount);
    }
}

// One PSSM cascade: project into its light space, 4-tap Poisson PCF with a
// per-cascade slope-scaled bias (same math as the forward pass's sampleCascade).
static float meshletSampleCascade(float3 worldPos, float ndl, int ci,
                                  constant PSSMData& pssm,
                                  depth2d_array<float, access::sample> shadowMaps) {
    float4 lsPos = pssm.lightSpaceMatrices[ci] * float4(worldPos, 1.0);
    float3 proj  = lsPos.xyz / lsPos.w;
    float2 uv    = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
    float  slope = clamp(1.0 - ndl, 0.0, 1.0);
    float  bias  = 0.002 * float(ci + 1) * (1.0 + 2.0 * slope);
    float  ref   = proj.z - bias;
    constexpr sampler cmp(address::clamp_to_edge, filter::linear, compare_func::less_equal);
    const float texel = 1.0 / 4096.0;
    float s = 0.0;
    for (int i = 0; i < 4; ++i)
        s += shadowMaps.sample_compare(cmp, uv + poissonDisk4[i] * texel * 2.0, ci, ref);
    return s * 0.25;
}

// Directional shadow with full forward parity: an RT hard-shadow near region,
// 3-cascade PSSM with cascade blend for mid/far, and a symmetric RT↔PSSM
// cross-fade around the RT end (matches 3d_pbr_normal_mapped.metal). The RT
// region collapses automatically when RT is off — the renderer binds a white
// rtShadow AND cascadeSplits.x (rtEnd) is 0, so blendLo < 0 and the near branch
// never triggers (PSSM covers everything), exactly like the forward pass.
static float meshletDirShadow(float3 worldPos, float ndl, float viewZ, float2 screenUV,
                              constant PSSMData& pssm,
                              depth2d_array<float, access::sample> shadowMaps,
                              texture2d<float, access::sample> rtShadow) {
    // Crisp, non-repeating fetch for the screen-space RT shadow (same as main).
    constexpr sampler rtShadowSampler(address::clamp_to_edge, filter::linear, mip_filter::none);
    float rtEnd     = pssm.cascadeSplits.x;
    float halfBlend = pssm.blendRange * 0.5;
    float blendLo   = rtEnd - halfBlend;
    float blendHi   = rtEnd + halfBlend;

    // Fully inside the RT region: the crisp traced shadow.
    if (viewZ < blendLo) {
        return rtShadow.sample(rtShadowSampler, screenUV).r;
    }

    // Mid/far: PSSM cascade + cascade blend.
    int ci = 0;
    if      (viewZ > pssm.cascadeSplits.z) ci = 2;
    else if (viewZ > pssm.cascadeSplits.y) ci = 1;
    float sh = meshletSampleCascade(worldPos, ndl, ci, pssm, shadowMaps);
    float cb = pssm.cascadeBlendRange;
    if (cb > 0.0 && ci < 2) {
        float cascadeEnd = (ci == 0) ? pssm.cascadeSplits.y : pssm.cascadeSplits.z;
        float blendStart = cascadeEnd - cb;
        if (viewZ > blendStart && viewZ < cascadeEnd) {
            float next = meshletSampleCascade(worldPos, ndl, ci + 1, pssm, shadowMaps);
            float t = (viewZ - blendStart) / cb;
            sh = mix(sh, next, smoothstep(0.0, 1.0, t));
        }
    }

    // Symmetric RT↔PSSM cross-fade window [blendLo, blendHi] centred on rtEnd.
    if (viewZ < blendHi && pssm.blendRange > 0.0) {
        float rt = rtShadow.sample(rtShadowSampler, screenUV).r;
        float t = (viewZ - blendLo) / pssm.blendRange;// 0 at blendLo -> 1 at blendHi
        sh = mix(rt, sh, smoothstep(0.0, 1.0, t));
    }
    return sh;
}

// Per-material light counts + a directional-shadow enable, packed to avoid a
// pile of single-uint buffers. pointCount is unused now that point lights come
// from the tiled clusters (kept for layout stability / fallback).
struct MeshletLightCounts { uint pointCount; uint spotCount; uint rectCount; uint dirShadowOn; };

// Screen-space effect gates + params (mirrors the forward pass's runtime flags).
// shadowRGB: the point-shadow texture carries R point / G rect / B spot channels
// (stochastic format). gibsOn: sample GIBS GI instead of IBL. refl/refr: RT
// reflection/refraction composite enable + intensity.
struct MeshletShadeFlags {
    uint  shadowRGB;
    uint  gibsOn;
    float reflOn;
    float reflIntensity;
    float refrOn;
    float refrIntensity;
};

// Clustered point-light tile lookup params (mirrors the forward pass's
// screenSize @4 / gridSize @5): screen size to derive the tile UV, grid dims to
// index the cluster buffer. gridSize is a 2D tile grid here (Z unused, matching
// the main pass's tileIndex = x + y*gridX).
struct MeshletTileParams { uint gridX; uint gridY; uint gridZ; float screenW; float screenH; };

fragment float4 fragmentMain(MeshletVertexOut in [[stage_in]],
                             constant uint&              shadeMode  [[buffer(0)]],
                             constant CameraData&        camera     [[buffer(1)]],
                             const device MaterialData*  materials  [[buffer(2)]],
                             const device DirLight*      dirLights  [[buffer(3)]],
                             const device PointLight*    pointLights[[buffer(4)]],
                             const device SpotLight*     spotLights [[buffer(5)]],
                             const device RectLight*     rectLights [[buffer(6)]],
                             constant PSSMData&          pssmData   [[buffer(7)]],
                             constant MeshletLightCounts& counts    [[buffer(8)]],
                             const device Cluster*       clusters   [[buffer(9)]],
                             constant MeshletTileParams& tile       [[buffer(10)]],
                             constant MeshletShadeFlags& flags      [[buffer(11)]],
                             // Weather-driven IBL dimming — same value the
                             // forward PBR fragment reads at buffer(20).
                             // x = weather IBL dimming, y = cloud-shadow strength
                             // (same pair the forward fragment reads at buffer 20).
                             constant float2&            envParams  [[buffer(12)]],
                             // Shared bindless material table (buffer 13), same
                             // handle the Bindless-MDI PBR fragment consumes.
                             const device MeshletMaterialTexs* materialTexs [[buffer(13)]],
                             // System textures for IBL + directional shadow +
                             // point shadow + rect-light video + screen-space AO/
                             // SSCS/GI/RT. Direct args are legal here (the meshlet
                             // pipeline is NOT an ICB pipeline).
                             texturecube<float, access::sample>   irradianceMap  [[texture(0)]],
                             texturecube<float, access::sample>   prefilterMap   [[texture(1)]],
                             texture2d<float, access::sample>     brdfLUT        [[texture(2)]],
                             depth2d_array<float, access::sample> pssmShadowMaps [[texture(3)]],
                             texture2d<float, access::sample>     rectLightVideo [[texture(4)]],
                             texture2d<float, access::sample>     pointShadowTex [[texture(5)]],
                             texture2d<float, access::sample>     texAO          [[texture(6)]],
                             texture2d<float, access::sample>     texSSCS        [[texture(7)]],
                             texture2d<float, access::sample>     gibsGI         [[texture(8)]],
                             texture2d<float, access::sample>     texReflection  [[texture(9)]],
                             texture2d<float, access::sample>     texRefraction  [[texture(10)]],
                             texture2d<float, access::sample>     texShadow      [[texture(11)]],
                             // Top-down cloud transmittance (cloudShadowMap).
                             texture2d<float, access::sample>     cloudShadowTex [[texture(12)]]) {
    // shadeMode: 1 = per-meshlet hashColor (bring-up default / probes / UI toggle),
    //            0 = lambertian fallback (no material bind — bindless caps absent),
    //            2 = full PBR from the shared material table + analytic lights + IBL.
    if (shadeMode == 1u) return float4(in.color, 1.0);

    if (shadeMode != 2u) {
        // Lambertian fallback (mode 0): no material/light binds are dereferenced.
        float3 N = normalize(in.worldNormal);
        float3 L = normalize(float3(0.4, 0.7, 0.5));
        return float4(float3(0.8) * (0.12 + 0.88 * max(dot(N, L), 0.0)), 1.0);
    }

    // ── Mode 2: real PBR (parity with 3d_pbr_normal_mapped.metal's shade) ──
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    MeshletMaterialTexs tex = materialTexs[in.materialID];
    MaterialData material = materials[in.materialID];

    // Prototype UV: triplanar mapping (world or object space) — same block as the
    // forward fragment. 0 = Off, 1 = World Space, 2 = Object Space.
    if (material.prototypeUVMode > 0.5) {
        float3 pos;
        float3 n;
        if (material.prototypeUVMode > 1.5) {
            pos = in.scaledLocalPos;
            n = abs(normalize(in.localNormal));
        } else {
            pos = in.worldPosition;
            n = abs(normalize(in.worldNormal));
        }
        if (n.x > n.y && n.x > n.z)      in.uv = pos.yz * material.uvScale;
        else if (n.y > n.z)              in.uv = pos.xz * material.uvScale;
        else                             in.uv = pos.xy * material.uvScale;
    }

    float4 baseColor = tex.albedo.sample(s, in.uv);
    // glTF MASK cutout (per-material cutoff in emissiveFactor.a; 0 = disabled).
    if (material.emissiveFactor.a > 0.0 &&
        baseColor.a * material.baseColorFactor.a < material.emissiveFactor.a) {
        discard_fragment();
    }

    Surface surf;
    surf.color      = srgbToLinear(baseColor.rgb * material.baseColorFactor.rgb);
    surf.ao         = tex.occlusion.sample(s, in.uv).r * material.occlusionStrength;
    surf.roughness  = tex.roughness.sample(s, in.uv).g * material.roughnessFactor;
    surf.metallic   = tex.metallic.sample(s, in.uv).b * material.metallicFactor;
    surf.emission   = srgbToLinear(tex.emissive.sample(s, in.uv).rgb * material.emissiveFactor.rgb) * material.emissiveStrength;
    surf.subsurface = material.subsurface;
    surf.specular   = material.specular;
    surf.specular_tint = material.specularTint;
    surf.anisotropic = material.anisotropic;
    surf.sheen      = material.sheen;
    surf.sheen_tint = material.sheenTint;
    surf.clearcoat  = material.clearcoat;
    surf.clearcoat_gloss = material.clearcoatGloss;

    // TBN normal mapping (same construction as the PBR fragment).
    float3 N = normalize(in.worldNormal);
    float3 T = normalize(in.worldTangent.xyz);
    T = normalize(T - dot(T, N) * N);
    float3 B = normalize(cross(N, T) * in.worldTangent.w);
    float3x3 TBN = float3x3(T, B, N);
    float3 norm = normalize(TBN * normalize(tex.normal.sample(s, in.uv).rgb * 2.0 - 1.0));
    float3 viewDir = normalize(camera.position - in.worldPosition);
    constexpr sampler screenSampler(address::clamp_to_edge, filter::linear);
    float2 screenUV = in.position.xy / float2(tile.screenW, tile.screenH);

    float3 result = float3(0.0);

    // Directional sun + PSSM shadow, tightened by the screen-space contact
    // shadow (min() = shadowed if either says so, matching the forward pass).
    float3 sunDir = normalize(-dirLights[0].direction);
    float viewZ = abs((camera.view * float4(in.worldPosition, 1.0)).z);
    float ndlSun = max(dot(norm, sunDir), 0.0);
    float shadow = counts.dirShadowOn != 0u
        ? meshletDirShadow(in.worldPosition, ndlSun, viewZ, screenUV, pssmData, pssmShadowMaps, texShadow)
        : 1.0;
    shadow = min(shadow, texSSCS.sample(screenSampler, screenUV).r);
    // Drifting cloud shadows on the sun term (twin of the forward fragment;
    // constants must match cloudShadowMap's region).
    if (envParams.y > 0.0) {
        float2 csCenter = floor(camera.position.xz / 16.0) * 16.0;
        float2 csUV = (in.worldPosition.xz - csCenter) / (2.0 * 2048.0) + 0.5;
        float2 csEdge = abs(csUV - 0.5);
        float csIn = 1.0 - smoothstep(0.45, 0.5, max(csEdge.x, csEdge.y));
        shadow *= mix(1.0, cloudShadowTex.sample(screenSampler, csUV).r, envParams.y * csIn);
    }
    result += CalculateDirectionalLight(dirLights[0], norm, T, B, viewDir, surf) * shadow;

    // Tiled point lights: index the cluster the fragment falls in and shade only
    // that tile's lights (same lookup as the forward pass — was a loop over ALL
    // point lights, which dominated the meshlet main pass). Point/rect/spot
    // shadow ride the stochastic texture's R/G/B channels when that format is
    // active (shadowRGB); otherwise white -> unshadowed.
    float pointShadow = pointShadowTex.sample(screenSampler, screenUV).r;
    float rectShadow  = (flags.shadowRGB != 0u) ? pointShadowTex.sample(screenSampler, screenUV).g : 1.0;
    float spotShadow  = (flags.shadowRGB != 0u) ? pointShadowTex.sample(screenSampler, screenUV).b : 1.0;
    uint tileX = uint(screenUV.x * float(tile.gridX));
    uint tileY = uint((1.0 - screenUV.y) * float(tile.gridY));
    uint tileIndex = tileX + tileY * tile.gridX;
    const device Cluster& cell = clusters[tileIndex];
    for (uint i = 0; i < cell.lightCount; ++i) {
        uint li = cell.lightIndices[i];
        result += CalculatePointLight(pointLights[li], norm, T, B, viewDir, surf, in.worldPosition) * pointShadow;
    }
    for (uint i = 0; i < counts.spotCount; ++i)
        result += CalculateSpotLight(spotLights[i], norm, T, B, viewDir, surf, in.worldPosition) * spotShadow;
    for (uint i = 0; i < counts.rectCount; ++i)
        result += CalculateRectLight(rectLights[i], norm, in.worldPosition, viewDir, surf, rectLightVideo) * rectShadow;

    // Ambient (screen-space AO attenuates indirect only): GIBS GI when enabled,
    // else IBL when the material opts in, else a minimal constant term.
    float screenAO = texAO.sample(screenSampler, screenUV).r;
    if (flags.gibsOn != 0u)
        result += gibsGI.sample(screenSampler, screenUV).rgb * surf.ao * screenAO;
    else if (material.iblEnabled > 0.5)
        result += CalculateIBL(norm, viewDir, surf, irradianceMap, prefilterMap, brdfLUT) * screenAO * envParams.x;
    else
        result += float3(0.03) * surf.ao * surf.color * screenAO;

    // RT mirror reflections (Fresnel-weighted, roughness-faded — same as main).
    if (flags.reflOn > 0.5) {
        float3 refl = texReflection.sample(screenSampler, screenUV).rgb;
        float NdotV = max(dot(norm, viewDir), 0.0);
        float3 F0r = mix(float3(0.04), surf.color, surf.metallic);
        float3 Fr = FresnelSchlickRoughness(NdotV, F0r, surf.roughness);
        float roughFade = (1.0 - surf.roughness) * (1.0 - surf.roughness);
        result += refl * Fr * roughFade * flags.reflIntensity * screenAO;
    }

    // RT refractions (KHR_materials_transmission): mix in the transmitted
    // radiance by the material's transmission factor (same BTDF as main).
    if (flags.refrOn > 0.5 && material.transmission > 0.0) {
        float3 refr = texRefraction.sample(screenSampler, screenUV).rgb;
        float NdotV = max(dot(norm, viewDir), 0.0);
        float3 F0t = mix(float3(0.04), surf.color, surf.metallic);
        float3 Ft = FresnelSchlickRoughness(NdotV, F0t, surf.roughness);
        float roughFade = (1.0 - surf.roughness) * (1.0 - surf.roughness);
        float3 transmitted = refr * surf.color * (1.0 - Ft) * flags.refrIntensity;
        result = mix(result, transmitted, material.transmission * roughFade);
    }

    result += surf.emission;
    return float4(result, 1.0);
}

// MRT output for the meshlet depth pre-pass (mirrors 3d_depth_only.metal's
// PrePassOutput): color(0) = world normal, color(1) = base color. The meshlet
// path has no PBR material bind yet, so albedo carries the per-meshlet debug
// color — enough for downstream passes that only need depth + a normal.
struct MeshletPrePassOutput {
    float4 normal [[color(0)]];
    float4 albedo [[color(1)]];
};

// alphaMask (buffer 0): 1 = alpha-cutout using the shared material table (glTF
// MASK), 0 = no material bind (hashColor albedo, no discard). The cutout MUST
// match the main pass, otherwise the pre-pass writes depth across a leaf's
// transparent holes and occludes whatever is behind them -> the holes render as
// clear color (the reported plant-leaf bug).
fragment MeshletPrePassOutput fragmentPrePass(
    MeshletVertexOut in [[stage_in]],
    constant uint& alphaMask [[buffer(0)]],
    const device MaterialData* materials [[buffer(2)]],
    const device MeshletMaterialTexs* materialTexs [[buffer(13)]]
) {
    MeshletPrePassOutput out;
    out.normal = float4(normalize(in.worldNormal), 1.0);
    if (alphaMask != 0u) {
        constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
        MaterialData m = materials[in.materialID];
        float4 baseColor = materialTexs[in.materialID].albedo.sample(s, in.uv);
        float cutoff = m.emissiveFactor.a;  // .a = MASK alpha cutoff (0 = disabled)
        if (cutoff > 0.0 && baseColor.a * m.baseColorFactor.a < cutoff) {
            discard_fragment();
        }
        out.albedo = float4(baseColor.rgb * m.baseColorFactor.rgb, 1.0);
    } else {
        out.albedo = float4(in.color, 1.0);  // no material bind: per-meshlet color
    }
    return out;
}

#if MESHLET_DEBUG_PROBES
// Lowest-level probe: a MESH-ONLY pipeline (no object stage, no payload, no
// buffer reads at all) emitting one green triangle on the left half of the
// screen. If this rasterizes while the object->mesh synthetic (centered
// magenta/yellow/cyan) doesn't, the amplification chain is at fault; if
// neither shows, drawMeshThreadgroups / encoder state is.
[[mesh]] void meshSynthetic(
    MeshletMeshT output,
    uint tid [[thread_position_in_threadgroup]]
) {
    if (tid == 0u) {
        MeshletVertexOut a, b, c;
        a.position = float4(-0.9, -0.8, 0.5, 1.0); a.color = float3(0.0, 1.0, 0.0);
        b.position = float4(-0.1, -0.8, 0.5, 1.0); b.color = float3(0.0, 1.0, 0.0);
        c.position = float4(-0.5,  0.8, 0.5, 1.0); c.color = float3(0.0, 1.0, 0.0);
        output.set_vertex(0, a);
        output.set_vertex(1, b);
        output.set_vertex(2, c);
        output.set_index(0, 0);
        output.set_index(1, 1);
        output.set_index(2, 2);
        { MeshletPrimOut pr; pr.primColor = float3(1.0); output.set_primitive(0, pr); }
        output.set_primitive_count(1);
    }
}
#endif // MESHLET_DEBUG_PROBES
