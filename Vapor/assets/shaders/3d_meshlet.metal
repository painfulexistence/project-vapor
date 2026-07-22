#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"  // shared CameraData/InstanceData/VertexData + inverse()

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

[[object]] void objectMain(
    object_data MeshletPayload& payload [[payload]],
    mesh_grid_properties grid,
    device const CameraData&    cam       [[buffer(0)]],
    device const InstanceData*  instances [[buffer(2)]],
    device const MeshletBounds* bounds    [[buffer(6)]],
    constant MeshletParams&     params    [[buffer(8)]],
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
        vout.position = viewProj * (model * float4(float3(vertices[vi].position), 1.0));
        vout.color = color;
        vout.worldNormal = normalMatrix * float3(vertices[vi].normal.xyz);
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

fragment float4 fragmentMain(MeshletVertexOut in [[stage_in]],
                             constant uint& debugColor [[buffer(0)]]) {
    // debugColor: 1 = per-meshlet hashColor (bring-up default / probes),
    // 0 = lambertian from the interpolated world normal (a real-geometry shade;
    // full PBR material binding for the meshlet path is a follow-up).
    if (debugColor != 0u) return float4(in.color, 1.0);
    float3 N = normalize(in.worldNormal);
    float3 L = normalize(float3(0.4, 0.7, 0.5));
    float ndl = max(dot(N, L), 0.0);
    return float4(float3(0.8) * (0.12 + 0.88 * ndl), 1.0);
}

// MRT output for the meshlet depth pre-pass (mirrors 3d_depth_only.metal's
// PrePassOutput): color(0) = world normal, color(1) = base color. The meshlet
// path has no PBR material bind yet, so albedo carries the per-meshlet debug
// color — enough for downstream passes that only need depth + a normal.
struct MeshletPrePassOutput {
    float4 normal [[color(0)]];
    float4 albedo [[color(1)]];
};

fragment MeshletPrePassOutput fragmentPrePass(MeshletVertexOut in [[stage_in]]) {
    MeshletPrePassOutput out;
    out.normal = float4(normalize(in.worldNormal), 1.0);
    out.albedo = float4(in.color, 1.0);
    return out;
}

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
