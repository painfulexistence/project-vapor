#ifndef TESS_LIB_METAL
#define TESS_LIB_METAL

// ============================================================================
// Adaptive GPU tessellation — shared CBT/LEB library (include-only).
//
// MSL mirror of Vapor/include/Vapor/cbt.hpp: SAME uint32 storage layout
// (count levels above a bitfield, bottom five levels served by popcount),
// SAME node convention (triangle (v0,v1,v2), bisected edge (v2,v0), children
// bit0 = (v0,m,v1) / bit1 = (v1,m,v2)), SAME neighbor split rules. The CPU
// header is the tested reference — change them together or the tessellation
// corrupts. Included by 3d_tess_update.metal (compute), 3d_tess_render.metal
// (vertex path) and 3d_tess_mesh.metal (object/mesh path).
//
// Requires 3d_common.metal (CameraData) to be included first.
// ============================================================================

// ---- shared GPU structs (C++ mirrors live in Vapor/tessellation.hpp) -------

// One LEB root: three corners (position + uv + normal) and the same-depth
// adjacency of the root triangle (heap ids; 0 = mesh boundary).
struct TessRoot {
    float4 posU[3];  // xyz = object-space position of corner vi, w = uv.x
    float4 nrmV[3];  // xyz = object-space normal at corner vi,  w = uv.y
    uint left;       // across edge (v1, v2)
    uint right;      // across edge (v0, v1)
    uint edge;       // across the bisected edge (v2, v0)
    uint node;       // this root's own heap id
};

// Per-tessellated-mesh constants (setComputeBytes / setVertexBytes, 112 B).
struct TessParams {
    float4x4 model;           // object -> world
    uint maxDepth;            // CBT max depth D
    uint rootDepth;           // ceil(log2(rootCount))
    uint rootCount;
    uint maxLeaves;           // TessLeafData capacity (draw clamp + split guard)
    float splitPixels;        // split when the leaf hypotenuse projects larger
    float screenHeight;       // pixels
    float displacementScale;  // 0 = flat; displacement amplitude (terrain: heightScale)
    uint flags;               // bit0 = freeze, bit1 = terrain heightfield displacement
    uint gridIndexCount;      // CPU grid topology size, for the draw args
    float terrainFrequency;   // TESS_FLAG_TERRAIN: OpenSimplex2 FBm frequency
    uint terrainSeed;         // ... noise seed
    uint terrainOctaves;      // ... octave count
};

constant uint TESS_FLAG_FREEZE = 1u;
constant uint TESS_FLAG_TERRAIN = 2u;

// GPU-written indirect args + counters, one per tessellated mesh (64 B).
// Field offsets are load-bearing (dispatchIndirect/drawIndexedIndirect/
// drawMeshTasksIndirect read at fixed offsets — see tessellation.hpp).
struct TessArgs {
    uint classify[3];   // offset  0: next frame's classify dispatch groups
    uint leafPrep[3];   // offset 12: this frame's leafPrep dispatch groups
    uint draw[5];       // offset 24: DrawCommand {indexCount, instanceCount,
                        //            firstIndex, vertexOffset, firstInstance}
    uint meshTasks[3];  // offset 44: task-shader grid for the mesh path
    uint leafCount;     // offset 56
    uint pad;
};

// Per-leaf corner cache for the compute (instanced vertex-pull) path: the
// leafPrep kernel decodes each leaf once so the vertex shader only lerps.
struct TessLeafData {
    float4 posU[3];  // xyz = object-space corner, w = uv.x
    float4 nrmV[3];  // xyz = corner normal, w = uv.y
    uint visible;    // 0 = frustum-culled (VS emits a degenerate triangle)
    uint depth;      // heap depth (debug coloring)
    uint node;       // heap id (debug)
    uint pad;
};

struct TessNeighbors {
    uint left;
    uint right;
    uint edge;
    uint node;
};

// ---- CBT storage access ----------------------------------------------------

inline uint tessDepthOf(uint node) {
    return 31u - clz(node);
}

inline uint tessBitfieldOffset(uint maxDepth) {
    return (2u << (maxDepth - 5u)) - 1u;
}

// Bitfield position of a node's leftmost ceil descendant (drop the leading 1).
inline uint tessCeilBitOf(uint node, uint maxDepth) {
    uint d = tessDepthOf(node);
    return (node << (maxDepth - d)) & ((1u << maxDepth) - 1u);
}

// Subtree leaf count. Counts above the bitfield are as fresh as the last
// reduction; the bottom five levels come straight from the bitfield words.
inline uint tessHeapRead(device const uint* cbt, uint maxDepth, uint node) {
    if (node == 0u) return 0u;
    uint d = tessDepthOf(node);
    uint s = maxDepth - 5u;
    if (d <= s) {
        return cbt[(1u << d) - 1u + (node - (1u << d))];
    }
    uint first = tessCeilBitOf(node, maxDepth);
    uint len = 1u << (maxDepth - d);  // <= 16, word-aligned range
    uint word = cbt[tessBitfieldOffset(maxDepth) + (first >> 5u)];
    uint mask = ((1u << len) - 1u) << (first & 31u);
    return popcount(word & mask);
}

inline uint tessLeafCount(device const uint* cbt) {
    return cbt[0];
}

// leafIndex -> leaf node, in heap order. The depth guard keeps the descent
// from stopping at an ancestor of a lone root in a sparse forest.
inline uint tessDecodeLeaf(device const uint* cbt, uint maxDepth, uint rootDepth, uint leafIndex) {
    uint node = 1u;
    while (tessDepthOf(node) < rootDepth || tessHeapRead(cbt, maxDepth, node) > 1u) {
        uint leftCount = tessHeapRead(cbt, maxDepth, node << 1u);
        if (leafIndex < leftCount) {
            node = node << 1u;
        } else {
            leafIndex -= leftCount;
            node = node << 1u | 1u;
        }
        if (tessDepthOf(node) >= maxDepth) break;  // safety against racy reads
    }
    return node;
}

// ---- LEB decoding (pure bit walks, no CBT reads) ---------------------------

// Barycentric corner weights of a node over its root corners, plus the root
// slot. Weights are dyadic — exact in fp32 — so shared edges land on
// bit-identical positions from both sides (crack-free without snapping).
struct TessTriangle {
    float3 w0;  // weights of corner v0 over (root v0, root v1, root v2)
    float3 w1;
    float3 w2;
    uint rootSlot;
};

inline TessTriangle tessDecodeTriangle(uint node, uint rootDepth) {
    uint lebDepth = tessDepthOf(node) - rootDepth;
    TessTriangle t;
    t.rootSlot = (node >> lebDepth) - (1u << rootDepth);
    t.w0 = float3(1, 0, 0);
    t.w1 = float3(0, 1, 0);
    t.w2 = float3(0, 0, 1);
    for (uint k = lebDepth; k-- > 0u;) {
        uint bit = (node >> k) & 1u;
        float3 mid = (t.w0 + t.w2) * 0.5;
        if (bit == 0u) {
            t.w2 = t.w1;
            t.w1 = mid;
        } else {
            t.w0 = t.w1;
            t.w1 = mid;
        }
    }
    return t;
}

// Same-depth neighborhood, from the root adjacency through the split rules
// along the node's bit path (mirror of CBT::neighbors + splitNeighborIDs).
inline TessNeighbors tessNeighbors(uint node, uint rootDepth, device const TessRoot* roots) {
    uint lebDepth = tessDepthOf(node) - rootDepth;
    uint slot = (node >> lebDepth) - (1u << rootDepth);
    TessNeighbors n;
    n.left = roots[slot].left;
    n.right = roots[slot].right;
    n.edge = roots[slot].edge;
    n.node = roots[slot].node;
    for (uint k = lebDepth; k-- > 0u;) {
        uint bit = (node >> k) & 1u;
        uint b2 = (n.right != 0u) ? 1u : 0u;
        uint b3 = (n.edge != 0u) ? 1u : 0u;
        TessNeighbors m;
        if (bit == 0u) {
            m.left = n.node << 1u | 1u;
            m.right = n.edge << 1u | b3;
            m.edge = n.right << 1u | b2;
            m.node = n.node << 1u;
        } else {
            m.left = n.edge << 1u;
            m.right = n.node << 1u;
            m.edge = n.left << 1u;
            m.node = n.node << 1u | 1u;
        }
        n = m;
    }
    return n;
}

// ---- corner attributes / metric / displacement -----------------------------

struct TessCorner {
    float3 pos;  // object space
    float3 nrm;
    float2 uv;
};

inline TessCorner tessCornerFromWeights(float3 w, uint rootSlot, device const TessRoot* roots) {
    device const TessRoot& r = roots[rootSlot];
    TessCorner c;
    c.pos = w.x * r.posU[0].xyz + w.y * r.posU[1].xyz + w.z * r.posU[2].xyz;
    c.nrm = w.x * r.nrmV[0].xyz + w.y * r.nrmV[1].xyz + w.z * r.nrmV[2].xyz;
    c.uv = w.x * float2(r.posU[0].w, r.nrmV[0].w) +
           w.y * float2(r.posU[1].w, r.nrmV[1].w) +
           w.z * float2(r.posU[2].w, r.nrmV[2].w);
    return c;
}

// Projected size (pixels) of the bounding sphere of a world-space edge — the
// LoD metric, always evaluated on a hypotenuse. Both triangles of a diamond
// share their hypotenuse, so both reach the same split/merge decision.
inline float tessProjectedPixels(float3 wa, float3 wb, device const CameraData& cam,
                                 float screenHeight) {
    float3 c = (wa + wb) * 0.5;
    float diameter = distance(wa, wb);
    float d = max(distance(c, cam.position) - diameter * 0.5, cam.near);
    return diameter / d * cam.proj[1][1] * 0.5 * screenHeight;
}

// Deterministic procedural displacement (edge-consistent: a function of the
// object-space position only). The generic placeholder until heightmap
// sampling is wired; terrain instances use the heightfield path below.
inline float tessDisplaceAmount(float3 p, float scale) {
    if (scale == 0.0) return 0.0;
    float h = sin(p.x * 3.1) * cos(p.z * 2.7) +
              0.35 * sin(p.x * 9.3 + p.z * 7.1) * cos(p.y * 4.3);
    return h * scale;
}

// ---- terrain heightfield displacement (TESS_FLAG_TERRAIN) ------------------
// The streamed-terrain height source (FastNoiseLite OpenSimplex2 FBm — the
// SAME field TerrainWorld::heightAt and the Main pass's terrain branch
// evaluate), driving true displaced tessellation: TerrainSystem submits a
// flat world-spanning plane with an IDENTITY model (object == world, which
// keeps displacement a function of the undisplaced position only —
// crack-free by construction) and the CBT refines it adaptively.
// Requires 3d_terrain_noise.metal (trhHeightAt) to be included first, the
// same top-level-include contract as 3d_common.metal above.

// Height (metres, = world y) of the terrain field under object-space p.
inline float tessTerrainHeight(float3 p, constant TessParams& params) {
    // Full fidelity (lodOctaves == octaves): the tessellated surface IS the
    // geometry, so it must match the CPU height field exactly — the fragment
    // stage's footprint-based octave LOD does not apply here.
    return trhHeightAt(p.xz, params.terrainFrequency, int(params.terrainOctaves),
                       float(params.terrainOctaves),
                       params.terrainSeed, params.displacementScale);
}

// Displaced terrain vertex: position lifted onto the heightfield, a
// central-difference normal, and normalized height. d = 1 m matches the LOD0
// heightmap texel spacing the original demo derived normals from. The
// detail-layer texturing itself is per-fragment (tessTerrainSplat below).
struct TessTerrainVertex {
    float3 pos;
    float3 nrm;
    float height01;
};
inline TessTerrainVertex tessTerrainDisplace(float3 pos, constant TessParams& params) {
    const float d = 1.0;
    float h  = tessTerrainHeight(pos, params);
    float hl = tessTerrainHeight(pos - float3(d, 0.0, 0.0), params);
    float hr = tessTerrainHeight(pos + float3(d, 0.0, 0.0), params);
    float hb = tessTerrainHeight(pos - float3(0.0, 0.0, d), params);
    float ht = tessTerrainHeight(pos + float3(0.0, 0.0, d), params);
    TessTerrainVertex v;
    v.pos = float3(pos.x, pos.y + h, pos.z);
    v.nrm = normalize(float3(hl - hr, 2.0 * d, hb - ht));
    v.height01 = clamp(h / max(params.displacementScale, 1e-3), 0.0, 1.0);
    return v;
}

// ── Terrain splat (MSL twin of RHIMain.frag's shadeTerrain) ─────────────────
// Per-layer world-space frequency (repeats/metre): grass 4 m, rock ~21 m,
// dirt 8 m, snow ~13 m. Weights recomputed per fragment from height/slope +
// world-space value-noise FBm breakup (defaultSplat rules), 4 detail layers
// tiled in world space, tangent-space detail normals perturbing the surface
// normal — identical to the streamed-tile Main-pass look.
constant float4 kTessLayerFreq = float4(0.25, 0.046875, 0.125, 0.078125);
constant uint kTessSplatSeed = 7u;

inline uint tessSplatHash2(int x, int y, uint seed) {
    uint h = uint(x) * 374761393u + uint(y) * 668265263u + seed * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float tessSplatHash01(int x, int y, uint seed) {
    return float(tessSplatHash2(x, y, seed) >> 8) * (1.0 / 16777216.0);
}
inline float tessSplatSmooth(float t) { return t * t * (3.0 - 2.0 * t); }
static float tessSplatFBm(float2 p, float wavelength, int octaves, uint seed) {
    float sum = 0.0, amp = 0.5, freq = 1.0 / wavelength;
    for (int k = 0; k < octaves; ++k) {
        float u = p.x * freq, v = p.y * freq;
        int xi = int(floor(u)), yi = int(floor(v));
        float fx = tessSplatSmooth(u - float(xi)), fy = tessSplatSmooth(v - float(yi));
        uint s = seed + uint(k) * 131u;
        float a = tessSplatHash01(xi, yi, s),     b = tessSplatHash01(xi + 1, yi, s);
        float c = tessSplatHash01(xi, yi + 1, s), d = tessSplatHash01(xi + 1, yi + 1, s);
        sum += amp * mix(mix(a, b, fx), mix(c, d, fx), fy);
        amp *= 0.5; freq *= 2.0;
    }
    return sum;
}
inline float4 tessSplatWeights(float height01, float slope, float2 worldXZ) {
    float b1 = tessSplatFBm(worldXZ, 180.0, 3, kTessSplatSeed);
    float b2 = tessSplatFBm(worldXZ, 45.0, 3, kTessSplatSeed + 101u);
    float rock = smoothstep(0.55, 1.05, slope + 0.25 * (b1 - 0.5));
    float snowline = 0.62 + 0.08 * (b2 - 0.5);
    float snow = smoothstep(snowline, snowline + 0.16, height01) * (1.0 - 0.85 * rock);
    float dirt = 0.55 * smoothstep(0.5, 0.75, b2) * smoothstep(0.18, 0.45, slope + 0.2 * (b1 - 0.5));
    dirt += 0.6 * smoothstep(0.10, 0.04, height01);
    dirt = clamp(dirt, 0.0, 1.0) * (1.0 - rock) * (1.0 - snow);
    float grass = max(1.0 - rock - snow - dirt, 0.0);
    float4 w = float4(grass, rock, dirt, snow);
    return w / max(w.x + w.y + w.z + w.w, 1e-4);
}
// Detail-layer albedo (linear) + a perturbed world normal from the surface
// normal N and the fragment's world XZ / height. detailAlbedo/detailNormal are
// the two 2D arrays the Main pass samples; sampler repeats + trilinear.
inline float3 tessTerrainSplat(float3 worldXZY, float height01, thread float3& N,
                               texture2d_array<float, access::sample> detailAlbedo,
                               texture2d_array<float, access::sample> detailNormal) {
    constexpr sampler ts(address::repeat, filter::linear, mip_filter::linear);
    float slope = length(N.xz) / max(N.y, 1e-3);
    float4 w = tessSplatWeights(height01, slope, worldXZY.xz);
    float3 c = float3(0.0);
    float3 dn = float3(0.0);
    for (int i = 0; i < 4; ++i) {
        float2 uv = worldXZY.xz * kTessLayerFreq[i];
        c  += w[i] * pow(detailAlbedo.sample(ts, uv, i).rgb, float3(2.2));
        dn += w[i] * (detailNormal.sample(ts, uv, i).xyz * 2.0 - 1.0);
    }
    dn = normalize(dn + float3(0.0, 0.0, 1e-4));
    float3 T = normalize(float3(1.0, 0.0, 0.0) - N * N.x);
    float3 B = cross(N, T);
    N = normalize(T * dn.x + B * dn.y + N * dn.z);
    return c;
}

inline float3 tessHashColor(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x = x ^ (x >> 4u);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return float3(float(x & 255u), float((x >> 8u) & 255u), float((x >> 16u) & 255u)) / 255.0;
}

// ---- per-leaf grid (8 segments -> 45 vertices, 64 triangles) ---------------
// The same topology is built once on the CPU for the instanced compute path
// (Renderer::buildTessGrid) and emitted per-threadgroup by the mesh shader —
// identical barycentrics, so both paths produce identical geometry.

constant uint TESS_GRID_SEGS = 8u;
constant uint TESS_GRID_VERTS = 45u;   // (S+1)(S+2)/2
constant uint TESS_GRID_TRIS = 64u;    // S^2

// Grid vertex (row r toward apex v1, column c toward v2) -> barycentric
// weights over (v0, v1, v2). Row 0 is the hypotenuse (v0 at c=0, v2 at c=S).
inline float3 tessGridBarycentric(uint r, uint c) {
    float fr = float(r) / float(TESS_GRID_SEGS);
    float fc = float(c) / float(TESS_GRID_SEGS);
    return float3(1.0 - fr - fc, fr, fc);
}

inline uint tessGridVertexIndex(uint r, uint c) {
    return r * (TESS_GRID_SEGS + 1u) - (r * (r - 1u)) / 2u + c;
}

inline uint2 tessGridVertexRC(uint vid) {
    uint r = 0u;
    uint rowLen = TESS_GRID_SEGS + 1u;
    while (vid >= rowLen) {
        vid -= rowLen;
        rowLen -= 1u;
        r += 1u;
    }
    return uint2(r, vid);
}

// Triangle t -> three grid-vertex indices, wound to match the root triangle's
// orientation in object space (the (c, r) parameter plane is mirrored).
inline uint3 tessGridTriangle(uint t) {
    uint r = 0u;
    uint rowTris = 2u * TESS_GRID_SEGS - 1u;
    while (t >= rowTris) {
        t -= rowTris;
        rowTris -= 2u;
        r += 1u;
    }
    uint c = t >> 1u;
    if ((t & 1u) == 0u) {  // upward triangle
        return uint3(tessGridVertexIndex(r, c),
                     tessGridVertexIndex(r + 1u, c),
                     tessGridVertexIndex(r, c + 1u));
    } else {               // downward triangle
        return uint3(tessGridVertexIndex(r, c + 1u),
                     tessGridVertexIndex(r + 1u, c),
                     tessGridVertexIndex(r + 1u, c + 1u));
    }
}

#endif // TESS_LIB_METAL
