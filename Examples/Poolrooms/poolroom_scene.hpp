#pragma once
// ============================================================================
// Poolrooms — scene assembly (engine-agnostic: glm + std only).
//
// Builds the whole environment out of the poolroom_mesh toolkit and returns
// it as per-material geometry buckets plus collision boxes and gameplay
// metadata. main.cpp converts the buckets into engine meshes/materials and
// bodies — nothing in here touches the engine, so the layout is unit-testable
// on its own.
//
// Layout (meters, Y up, deck surface at y = 0):
//   * Hall interior 24 x 10, ceiling at 4.6, tiled walls and deck.
//   * Sunken pool 14 x 6 in the middle, floor at -2.35, waterline at -0.35,
//     bullnose coping around the rim, mosaic accent band at the waterline.
//   * Six skylight wells (2 x 3 grid) punched through the ceiling — the sun
//     and sky light the room through them.
//   * Six tiled pillars on the north/south decks.
//   * Two doorways in the west wall into a dark corridor stub.
//   * Bent-tube handrail entries, a mosaic stair block descending into the
//     water, and two glossy tube slides dropping from the north wall.
// ============================================================================

#include "poolroom_mesh.hpp"
#include <array>

namespace poolgen {

// One geometry bucket per material so main.cpp can assign textures per slot.
enum class MaterialSlot : int {
    DeckTile = 0,
    WallTile,
    CeilingPlaster,
    PoolWallTile,
    PoolFloorTile,
    AccentTile,   // waterline mosaic band
    Grout,        // recessed channels + cove trim
    Coping,       // bullnose stone around the rim
    DarkPlaster,  // corridor stub
    Metal,        // handrails + lamp rims
    LampGlow,     // porthole lamp faces (emissive)
    SlideBlue,    // water-slide tube shells
    SlideViolet,
    Count
};

inline const char* materialSlotName(MaterialSlot s) {
    switch (s) {
        case MaterialSlot::DeckTile: return "deck_tile";
        case MaterialSlot::WallTile: return "wall_tile";
        case MaterialSlot::CeilingPlaster: return "ceiling_plaster";
        case MaterialSlot::PoolWallTile: return "pool_wall_tile";
        case MaterialSlot::PoolFloorTile: return "pool_floor_tile";
        case MaterialSlot::AccentTile: return "accent_tile";
        case MaterialSlot::Grout: return "grout";
        case MaterialSlot::Coping: return "coping";
        case MaterialSlot::DarkPlaster: return "dark_plaster";
        case MaterialSlot::Metal: return "metal";
        case MaterialSlot::LampGlow: return "lamp_glow";
        case MaterialSlot::SlideBlue: return "slide_blue";
        case MaterialSlot::SlideViolet: return "slide_violet";
        default: return "unknown";
    }
}

struct PoolroomScene {
    std::array<MeshData, static_cast<size_t>(MaterialSlot::Count)> buckets;
    std::vector<CollisionBox> colliders;

    // Gameplay / rendering metadata.
    glm::vec3 spawnPosition{ -10.2f, 0.9f, 4.1f };  // capsule CENTER (feet on deck)
    float spawnYawDeg = -68.0f;                     // facing the pool
    float waterLevel = -0.35f;
    glm::vec2 poolMinXZ{ -7.0f, -3.0f };
    glm::vec2 poolMaxXZ{ 7.0f, 3.0f };
    float poolFloorY = -2.35f;
    float deckY = 0.0f;
    float ceilingY = 4.6f;
    // Ladder climb volumes (AABBs) — main.cpp turns these into climb zones.
    std::vector<std::pair<glm::vec3, glm::vec3>> ladderZones;  // (min, max)
    // Skylight well centers (X, Z) and half sizes — used to aim light shafts.
    std::vector<glm::vec4> skylights;  // x, z, halfX, halfZ
    // Porthole wall lamps: position + outward normal. main.cpp attaches a warm
    // point light to each; the glow disc itself is emissive geometry, so it
    // shows up in the water's planar reflection.
    std::vector<std::pair<glm::vec3, glm::vec3>> lamps;

    MeshData& bucket(MaterialSlot s) { return buckets[static_cast<size_t>(s)]; }
};

// ── Assembly ────────────────────────────────────────────────────────────────

inline PoolroomScene buildPoolroomScene() {
    PoolroomScene S;

    // Master dimensions.
    const float hallMinX = -12.0f, hallMaxX = 12.0f;
    const float hallMinZ = -5.0f, hallMaxZ = 5.0f;
    const float ceilY = S.ceilingY;                     // 4.6
    const float poolMinX = S.poolMinXZ.x, poolMaxX = S.poolMaxXZ.x;
    const float poolMinZ = S.poolMinXZ.y, poolMaxZ = S.poolMaxXZ.y;
    const float poolFloorY = S.poolFloorY;              // -2.35

    // Coping: proud of the deck by `copingProud`, quarter-round r, deck-side
    // lip and a skirt down to the accent band.
    const float copingProud = 0.012f;
    const float copingR = 0.06f;
    const float copingLip = 0.16f;
    const float copingSkirt = 0.30f;                    // skirt bottom ~ -0.36
    const float skirtBottomY = copingProud - copingR - copingSkirt;  // ≈ -0.348
    const float accentBandH = 0.30f;
    const float accentTopY = skirtBottomY;              // band starts under the skirt
    const float accentBottomY = accentTopY - accentBandH;

    TilePanelDesc deckTiles;    // 20 cm square tile, recessed grout
    deckTiles.tileW = 0.20f; deckTiles.tileH = 0.20f;
    TilePanelDesc wallTiles = deckTiles;
    TilePanelDesc poolTiles = deckTiles;
    TilePanelDesc accentTiles;  // 10 cm mosaic
    accentTiles.tileW = 0.10f; accentTiles.tileH = 0.10f;
    accentTiles.groutWidth = 0.006f;
    accentTiles.groutDepth = 0.0035f;
    accentTiles.bevel = 0.004f;

    auto tilePhase = [](float worldCoord, float tileSize) {
        float f = worldCoord / tileSize;
        f -= std::floor(f);
        return f;
    };

    MeshData& deck = S.bucket(MaterialSlot::DeckTile);
    MeshData& wall = S.bucket(MaterialSlot::WallTile);
    MeshData& ceil = S.bucket(MaterialSlot::CeilingPlaster);
    MeshData& poolWall = S.bucket(MaterialSlot::PoolWallTile);
    MeshData& poolFloor = S.bucket(MaterialSlot::PoolFloorTile);
    MeshData& accent = S.bucket(MaterialSlot::AccentTile);
    MeshData& grout = S.bucket(MaterialSlot::Grout);
    MeshData& coping = S.bucket(MaterialSlot::Coping);
    MeshData& dark = S.bucket(MaterialSlot::DarkPlaster);
    MeshData& metal = S.bucket(MaterialSlot::Metal);

    // ── Deck (floor around the pool) ── frame U=+Z, V=+X → normal +Y.
    // The tile lattice is phased so it stays continuous across the 4 panels.
    auto deckPanel = [&](float x0, float z0, float x1, float z1) {
        TilePanelDesc d = deckTiles;
        d.width = z1 - z0;
        d.height = x1 - x0;
        d.uPhase = tilePhase(z0, d.tileW);
        d.vPhase = tilePhase(x0, d.tileH);
        buildTilePanel(glm::vec3(x0, 0.0f, z0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0),
                       d, deck, grout);
    };
    deckPanel(hallMinX, poolMaxZ, hallMaxX, hallMaxZ);   // north
    deckPanel(hallMinX, hallMinZ, hallMaxX, poolMinZ);   // south
    deckPanel(hallMinX, poolMinZ, poolMinX, poolMaxZ);   // west
    deckPanel(poolMaxX, poolMinZ, hallMaxX, poolMaxZ);   // east

    // ── Hall walls ── tiled floor-to-ceiling; the west wall has 2 doorways.
    const float doorW = 1.6f, doorH = 2.4f;
    const float doorZ0a = -2.3f, doorZ0b = 0.7f;  // south edges of the two doors

    // North wall (z = hallMaxZ, faces -Z): origin at (maxX, 0, maxZ), U = -X.
    {
        TilePanelDesc d = wallTiles;
        d.width = hallMaxX - hallMinX; d.height = ceilY;
        buildTilePanel(glm::vec3(hallMaxX, 0, hallMaxZ), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                       d, wall, grout);
    }
    // South wall (z = hallMinZ, faces +Z).
    {
        TilePanelDesc d = wallTiles;
        d.width = hallMaxX - hallMinX; d.height = ceilY;
        buildTilePanel(glm::vec3(hallMinX, 0, hallMinZ), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                       d, wall, grout);
    }
    // East wall (x = hallMaxX, faces -X).
    {
        TilePanelDesc d = wallTiles;
        d.width = hallMaxZ - hallMinZ; d.height = ceilY;
        buildTilePanel(glm::vec3(hallMaxX, 0, hallMinZ), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0),
                       d, wall, grout);
    }
    // West wall (x = hallMinX, faces +X): segments around the two doorways.
    // Frame U = -Z starting from hallMaxZ so u grows southward; the lattice
    // phase keeps grout lines continuous across the segments.
    {
        auto westSegment = [&](float z0, float z1, float y0, float y1) {
            if (z1 - z0 < 1e-4f || y1 - y0 < 1e-4f) return;
            TilePanelDesc d = wallTiles;
            d.width = z1 - z0; d.height = y1 - y0;
            d.uPhase = tilePhase(hallMaxZ - z1, d.tileW);
            d.vPhase = tilePhase(y0, d.tileH);
            buildTilePanel(glm::vec3(hallMinX, y0, z1), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0),
                           d, wall, grout);
        };
        westSegment(hallMinZ, doorZ0a, 0.0f, ceilY);                 // south of door A
        westSegment(doorZ0a + doorW, doorZ0b, 0.0f, ceilY);          // between doors
        westSegment(doorZ0b + doorW, hallMaxZ, 0.0f, ceilY);         // north of door B
        westSegment(doorZ0a, doorZ0a + doorW, doorH, ceilY);         // lintel A
        westSegment(doorZ0b, doorZ0b + doorW, doorH, ceilY);         // lintel B
    }

    // ── Pool basin ── interior faces. Wall tile field from the floor up to
    // the accent band, the mosaic band above it, coping skirt covers the rest.
    {
        struct Side { glm::vec3 origin; glm::vec3 U; float len; };
        // Interior faces: normals point INTO the pool (U x V with V = +Y).
        const Side sides[4] = {
            { glm::vec3(poolMinX, 0, poolMinZ), glm::vec3(1, 0, 0), poolMaxX - poolMinX },   // south face -> +Z
            { glm::vec3(poolMaxX, 0, poolMaxZ), glm::vec3(-1, 0, 0), poolMaxX - poolMinX },  // north face -> -Z
            { glm::vec3(poolMaxX, 0, poolMinZ), glm::vec3(0, 0, 1), poolMaxZ - poolMinZ },   // east face -> -X
            { glm::vec3(poolMinX, 0, poolMaxZ), glm::vec3(0, 0, -1), poolMaxZ - poolMinZ },  // west face -> +X
        };
        for (const Side& s : sides) {
            // Main tile field.
            TilePanelDesc d = poolTiles;
            d.width = s.len;
            d.height = accentBottomY - poolFloorY;
            d.vPhase = tilePhase(poolFloorY, d.tileH);
            buildTilePanel(s.origin + glm::vec3(0, poolFloorY, 0), s.U, glm::vec3(0, 1, 0),
                           d, poolWall, grout);
            // Accent mosaic band at the waterline. It runs all the way up to
            // deck level BEHIND the coping skirt (which hangs 6 cm inside the
            // rim), so there is no open gap when seen from underwater.
            TilePanelDesc a = accentTiles;
            a.width = s.len;
            a.height = 0.0f - accentBottomY;
            buildTilePanel(s.origin + glm::vec3(0, accentBottomY, 0), s.U, glm::vec3(0, 1, 0),
                           a, accent, grout);
        }
        // Pool floor.
        TilePanelDesc d = poolTiles;
        d.width = poolMaxZ - poolMinZ;
        d.height = poolMaxX - poolMinX;
        buildTilePanel(glm::vec3(poolMinX, poolFloorY, poolMinZ), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0),
                       d, poolFloor, grout);
    }

    // ── Coping ── bullnose swept around the rim, slightly proud of the deck.
    {
        auto rim = roundedRectPathCCW(glm::vec2(poolMinX, poolMinZ), glm::vec2(poolMaxX, poolMaxZ),
                                      0.12f, 4, copingProud);
        sweepProfile(rim, true, copingProfile(copingR, copingLip, copingSkirt, 6),
                     1.0f / 0.30f, coping);
    }

    // ── Cove trim ── concave quarter-round along the hall wall bases
    // (skipping the doorway gaps on the west wall). Path travels with the
    // room interior on the LEFT so the profile faces inward.
    {
        const float inset = 0.0f;
        auto cove = [&](std::vector<glm::vec3> path) {
            sweepProfile(path, false, coveProfile(0.05f, 5), 1.0f / 0.20f,
                         S.bucket(MaterialSlot::Grout));
        };
        // South wall base: interior (+Z) on the left when traveling -X... the
        // cove profile bends from the wall face down to the floor; direction
        // only affects UV, the profile's `side` faces away from the wall via
        // the left-of-travel rule, so pick travel so cross(Y,T) points into
        // the room: south wall (z=minZ) needs +Z => T = -X.
        cove({ glm::vec3(hallMaxX - inset, 0, hallMinZ), glm::vec3(hallMinX + inset, 0, hallMinZ) });
        // North wall (z=maxZ) needs -Z => T = +X.
        cove({ glm::vec3(hallMinX + inset, 0, hallMaxZ), glm::vec3(hallMaxX - inset, 0, hallMaxZ) });
        // East wall (x=maxX) needs -X => T = -Z.
        cove({ glm::vec3(hallMaxX, 0, hallMaxZ - inset), glm::vec3(hallMaxX, 0, hallMinZ + inset) });
        // West wall (x=minX) needs +X => T = +Z, split around the doorways.
        cove({ glm::vec3(hallMinX, 0, hallMinZ + inset), glm::vec3(hallMinX, 0, doorZ0a) });
        cove({ glm::vec3(hallMinX, 0, doorZ0a + doorW), glm::vec3(hallMinX, 0, doorZ0b) });
        cove({ glm::vec3(hallMinX, 0, doorZ0b + doorW), glm::vec3(hallMinX, 0, hallMaxZ - inset) });
    }

    // ── Ceiling ── flat plaster facing down, minus six skylight wells.
    {
        const float wellHalfX = 1.2f, wellHalfZ = 0.8f;
        const float wellDepth = 0.5f;
        const float wellXs[3] = { -5.0f, 0.0f, 5.0f };
        const float wellZs[2] = { -1.5f, 1.5f };
        for (float wx : wellXs) {
            for (float wz : wellZs) {
                S.skylights.push_back(glm::vec4(wx, wz, wellHalfX, wellHalfZ));
            }
        }
        const float zEdges[6] = { wellZs[0] - wellHalfZ, wellZs[0] + wellHalfZ,
                                  wellZs[1] - wellHalfZ, wellZs[1] + wellHalfZ, 0, 0 };
        // Down-facing quad helper (U=+X, V=+Z -> normal -Y).
        auto ceilQuad = [&](float x0, float z0, float x1, float z1) {
            if (x1 - x0 < 1e-4f || z1 - z0 < 1e-4f) return;
            plainQuad(glm::vec3(x0, ceilY, z0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1),
                      x1 - x0, z1 - z0, 0.5f, ceil);
        };
        // Z strips without wells.
        ceilQuad(hallMinX, hallMinZ, hallMaxX, zEdges[0]);
        ceilQuad(hallMinX, zEdges[1], hallMaxX, zEdges[2]);
        ceilQuad(hallMinX, zEdges[3], hallMaxX, hallMaxZ);
        // Well rows: X segments between the wells.
        for (float wz : wellZs) {
            const float z0 = wz - wellHalfZ, z1 = wz + wellHalfZ;
            float cursor = hallMinX;
            for (float wx : wellXs) {
                ceilQuad(cursor, z0, wx - wellHalfX, z1);
                cursor = wx + wellHalfX;
            }
            ceilQuad(cursor, z0, hallMaxX, z1);
        }
        // Well shafts: 4 plaster faces per well, facing into the shaft, from
        // the ceiling up to the open top.
        for (float wx : wellXs) {
            for (float wz : wellZs) {
                const float x0 = wx - wellHalfX, x1 = wx + wellHalfX;
                const float z0 = wz - wellHalfZ, z1 = wz + wellHalfZ;
                // south face (z = z0) faces +Z (into the well).
                plainQuad(glm::vec3(x0, ceilY, z0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                          x1 - x0, wellDepth, 0.5f, ceil);
                // north face (z = z1) faces -Z.
                plainQuad(glm::vec3(x1, ceilY, z1), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                          x1 - x0, wellDepth, 0.5f, ceil);
                // west face (x = x0) faces +X.
                plainQuad(glm::vec3(x0, ceilY, z1), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0),
                          z1 - z0, wellDepth, 0.5f, ceil);
                // east face (x = x1) faces -X.
                plainQuad(glm::vec3(x1, ceilY, z0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0),
                          z1 - z0, wellDepth, 0.5f, ceil);
            }
        }
    }

    // ── Pillars ── four tiled faces each, on the north and south decks.
    {
        const float half = 0.275f;
        const glm::vec2 centers[6] = {
            { -8.0f, 3.9f }, { 0.0f, 3.9f }, { 8.0f, 3.9f },
            { -8.0f, -3.9f }, { 0.0f, -3.9f }, { 8.0f, -3.9f },
        };
        for (const glm::vec2& c : centers) {
            const float x0 = c.x - half, x1 = c.x + half;
            const float z0 = c.y - half, z1 = c.y + half;
            auto face = [&](glm::vec3 origin, glm::vec3 U, float w) {
                TilePanelDesc d = wallTiles;
                d.width = w; d.height = ceilY;
                buildTilePanel(origin, U, glm::vec3(0, 1, 0), d, wall, grout);
            };
            // Outward faces: normal = U x +Y must point away from the pillar.
            face(glm::vec3(x1, 0, z0), glm::vec3(-1, 0, 0), x1 - x0);  // south side -> -Z
            face(glm::vec3(x0, 0, z1), glm::vec3(1, 0, 0), x1 - x0);   // north side -> +Z
            face(glm::vec3(x1, 0, z1), glm::vec3(0, 0, -1), z1 - z0);  // east side -> +X
            face(glm::vec3(x0, 0, z0), glm::vec3(0, 0, 1), z1 - z0);   // west side -> -X
            S.colliders.push_back({ glm::vec3(c.x, ceilY * 0.5f, c.y),
                                    glm::vec3(half, ceilY * 0.5f, half) });
        }
    }

    // ── Corridor stub behind the west wall ── dark plaster box; the two
    // doorway reveals connect the hall face to the corridor face.
    {
        const float wallT = 0.35f;                       // west wall thickness
        const float corMinX = hallMinX - wallT - 2.8f;   // corridor depth 2.8
        const float corMaxX = hallMinX - wallT;
        const float corMinZ = -3.0f, corMaxZ = 3.0f;
        const float corCeil = 2.6f;

        // Interior faces (normals into the corridor).
        plainQuad(glm::vec3(corMinX, 0, corMinZ), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0),
                  corMaxZ - corMinZ, corMaxX - corMinX, 0.35f, dark);                       // floor (+Y)
        plainQuad(glm::vec3(corMinX, corCeil, corMinZ), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1),
                  corMaxX - corMinX, corMaxZ - corMinZ, 0.35f, dark);                       // ceiling (-Y)
        plainQuad(glm::vec3(corMinX, 0, corMinZ), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                  corMaxX - corMinX, corCeil, 0.35f, dark);                                // south face +Z
        plainQuad(glm::vec3(corMaxX, 0, corMaxZ), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                  corMaxX - corMinX, corCeil, 0.35f, dark);                                // north face -Z
        plainQuad(glm::vec3(corMinX, 0, corMaxZ), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0),
                  corMaxZ - corMinZ, corCeil, 0.35f, dark);                                // west face +X
        // East side of the corridor (the back of the hall's west wall),
        // split around the doorway openings.
        auto corridorEastFace = [&](float z0, float z1, float y0, float y1) {
            if (z1 - z0 < 1e-4f || y1 - y0 < 1e-4f) return;
            plainQuad(glm::vec3(corMaxX, y0, z0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0),
                      z1 - z0, y1 - y0, 0.35f, dark);                                      // faces -X
        };
        corridorEastFace(corMinZ, doorZ0a, 0, corCeil);
        corridorEastFace(doorZ0a + doorW, doorZ0b, 0, corCeil);
        corridorEastFace(doorZ0b + doorW, corMaxZ, 0, corCeil);
        corridorEastFace(doorZ0a, doorZ0a + doorW, doorH, corCeil);
        corridorEastFace(doorZ0b, doorZ0b + doorW, doorH, corCeil);

        // Doorway reveals through the wall: jambs face each other, head faces
        // down, all plaster.
        auto doorway = [&](float dz0) {
            const float dz1 = dz0 + doorW;
            // south jamb faces +Z... jamb plane z=dz0, faces INTO the doorway (+Z).
            plainQuad(glm::vec3(corMaxX, 0, dz0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                      wallT, doorH, 0.35f, dark);
            // north jamb (z=dz1) faces -Z.
            plainQuad(glm::vec3(corMaxX + wallT, 0, dz1), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                      wallT, doorH, 0.35f, dark);
            // head (y=doorH) faces down: U=+X, V=+Z -> -Y.
            plainQuad(glm::vec3(corMaxX, doorH, dz0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1),
                      wallT, doorW, 0.35f, dark);
            // floor through the doorway continues the deck: emit a dark floor
            // strip so there is no hole (+Y).
            plainQuad(glm::vec3(corMaxX, 0, dz0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0),
                      doorW, wallT, 0.35f, dark);
        };
        doorway(doorZ0a);
        doorway(doorZ0b);

        // Corridor collision.
        const float corMidX = (corMinX + corMaxX) * 0.5f;
        S.colliders.push_back({ glm::vec3(corMidX, -0.25f, 0), glm::vec3((corMaxX - corMinX) * 0.5f + wallT, 0.25f, 3.2f) });          // floor
        S.colliders.push_back({ glm::vec3(corMidX, corCeil + 0.2f, 0), glm::vec3((corMaxX - corMinX) * 0.5f + wallT, 0.2f, 3.2f) });   // ceiling
        S.colliders.push_back({ glm::vec3(corMinX - 0.175f, corCeil * 0.5f, 0), glm::vec3(0.175f, corCeil * 0.5f, 3.2f) });            // west
        S.colliders.push_back({ glm::vec3(corMidX, corCeil * 0.5f, corMinZ - 0.175f), glm::vec3((corMaxX - corMinX) * 0.5f, corCeil * 0.5f, 0.175f) });  // south
        S.colliders.push_back({ glm::vec3(corMidX, corCeil * 0.5f, corMaxZ + 0.175f), glm::vec3((corMaxX - corMinX) * 0.5f, corCeil * 0.5f, 0.175f) });  // north
    }

    // ── Pool-entry handrails ── the classic bent-tube Π hoop: up from the
    // deck, a smooth bend over the coping, straight down into the water.
    // (No rungs — reference-style grab rails; the climb zone still lets the
    // player pull themselves out against one.)
    const float railR = 0.024f;
    const int railSides = 14;
    auto handrailBow = [&](glm::vec3 deckAnchor, glm::vec3 dirIn, float overWater,
                           float apexY, float dropToY) {
        // deckAnchor: rail base on the deck (y = 0). dirIn: horizontal unit
        // direction toward the pool. The hoop rises to apexY, runs
        // `overWater` toward the pool and dives down to dropToY.
        const float bendR = 0.24f;
        std::vector<glm::vec3> path;
        appendLine(path, deckAnchor + glm::vec3(0, 0.02f, 0),
                   deckAnchor + glm::vec3(0, apexY - bendR, 0), 4, true);
        appendArc(path, deckAnchor + glm::vec3(0, apexY - bendR, 0) + dirIn * bendR,
                  -dirIn * bendR, glm::vec3(0, bendR, 0), 6);
        appendLine(path, path.back(),
                   deckAnchor + glm::vec3(0, apexY, 0) + dirIn * (overWater - bendR), 2, false);
        appendArc(path, deckAnchor + glm::vec3(0, apexY - bendR, 0) + dirIn * (overWater - bendR),
                  glm::vec3(0, bendR, 0), dirIn * bendR, 6);
        appendLine(path, path.back(),
                   deckAnchor + dirIn * overWater + glm::vec3(0, dropToY, 0), 6, false);
        sweepTube(path, railR, railSides, 0.25f, metal, true);
    };
    auto poolEntry = [&](float cx, float poolEdgeZ, float dirOut) {
        // dirOut = +1: deck at +Z of the pool edge; -1: deck at -Z.
        const glm::vec3 dirIn(0, 0, -dirOut);
        const float gap = 0.72f;  // hoop-to-hoop spacing
        for (float sx : { cx - gap * 0.5f, cx + gap * 0.5f }) {
            handrailBow(glm::vec3(sx, 0.0f, poolEdgeZ + dirOut * 0.38f), dirIn,
                        0.78f, 0.82f, S.waterLevel - 0.85f);
            S.colliders.push_back({ glm::vec3(sx, 0.4f, poolEdgeZ + dirOut * 0.38f),
                                    glm::vec3(0.04f, 0.42f, 0.04f) });
        }
        const glm::vec3 zoneMin(cx - 0.55f, poolFloorY, poolEdgeZ - 0.6f);
        const glm::vec3 zoneMax(cx + 0.55f, 0.95f, poolEdgeZ + 0.6f);
        S.ladderZones.emplace_back(glm::min(zoneMin, zoneMax), glm::max(zoneMin, zoneMax));
    };
    poolEntry(6.2f, poolMaxZ, 1.0f);    // NE entry onto the north deck
    poolEntry(-6.2f, poolMinZ, -1.0f);  // SW entry onto the south deck

    // ── Tiled stairs into the pool ── a solid mosaic-tiled block descending
    // from the west deck edge to swimming depth, flanked by two handrail
    // hoops (the reference look: steps + smooth bent rails, no rung ladder).
    {
        const int stepCount = 10;
        const float rise = 0.21f, run = 0.32f;  // gentle enough for the capsule to walk up
        const float halfW = 1.3f;                 // stairs span z in [-1.3, 1.3]
        const float topX = poolMinX;              // flush with the west rim
        TilePanelDesc stepTiles = accentTiles;    // 10 cm mosaic reads best on steps

        for (int i = 0; i < stepCount; ++i) {
            const float x0 = topX + i * run;
            const float x1 = x0 + run;
            const float topY = -rise * static_cast<float>(i);
            const float botY = topY - rise;
            // Tread (up): U=+Z, V=+X.
            TilePanelDesc t = stepTiles;
            t.width = 2.0f * halfW; t.height = run;
            buildTilePanel(glm::vec3(x0, topY, -halfW), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0),
                           t, accent, grout);
            // Riser (faces +X, toward the pool interior).
            TilePanelDesc r = stepTiles;
            r.width = 2.0f * halfW; r.height = rise;
            buildTilePanel(glm::vec3(x1, botY, -halfW), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0),
                           r, accent, grout);
            // Solid sides down to the pool floor (the block reads as masonry).
            TilePanelDesc sd = stepTiles;
            sd.width = run; sd.height = topY - poolFloorY;
            // south side faces -Z: U=-X from x1.
            buildTilePanel(glm::vec3(x1, poolFloorY, -halfW), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                           sd, accent, grout);
            // north side faces +Z: U=+X from x0.
            buildTilePanel(glm::vec3(x0, poolFloorY, halfW), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                           sd, accent, grout);
            // Collision: full-depth box per step.
            S.colliders.push_back({ glm::vec3((x0 + x1) * 0.5f, (poolFloorY + topY) * 0.5f, 0.0f),
                                    glm::vec3(run * 0.5f, (topY - poolFloorY) * 0.5f, halfW) });
        }
        // Front face of the block (below the last riser, facing +X).
        {
            const float frontX = topX + stepCount * run;
            const float frontTop = -rise * stepCount;
            TilePanelDesc f = stepTiles;
            f.width = 2.0f * halfW; f.height = frontTop - poolFloorY;
            buildTilePanel(glm::vec3(frontX, poolFloorY, -halfW), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0),
                           f, accent, grout);
        }
        // Handrail hoops flanking the stairs, diving parallel to the pitch.
        handrailBow(glm::vec3(topX - 0.45f, 0.0f, -(halfW + 0.18f)), glm::vec3(1, 0, 0),
                    1.15f, 0.85f, S.waterLevel - 0.9f);
        handrailBow(glm::vec3(topX - 0.45f, 0.0f, halfW + 0.18f), glm::vec3(1, 0, 0),
                    1.15f, 0.85f, S.waterLevel - 0.9f);
        // The stairs are their own exit — mark a climb-free zone is not
        // needed; swimmers walk out. (Zones stay for the two rail entries.)
    }

    // ── Water slides ── two glossy tube slides on the north wall: a vertical
    // drop hugging the wall, a sweeping bend over the coping, and an open
    // mouth grazing the water. Outer shell + inward-facing lining + a rim
    // annulus make the mouth read as a real hollow tube.
    {
        const float tubeR = 0.42f, wallT = 0.05f;
        auto slide = [&](float cx, MaterialSlot slot) {
            MeshData& shell = S.bucket(slot);
            const float zWall = hallMaxZ - 0.55f;   // drop axis, proud of the wall
            const float bendR = 1.15f;
            const float mouthY = 0.55f;             // mouth center: bottom clears the coping lip
            const float mouthZ = poolMaxZ - 0.75f;  // hangs over the pool

            std::vector<glm::vec3> path;
            // Start above the ceiling plane so the open top edge is hidden
            // (the ceiling is solid here — the skylight wells sit at z ±1.5).
            appendLine(path, glm::vec3(cx, ceilY + 0.4f, zWall),
                       glm::vec3(cx, mouthY + bendR, zWall), 6, true);
            appendArc(path, glm::vec3(cx, mouthY + bendR, zWall - bendR),
                      glm::vec3(0, 0, bendR), glm::vec3(0, -bendR, 0), 8);
            appendLine(path, path.back(), glm::vec3(cx, mouthY, mouthZ), 3, false);

            const size_t v0 = shell.positions.size();
            const size_t i0 = shell.indices.size();
            sweepTube(path, tubeR, 20, 1.0f, shell, false);          // outer
            const size_t v1 = shell.positions.size();
            const size_t i1 = shell.indices.size();
            sweepTube(path, tubeR - wallT, 20, 1.0f, shell, false);  // lining
            flipAppended(shell, v1, i1);
            (void)v0; (void)i0;
            annulus(glm::vec3(cx, mouthY, mouthZ), glm::vec3(0, 0, -1),
                    tubeR, tubeR - wallT, 20, shell);

            // Collision: the vertical column (players walk around it).
            S.colliders.push_back({ glm::vec3(cx, ceilY * 0.5f, zWall),
                                    glm::vec3(tubeR + 0.03f, ceilY * 0.5f, tubeR + 0.03f) });
        };
        slide(3.6f, MaterialSlot::SlideBlue);
        slide(5.2f, MaterialSlot::SlideViolet);
    }

    // ── Porthole wall lamps ── warm round fixtures: a metal rim ring (torus)
    // around an emissive glow disc, slightly proud of the tile face. These
    // carry the flooded-mode look: their glow discs reflect and smear across
    // the water via the planar-reflection pass.
    {
        MeshData& glow = S.bucket(MaterialSlot::LampGlow);
        auto lamp = [&](glm::vec3 wallPos, glm::vec3 outNormal) {
            const glm::vec3 n = glm::normalize(outNormal);
            disc(wallPos + n * 0.045f, n, 0.16f, 24, glow);
            sweepTube(circlePath(wallPos + n * 0.035f, n, 0.185f, 24), 0.028f, 10, 0.3f,
                      metal, false);
            S.lamps.emplace_back(wallPos + n * 0.045f, n);
        };
        const float lampY = 2.25f;
        for (float lx : { -9.0f, -3.0f, 3.0f, 9.0f }) {
            lamp(glm::vec3(lx, lampY, hallMaxZ), glm::vec3(0, 0, -1));  // north wall
            lamp(glm::vec3(lx, lampY, hallMinZ), glm::vec3(0, 0, 1));   // south wall
        }
        for (float lz : { -2.5f, 2.5f }) {
            lamp(glm::vec3(hallMaxX, lampY, lz), glm::vec3(-1, 0, 0));  // east wall
        }
        // A dim pair on the corridor's far wall, glowing back through the
        // doorways — the two doors read as lit portals from the hall.
        const float corFarX = hallMinX - 0.35f - 2.8f;
        lamp(glm::vec3(corFarX, 1.9f, -1.5f), glm::vec3(1, 0, 0));
        lamp(glm::vec3(corFarX, 1.9f, 1.5f), glm::vec3(1, 0, 0));
    }

    // ── Collision: room shell, deck, basin ──
    {
        // Deck slabs (top at y=0).
        S.colliders.push_back({ glm::vec3(0, -0.25f, (poolMaxZ + hallMaxZ) * 0.5f),
                                glm::vec3(12.0f, 0.25f, (hallMaxZ - poolMaxZ) * 0.5f) });  // north
        S.colliders.push_back({ glm::vec3(0, -0.25f, (poolMinZ + hallMinZ) * 0.5f),
                                glm::vec3(12.0f, 0.25f, (poolMinZ - hallMinZ) * 0.5f) });  // south
        S.colliders.push_back({ glm::vec3((hallMinX + poolMinX) * 0.5f, -0.25f, 0),
                                glm::vec3((poolMinX - hallMinX) * 0.5f, 0.25f, 3.0f) });   // west
        S.colliders.push_back({ glm::vec3((hallMaxX + poolMaxX) * 0.5f, -0.25f, 0),
                                glm::vec3((hallMaxX - poolMaxX) * 0.5f, 0.25f, 3.0f) });   // east
        // Pool floor + basin walls.
        S.colliders.push_back({ glm::vec3(0, poolFloorY - 0.25f, 0), glm::vec3(7.35f, 0.25f, 3.35f) });
        const float bwCY = (poolFloorY + 0.0f) * 0.5f;
        const float bwHY = -poolFloorY * 0.5f;
        S.colliders.push_back({ glm::vec3(0, bwCY, poolMaxZ + 0.175f), glm::vec3(7.35f, bwHY, 0.175f) });
        S.colliders.push_back({ glm::vec3(0, bwCY, poolMinZ - 0.175f), glm::vec3(7.35f, bwHY, 0.175f) });
        S.colliders.push_back({ glm::vec3(poolMaxX + 0.175f, bwCY, 0), glm::vec3(0.175f, bwHY, 3.35f) });
        S.colliders.push_back({ glm::vec3(poolMinX - 0.175f, bwCY, 0), glm::vec3(0.175f, bwHY, 3.35f) });
        // Outer walls.
        S.colliders.push_back({ glm::vec3(0, ceilY * 0.5f, hallMaxZ + 0.175f), glm::vec3(12.35f, ceilY * 0.5f, 0.175f) });
        S.colliders.push_back({ glm::vec3(0, ceilY * 0.5f, hallMinZ - 0.175f), glm::vec3(12.35f, ceilY * 0.5f, 0.175f) });
        S.colliders.push_back({ glm::vec3(hallMaxX + 0.175f, ceilY * 0.5f, 0), glm::vec3(0.175f, ceilY * 0.5f, 5.35f) });
        // West wall segments around the doorways (thickness matches the
        // corridor reveal depth).
        auto westBox = [&](float z0, float z1, float y0, float y1) {
            if (z1 - z0 < 1e-4f || y1 - y0 < 1e-4f) return;
            S.colliders.push_back({ glm::vec3(hallMinX - 0.175f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f),
                                    glm::vec3(0.175f, (y1 - y0) * 0.5f, (z1 - z0) * 0.5f) });
        };
        westBox(hallMinZ, doorZ0a, 0, ceilY);
        westBox(doorZ0a + doorW, doorZ0b, 0, ceilY);
        westBox(doorZ0b + doorW, hallMaxZ, 0, ceilY);
        westBox(doorZ0a, doorZ0a + doorW, doorH, ceilY);
        westBox(doorZ0b, doorZ0b + doorW, doorH, ceilY);
        // Ceiling (also seals the skylight wells physically).
        S.colliders.push_back({ glm::vec3(0, ceilY + 0.2f, 0), glm::vec3(12.35f, 0.2f, 5.35f) });
    }

    return S;
}

}  // namespace poolgen
