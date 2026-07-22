#pragma once
#include <SDL3/SDL_stdinc.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <glm/glm.hpp>
#include <mutex>
#include <vector>

namespace Vapor {

// ============================================================================
// VoxelWorld — CPU source of truth for a raymarched micro-voxel volume.
//
// Storage is a sparse two-level structure sized for worlds far beyond the
// dense-grid original (Atmospheric's MicroVoxel kept a full gridDim^3 byte
// array; here only occupied 8^3 bricks consume memory):
//
//   page table  : one Uint32 per 8^3-voxel brick cell over the whole grid.
//                 PAGE_EMPTY = air, PAGE_UNIFORM_BIT|mat = solid brick of one
//                 material (no pool slot — terrain interiors collapse to this),
//                 otherwise an index into the brick pool.
//   brick pool  : 576-byte bricks, GPU-layout-compatible (memcpy'd into a
//                 storage buffer): 16 Uint32 occupancy-bitmask words followed
//                 by 512 material bytes. A single linear in-brick index
//                 i = x + y*8 + z*64 addresses both: occupancy bit i&31 of
//                 word i>>5, material byte i.
//   palette     : 256 materials, 8 bytes each (albedo+emission, params) —
//                 the two palette rows of the original, interleaved.
//
// The renderer consumes this data verbatim; takeDirty() hands it the brick
// slots and tables that changed since the last flush, so edits upload
// per-brick instead of re-uploading the volume. Generation can run on worker
// threads chunk-by-chunk (generateColumnChunk) — slot allocation and dirty
// tracking are the only shared state and sit behind one mutex; the pool
// vector is pre-reserved so slot writes never relocate it.
//
// All coordinates here are LOCAL to the volume: voxel (0,0,0)'s min corner is
// the local origin, one voxel spans voxelSize meters. The owning component
// supplies the world-space placement.
// ============================================================================

// Uploaded verbatim as 2 uints per entry; the shader decode contract
// (MicroVoxel.frag / 3d_microvoxel.metal, little-endian byte order) is
//   word0 = r | g<<8 | b<<16 | emission<<24
//   word1 = reflectivity | roughness<<8 | transmission<<16 | ior<<24
struct VoxelMaterial {
    Uint8 r = 0, g = 0, b = 0;
    Uint8 emission = 0;      // 0..255 self-illumination, scaled by emissiveStrength
    Uint8 reflectivity = 0;  // 0..255 mirror F0 (opaque path; glass derives F from ior)
    Uint8 roughness = 0;     // 0..255 glossy jitter: blurs reflection AND refraction (frosted)
    Uint8 transmission = 0;  // 0..255 dielectric transmission; >0 = the BTDF (glass) path
    Uint8 ior = 0;           // index of refraction, encoded (actual = 1.0 + ior/255), so 1.0..2.0
};
static_assert(sizeof(VoxelMaterial) == 8, "GPU palette layout: 8 bytes per material");

class VoxelWorld {
public:
    static constexpr int BRICK_DIM = 8;
    static constexpr int BRICK_VOXELS = BRICK_DIM * BRICK_DIM * BRICK_DIM;
    static constexpr Uint32 PAGE_EMPTY = 0xFFFFFFFFu;
    static constexpr Uint32 PAGE_UNIFORM_BIT = 0x80000000u;
    // x/z footprint of one generation job (full-height column block).
    static constexpr int GEN_CHUNK_DIM = 64;

    struct Brick {
        std::array<Uint32, 16> occupancy = {};   // bit i&31 of word i>>5, i = x + y*8 + z*64
        std::array<Uint8, BRICK_VOXELS> materials = {};  // palette index per voxel, 0 = air
    };
    static_assert(sizeof(Brick) == 576, "GPU brick pool layout: 16 occupancy words + 512 material bytes");

    // Dirty state handed to the renderer each frame. Brick slots are deduped.
    struct DirtyBatch {
        bool pageTable = false;
        bool palette = false;
        std::vector<Uint32> brickSlots;
    };

    // Default demo materials (index 0 = air), matching the original's values.
    enum : Uint8 {
        MatGrass = 1,
        MatDirt = 2,
        MatStone = 3,
        MatSnow = 4,
        MatSand = 5,
        MatOre = 6,
        MatCrystal = 7,
        MatGlow = 8,
    };

    VoxelWorld() = default;

    // gridDim components are rounded down to multiples of BRICK_DIM.
    // brickCapacity bounds pool memory (brickCapacity * 576 bytes CPU + GPU).
    void configure(glm::ivec3 gridDimIn, float voxelSizeIn, Uint32 brickCapacityIn);

    // ---- Generation ------------------------------------------------------
    // prepareGeneration resets all storage, builds the default palette and the
    // deterministic feature placements (crystal/glowstone spheres) for `seed`.
    // generateColumnChunk fills one GEN_CHUNK_DIM x gridDim.y x GEN_CHUNK_DIM
    // column block; chunks are disjoint, so any number may run concurrently on
    // worker threads. generate() = prepare + every chunk, single-threaded.
    void prepareGeneration(Uint32 seedIn);
    void generateColumnChunk(int chunkX, int chunkZ);
    void generate(Uint32 seedIn);
    glm::ivec2 columnChunkCount() const;

    // ---- Editing (local meters: volume min corner = origin) --------------
    // Clears voxels to air inside the sphere, updating occupancy bits, freeing
    // emptied bricks and materializing carved uniform bricks. Affected slots
    // land in the dirty batch. Returns true if anything changed.
    bool carveSphere(const glm::vec3& localCenter, float radius);

    // First solid voxel along a local-space ray (three-level DDA: empty brick
    // cells are skipped in one step, occupied bricks walk their bitmask).
    // On hit fills the local-space hit position (entry face) and the hit cell.
    bool raycast(const glm::vec3& localRo, const glm::vec3& localRd, float maxDist,
                 glm::vec3& outLocalHit, glm::ivec3& outCell) const;

    // ---- Queries ---------------------------------------------------------
    Uint8 voxelAt(const glm::ivec3& cell) const;
    bool isInside(const glm::ivec3& cell) const {
        return cell.x >= 0 && cell.y >= 0 && cell.z >= 0 && cell.x < gridDim.x && cell.y < gridDim.y
            && cell.z < gridDim.z;
    }
    const glm::ivec3& dim() const { return gridDim; }
    glm::ivec3 brickGrid() const { return gridDim / BRICK_DIM; }
    glm::vec3 extent() const { return glm::vec3(gridDim) * voxelSize; }
    float voxelSizeMeters() const { return voxelSize; }
    Uint32 capacity() const { return brickCapacity; }
    Uint64 solidVoxels() const { return solidCount.load(std::memory_order_relaxed); }
    Uint32 residentBricks() const;
    Uint64 droppedBricks() const { return droppedCount.load(std::memory_order_relaxed); }
    // Surface height of the generated terrain at a voxel column, in voxel
    // units — a pure function of (x, z, seed), valid the moment
    // prepareGeneration has run (no chunks needed). Lets gameplay place
    // things on the surface (the demo's quad flora) without sampling voxels.
    float terrainHeight(int x, int z) const;

    // ---- Renderer access -------------------------------------------------
    const std::vector<Uint32>& pageTableData() const { return pageTable; }
    const Brick& brick(Uint32 slot) const { return bricks[slot]; }
    Uint32 brickPoolSize() const { return static_cast<Uint32>(bricks.size()); }
    const std::array<VoxelMaterial, 256>& paletteData() const { return palette; }
    std::array<VoxelMaterial, 256>& editablePalette() {
        markPaletteDirty();
        return palette;
    }
    // Moves the accumulated dirty state out (thread-safe against generation).
    DirtyBatch takeDirty();
    bool hasDirty() const;

private:
    struct FeatureSphere {
        glm::vec3 center;  // voxels
        float radius = 0.0f;  // voxels
        Uint8 material = 0;
    };

    size_t pageIndex(const glm::ivec3& brickCell) const {
        const glm::ivec3 bg = brickGrid();
        return (static_cast<size_t>(brickCell.z) * bg.y + brickCell.y) * bg.x + brickCell.x;
    }
    static int voxelIndexInBrick(const glm::ivec3& local) {
        return local.x + local.y * BRICK_DIM + local.z * BRICK_DIM * BRICK_DIM;
    }
    static bool occupancyBit(const Brick& b, int i) {
        return (b.occupancy[static_cast<size_t>(i) >> 5] >> (static_cast<Uint32>(i) & 31u)) & 1u;
    }

    void setDefaultPalette();
    // Allocates a pool slot (mutex-held by caller); PAGE_EMPTY when exhausted.
    Uint32 allocSlotLocked();
    void freeSlotLocked(Uint32 slot);
    void markBrickDirtyLocked(Uint32 slot);
    void markPaletteDirty();
    // Fetches the brick a cell lives in for editing, materializing uniform
    // entries into pool slots. Returns PAGE_EMPTY for air cells that stay air.
    Uint32 resolveForEdit(const glm::ivec3& brickCell);

    glm::ivec3 gridDim = glm::ivec3(256);
    float voxelSize = 0.05f;
    Uint32 brickCapacity = 262144;
    Uint32 seed = 1337u;

    std::vector<Uint32> pageTable;
    std::vector<Brick> bricks;
    std::vector<Uint32> freeSlots;
    std::array<VoxelMaterial, 256> palette = {};

    std::vector<FeatureSphere> features;      // crystals + glowstone, in voxels
    std::atomic<Uint64> solidCount { 0 };
    std::atomic<Uint64> droppedCount { 0 };   // bricks lost to pool exhaustion

    mutable std::mutex poolMutex;  // guards freeSlots, bricks growth, dirty state
    std::vector<Uint32> dirtyBrickSlots;
    std::vector<bool> brickDirtyFlags;  // slot -> already in dirtyBrickSlots
    bool pageTableDirty = false;
    bool paletteDirty = false;
};

}  // namespace Vapor
