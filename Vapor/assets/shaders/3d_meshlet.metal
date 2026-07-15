#include <metal_stdlib>
using namespace metal;

// Meshlet task/mesh pipeline — Metal backend. Mirror of Meshlet.task /
// Meshlet.mesh / MeshletDebug.frag (Vulkan). objectMain culls per meshlet
// (frustum + backface cone + two-sphere cluster-LOD cut) and forwards survivor
// indices via the payload; meshMain expands the meshlet's triangles from the
// merged scene vertex buffer; fragmentMain shows a per-meshlet debug color.
//
// Buffer indices (RHI setVertexBuffer routes to BOTH object and mesh stages for
// mesh pipelines): 0=camera, 2=instances, 3=meshlets, 4=meshletVertices,
// 5=meshletTriangles(u8), 6=meshletBounds, 7=merged VB, 8=params (setVertexBytes).

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float near;
    float far;
    float3 position;
    float4 frustumPlanes[6];
};

struct InstanceData {
    float4x4 model;
    float4 color;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    float3 AABBMin;
    float3 AABBMax;
    float4 boundingSphere;
};

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

// Must match Vapor::VertexData (48 bytes, tightly packed).
struct VertexData {
    packed_float3 position;
    packed_float2 uv;
    packed_float3 normal;
    packed_float4 tangent;
};

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
    threadgroup atomic_uint visibleCount;
    if (tid == 0u) atomic_store_explicit(&visibleCount, 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint local = gid * 32u + tid;
    if (local < params.meshletCount) {
        uint mi = params.meshletOffset + local;
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
        bool visible = true;
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

        if (visible) {
            uint slot = atomic_fetch_add_explicit(&visibleCount, 1u, memory_order_relaxed);
            payload.meshletIndices[slot] = mi;
        }
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tid == 0u) {
        uint count = atomic_load_explicit(&visibleCount, memory_order_relaxed);
        grid.set_threadgroups_per_grid(uint3(count, 1u, 1u));
    }
}

struct MeshletVertexOut {
    float4 position [[position]];
    float3 color;
};

using MeshletMeshT = metal::mesh<MeshletVertexOut, void, 64, 128, metal::topology::triangle>;

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
    if (params.errorThreshold <= -2.5) {
        uint mi = payload.meshletIndices[gid];
        Meshlet m = meshlets[mi];
        float3 c = float3(saturate(float(m.vertexCount) / 64.0),
                          saturate(float(m.triangleCount) / 128.0),
                          float(mi & 255u) / 255.0);
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
            output.set_primitive_count(1);
        }
        return;
    }

    uint mi = payload.meshletIndices[gid];
    Meshlet m = meshlets[mi];

    // Transform in the SAME associativity as the pre-pass / main vertex shader:
    // world = model * pos, clip = proj * view * world. A fused proj*view*model
    // MVP rounds differently and can land fragments epsilon-farther than the
    // pre-pass depth, which the main pass's LessOrEqual test then rejects
    // (whole-cluster dropout -> clear color).
    float4x4 model = instances[params.instanceID].model;
    float4x4 viewProj = cam.proj * cam.view;
    float3 color = hashColor(mi);

    for (uint v = tid; v < m.vertexCount; v += 64u) {
        uint vi = meshletVertices[m.vertexOffset + v];  // merged-VB vertex index
        MeshletVertexOut vout;
        float4 worldPos = model * float4(float3(vertices[vi].position), 1.0);
        vout.position = viewProj * worldPos;
        vout.color = color;
        output.set_vertex(v, vout);
    }
    for (uint t = tid; t < m.triangleCount * 3u; t += 64u) {
        output.set_index(t, meshletTriangles[m.triangleOffset + t]);
    }
    if (tid == 0u) {
        output.set_primitive_count(m.triangleCount);
    }
}

fragment float4 fragmentMain(MeshletVertexOut in [[stage_in]]) {
    return float4(in.color, 1.0);
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
        output.set_primitive_count(1);
    }
}
