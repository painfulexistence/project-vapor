// Adaptive GPU tessellation — shared CBT/LEB library, GLSL twin of
// 3d_tess_lib.metal (which mirrors Vapor/include/Vapor/cbt.hpp). SAME uint32
// storage layout, node convention and neighbor split rules — the CPU header is
// the tested reference; change them together or the tessellation corrupts.
//
// CBT/roots access: GLSL functions cannot take buffer pointers, so the
// CBT/roots-reading helpers reference fixed-name SSBOs this header declares
// itself when the includer configures their bindings BEFORE the #include:
//   #define TESS_CBT_RO_BINDING 0   // -> readonly uint tessCbt[]
//   #define TESS_ROOTS_BINDING 2    // -> readonly TessRoot tessRoots[]
// Pure bit-walk helpers have no such requirement.
#ifndef TESS_COMMON_GLSL
#define TESS_COMMON_GLSL

// ---- shared GPU structs (C++ mirrors live in Vapor/tessellation.hpp) -------

// One LEB root: three corners (position + uv + normal) and the same-depth
// adjacency of the root triangle (heap ids; 0 = mesh boundary).
struct TessRoot {
    vec4 posU[3];  // xyz = object-space position of corner vi, w = uv.x
    vec4 nrmV[3];  // xyz = object-space normal at corner vi,  w = uv.y
    uint left;     // across edge (v1, v2)
    uint right;    // across edge (v0, v1)
    uint edge;     // across the bisected edge (v2, v0)
    uint node;     // this root's own heap id
};

// Per-tessellated-mesh constants — must match TessParamsGpu (112 bytes). On
// Vulkan they ride a host-visible buffer (the struct exceeds a push slot).
struct TessParams {
    mat4 model;               // object -> world
    uint maxDepth;            // CBT max depth D
    uint rootDepth;           // ceil(log2(rootCount))
    uint rootCount;
    uint maxLeaves;           // TessLeafData capacity (draw clamp + split guard)
    float splitPixels;        // split when the leaf hypotenuse projects larger
    float screenHeight;       // pixels
    float displacementScale;  // 0 = flat; procedural displacement amplitude
    uint flags;               // bit0 = freeze (classify becomes a no-op)
    uint gridIndexCount;      // CPU grid topology size, for the draw args
    uint pad0;
    uint pad1;
    uint pad2;
};

const uint TESS_FLAG_FREEZE = 1u;

struct TessNeighbors {
    uint left;
    uint right;
    uint edge;
    uint node;
};

// Must match Vapor::CameraRenderData (same twin as the RT kernels).
struct TessCameraData {
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

// ---- pure bit helpers ------------------------------------------------------

uint tessDepthOf(uint node) {
    return uint(findMSB(node));  // == 31 - clz(node) for node > 0
}

uint tessBitfieldOffset(uint maxDepth) {
    return (2u << (maxDepth - 5u)) - 1u;
}

// Bitfield position of a node's leftmost ceil descendant (drop the leading 1).
uint tessCeilBitOf(uint node, uint maxDepth) {
    uint d = tessDepthOf(node);
    return (node << (maxDepth - d)) & ((1u << maxDepth) - 1u);
}

// ---- configurable SSBO declarations ----------------------------------------

#ifdef TESS_CBT_RO_BINDING
layout(std430, set = 0, binding = TESS_CBT_RO_BINDING) readonly buffer TessCbtRO { uint tessCbt[]; };
#define TESS_HAS_CBT
#endif
#ifdef TESS_ROOTS_BINDING
layout(std430, set = 0, binding = TESS_ROOTS_BINDING) readonly buffer TessRootBuf { TessRoot tessRoots[]; };
#define TESS_HAS_ROOTS
#endif

// ---- CBT storage access (requires the tessCbt[] SSBO) ----------------------
#ifdef TESS_HAS_CBT

// Subtree leaf count. Counts above the bitfield are as fresh as the last
// reduction; the bottom five levels come straight from the bitfield words.
uint tessHeapRead(uint maxDepth, uint node) {
    if (node == 0u) return 0u;
    uint d = tessDepthOf(node);
    uint s = maxDepth - 5u;
    if (d <= s) {
        return tessCbt[(1u << d) - 1u + (node - (1u << d))];
    }
    uint first = tessCeilBitOf(node, maxDepth);
    uint len = 1u << (maxDepth - d);  // <= 16, word-aligned range
    uint word = tessCbt[tessBitfieldOffset(maxDepth) + (first >> 5u)];
    uint mask = ((1u << len) - 1u) << (first & 31u);
    return uint(bitCount(word & mask));
}

uint tessLeafCount() {
    return tessCbt[0];
}

// leafIndex -> leaf node, in heap order. The depth guard keeps the descent
// from stopping at an ancestor of a lone root in a sparse forest.
uint tessDecodeLeaf(uint maxDepth, uint rootDepth, uint leafIndex) {
    uint node = 1u;
    while (tessDepthOf(node) < rootDepth || tessHeapRead(maxDepth, node) > 1u) {
        uint leftCount = tessHeapRead(maxDepth, node << 1u);
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

#endif // TESS_HAS_CBT

// ---- LEB decoding (pure bit walks, no CBT reads) ---------------------------

// Barycentric corner weights of a node over its root corners, plus the root
// slot. Weights are dyadic — exact in fp32 — so shared edges land on
// bit-identical positions from both sides (crack-free without snapping).
struct TessTriangle {
    vec3 w0;  // weights of corner v0 over (root v0, root v1, root v2)
    vec3 w1;
    vec3 w2;
    uint rootSlot;
};

TessTriangle tessDecodeTriangle(uint node, uint rootDepth) {
    uint lebDepth = tessDepthOf(node) - rootDepth;
    TessTriangle t;
    t.rootSlot = (node >> lebDepth) - (1u << rootDepth);
    t.w0 = vec3(1, 0, 0);
    t.w1 = vec3(0, 1, 0);
    t.w2 = vec3(0, 0, 1);
    for (uint k = lebDepth; k-- > 0u;) {
        uint bit = (node >> k) & 1u;
        vec3 mid = (t.w0 + t.w2) * 0.5;
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

// ---- roots-dependent helpers (require the tessRoots[] SSBO) ----------------
#ifdef TESS_HAS_ROOTS

// Same-depth neighborhood, from the root adjacency through the split rules
// along the node's bit path (mirror of CBT::neighbors + splitNeighborIDs).
TessNeighbors tessNeighborsOf(uint node, uint rootDepth) {
    uint lebDepth = tessDepthOf(node) - rootDepth;
    uint slot = (node >> lebDepth) - (1u << rootDepth);
    TessNeighbors n;
    n.left = tessRoots[slot].left;
    n.right = tessRoots[slot].right;
    n.edge = tessRoots[slot].edge;
    n.node = tessRoots[slot].node;
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

struct TessCorner {
    vec3 pos;  // object space
    vec3 nrm;
    vec2 uv;
};

TessCorner tessCornerFromWeights(vec3 w, uint rootSlot) {
    TessCorner c;
    c.pos = w.x * tessRoots[rootSlot].posU[0].xyz + w.y * tessRoots[rootSlot].posU[1].xyz +
            w.z * tessRoots[rootSlot].posU[2].xyz;
    c.nrm = w.x * tessRoots[rootSlot].nrmV[0].xyz + w.y * tessRoots[rootSlot].nrmV[1].xyz +
            w.z * tessRoots[rootSlot].nrmV[2].xyz;
    c.uv = w.x * vec2(tessRoots[rootSlot].posU[0].w, tessRoots[rootSlot].nrmV[0].w) +
           w.y * vec2(tessRoots[rootSlot].posU[1].w, tessRoots[rootSlot].nrmV[1].w) +
           w.z * vec2(tessRoots[rootSlot].posU[2].w, tessRoots[rootSlot].nrmV[2].w);
    return c;
}

#endif // TESS_HAS_ROOTS

// ---- metric / displacement / debug -----------------------------------------

// Projected size (pixels) of the bounding sphere of a world-space edge — the
// LoD metric, always evaluated on a hypotenuse. Both triangles of a diamond
// share their hypotenuse, so both reach the same split/merge decision.
float tessProjectedPixels(vec3 wa, vec3 wb, TessCameraData cam, float screenHeight) {
    vec3 c = (wa + wb) * 0.5;
    float diameter = distance(wa, wb);
    float d = max(distance(c, cam.position) - diameter * 0.5, cam.nearPlane);
    return diameter / d * cam.proj[1][1] * 0.5 * screenHeight;
}

// Deterministic procedural displacement (edge-consistent: a function of the
// object-space position only). Placeholder until heightmap sampling is wired.
float tessDisplaceAmount(vec3 p, float scale) {
    if (scale == 0.0) return 0.0;
    float h = sin(p.x * 3.1) * cos(p.z * 2.7) +
              0.35 * sin(p.x * 9.3 + p.z * 7.1) * cos(p.y * 4.3);
    return h * scale;
}

vec3 tessHashColor(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x = x ^ (x >> 4u);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return vec3(float(x & 255u), float((x >> 8u) & 255u), float((x >> 16u) & 255u)) / 255.0;
}

// ---- per-leaf grid (8 segments -> 45 vertices, 64 triangles) ---------------
// The same topology is built once on the CPU for the instanced compute path
// (Renderer::buildTessGrid) and emitted per-threadgroup by the mesh shader —
// identical barycentrics, so both paths produce identical geometry.

const uint TESS_GRID_SEGS = 8u;
const uint TESS_GRID_VERTS = 45u;   // (S+1)(S+2)/2
const uint TESS_GRID_TRIS = 64u;    // S^2

// Grid vertex (row r toward apex v1, column c toward v2) -> barycentric
// weights over (v0, v1, v2). Row 0 is the hypotenuse (v0 at c=0, v2 at c=S).
vec3 tessGridBarycentric(uint r, uint c) {
    float fr = float(r) / float(TESS_GRID_SEGS);
    float fc = float(c) / float(TESS_GRID_SEGS);
    return vec3(1.0 - fr - fc, fr, fc);
}

uint tessGridVertexIndex(uint r, uint c) {
    return r * (TESS_GRID_SEGS + 1u) - (r * (r - 1u)) / 2u + c;
}

uvec2 tessGridVertexRC(uint vid) {
    uint r = 0u;
    uint rowLen = TESS_GRID_SEGS + 1u;
    while (vid >= rowLen) {
        vid -= rowLen;
        rowLen -= 1u;
        r += 1u;
    }
    return uvec2(r, vid);
}

// Triangle t -> three grid-vertex indices, wound to match the root triangle's
// orientation in object space (the (c, r) parameter plane is mirrored).
uvec3 tessGridTriangle(uint t) {
    uint r = 0u;
    uint rowTris = 2u * TESS_GRID_SEGS - 1u;
    while (t >= rowTris) {
        t -= rowTris;
        rowTris -= 2u;
        r += 1u;
    }
    uint c = t >> 1u;
    if ((t & 1u) == 0u) {  // upward triangle
        return uvec3(tessGridVertexIndex(r, c),
                     tessGridVertexIndex(r + 1u, c),
                     tessGridVertexIndex(r, c + 1u));
    } else {               // downward triangle
        return uvec3(tessGridVertexIndex(r, c + 1u),
                     tessGridVertexIndex(r + 1u, c),
                     tessGridVertexIndex(r + 1u, c + 1u));
    }
}

#endif // TESS_COMMON_GLSL
