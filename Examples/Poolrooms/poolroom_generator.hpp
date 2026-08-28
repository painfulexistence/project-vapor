#pragma once
// ============================================================================
// Poolrooms — the whole environment registered as ONE scene-JSON mesh
// generator, so poolrooms.json can say
//
//   { "name": "Hall", "procMesh": { "generator": "poolroom",
//                                   "materials": { "deck_tile": "deckTile", ... } } }
//
// instead of main.cpp calling buildPoolroomScene() and wiring the result up by
// hand. This is the composite case Vapor::MeshGenerators exists for: the
// engine registers thin wrappers over its procgen primitives, and an app
// registers whole structures, where the cross-element decisions live.
//
// A poolroom knows more than where its triangles go — it knows where the water
// sits, where the lamps hang, where the player starts. Those ride out on
// GeneratedContent's other channels: colliders for the physics layer, and
// children (a name, a local transform, a component blob) for everything the
// applier registry already understands. Nothing here mentions the renderer.
// ============================================================================

#include "Vapor/scene_blueprint.hpp"
#include "poolroom_scene.hpp"
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace poolgen {

// Waist-deep, for the flooded mood.
inline constexpr float kFloodLevel = 0.45f;

// The water component, built as JSON rather than a WaterComponent, because
// that is the channel a generated child carries — and it means the applier
// registry validates it on the way in, exactly like a hand-authored scene.
//
// The two moods differ in far more than water; this covers only what the
// GENERATOR owns (the surface and the volume it lights). Sun, fog and sky live
// on their own entities in the scene file.
inline std::string waterComponentJson(bool flooded, const PoolroomScene& S) {
    using nlohmann::json;
    const float level = flooded ? kFloodLevel : S.waterLevel;

    json look, caustics, spectrum, grid;
    // Shared surface detail.
    look["normalMapScroll"] = { 1.0, 0.35, -0.45, 1.0 };
    look["normalMapScrollSpeed"] = { 0.011, 0.008 };
    look["dampeningFactor"] = 4.0;
    look["foamTiling"] = 2.0;

    if (!flooded) {
        // Sunlit pool: the surface fills the sunken basin only.
        grid = { { "tilesX", 56 }, { "tilesZ", 24 }, { "tileSize", 0.25 } };
        look["surfaceColor"] = { 0.82, 0.90, 0.94, 1.0 };
        look["refractionColor"] = { 0.42, 0.68, 0.72, 1.0 };
        look["refractionDistortionFactor"] = 0.03;
        look["refractionHeightFactor"] = 2.2;
        look["depthSofteningDistance"] = 0.30;
        look["roughness"] = 0.045;
        look["reflectance"] = 0.38;  // F0 ~ 0.023 — real water
        look["specIntensity"] = 6.0;
        look["foamBrightness"] = 1.1;
        look["fftParams"] = { 16.0, 6.0, 0.30, 1.0 };
        look["reflectionParams"] = { 0.85, 0.05, 0.5, 256.0 };

        caustics = { { "intensity", 1.15 }, { "worldScale", 1.7 }, { "speed", 0.6 },
                     { "fadeDepth", 9.0 }, { "envReflectionIntensity", 1.0 } };
        // Bounds are relative to the water entity, which sits AT the water
        // level — so the basin runs from the pool floor up to the surface.
        caustics["boundsMin"] = { S.poolMinXZ.x, S.poolFloorY - level, S.poolMinXZ.y };
        caustics["boundsMax"] = { S.poolMaxXZ.x, 0.0, S.poolMaxXZ.y };

        // 1.9e-4 Phillips => ~1.5 cm RMS, ~4.5 cm crests: an indoor ripple.
        spectrum = { { "patchSize", 16.0 },      { "windSpeedMps", 1.7 },
                     { "windDirRad", 0.9 },      { "amplitude", 0.00019 },
                     { "choppiness", 1.05 },     { "depthMeters", 2.0 },
                     { "smallWaveCutoff", 0.012 }, { "directionalSpread", 2.5 } };
    } else {
        // Flooded hall: waist-deep everywhere, glassy and viscous. Murky warm
        // green — reflections carry a sepia cast and absorption is fast.
        grid = { { "tilesX", 96 }, { "tilesZ", 40 }, { "tileSize", 0.25 } };
        look["surfaceColor"] = { 0.97, 0.93, 0.84, 1.0 };
        look["refractionColor"] = { 0.11, 0.16, 0.12, 1.0 };
        look["refractionDistortionFactor"] = 0.045;
        look["refractionHeightFactor"] = 0.85;
        look["depthSofteningDistance"] = 0.50;
        look["roughness"] = 0.02;
        look["reflectance"] = 0.42;
        look["specIntensity"] = 3.0;
        look["foamBrightness"] = 0.85;
        // Almost no micro detail: the swells read viscous and oily.
        look["fftParams"] = { 16.0, 6.0, 0.06, 1.0 };
        // Strong planar reflection with a heavy wobble — the lamp glows smear
        // into long living streaks.
        look["reflectionParams"] = { 0.94, 0.085, 0.35, 256.0 };

        caustics = { { "intensity", 0.5 }, { "worldScale", 1.3 }, { "speed", 0.5 },
                     { "fadeDepth", 3.0 }, { "envReflectionIntensity", 0.8 } };
        caustics["boundsMin"] = { -12.0, S.poolFloorY - level, -5.0 };
        caustics["boundsMax"] = { 12.0, 0.0, 5.0 };

        // Smooth rolling lobes: kill everything shorter than ~22 cm, broad
        // spread, languid clock. 6.7e-4 => ~4.5 cm RMS, ~13 cm crests.
        spectrum = { { "patchSize", 16.0 },     { "windSpeedMps", 2.3 },
                     { "windDirRad", 0.6 },     { "amplitude", 0.00067 },
                     { "choppiness", 1.3 },     { "depthMeters", 1.2 },
                     { "smallWaveCutoff", 0.22 }, { "directionalSpread", 1.6 },
                     { "timeScale", 0.75 } };
    }

    const json water = { { "water",
                           { { "grid", grid },
                             { "look", look },
                             { "caustics", caustics },
                             { "spectrum", spectrum } } } };
    return water.dump();
}

// Register the composite generator. Call once, BEFORE loading a scene that
// names it — the same contract as any TextureGenerators::add.
//
// Params:
//   "flooded" (bool) — pick the mood. The geometry is identical either way;
//                      only the water surface it emits differs.
inline void registerPoolroomGenerator() {
    Vapor::MeshGenerators::instance().add("poolroom", 1, [](const nlohmann::json& params) {
        Vapor::GeneratedContent out;
        PoolroomScene S = buildPoolroomScene();

        // Geometry, one bucket per material slot. The slot names are the same
        // strings the scene file maps to its materials.
        for (int i = 0; i < int(MaterialSlot::Count); ++i) {
            MeshData& bucket = S.buckets[static_cast<size_t>(i)];
            if (bucket.empty()) continue;
            out.buckets[materialSlotName(MaterialSlot(i))] = std::move(bucket);
        }

        // Static collision, straight from the CSG that made the walls — so the
        // physics shape cannot drift from what is drawn.
        out.colliders = std::move(S.colliders);

        const auto floodedIt = params.find("flooded");
        const bool flooded =
            floodedIt != params.end() && floodedIt->is_boolean() ? floodedIt->get<bool>() : false;

        // The water surface. Its Y is the water level, which is also the plane
        // the caustics are projected onto — one number, one place.
        out.children.push_back({ "PoolWater",
                                 glm::vec3(0.0f, flooded ? kFloodLevel : S.waterLevel, 0.0f),
                                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f),
                                 waterComponentJson(flooded, S) });

        // Where the player starts. No components — a named, placed marker the
        // app looks up, because a character controller is app code, not scene
        // data.
        out.children.push_back({ "PlayerSpawn", S.spawnPosition,
                                 glm::angleAxis(glm::radians(S.spawnYawDeg), glm::vec3(0, 1, 0)),
                                 glm::vec3(1.0f), "" });

        // Porthole lamps: a warm point light at each glow disc, pushed a little
        // off the wall along its outward normal so the falloff reads on the
        // tiles rather than clipping into them. The emissive disc itself is
        // geometry in the LampGlow bucket, which is what smears across the
        // water's planar reflection.
        const std::string lampLight =
            R"({"pointLight":{"color":[1.0,0.76,0.5],"intensity":2.2,"radius":7.0}})";
        for (size_t i = 0; i < S.lamps.size(); ++i) {
            const glm::vec3 position = S.lamps[i].first;
            const glm::vec3 normal = S.lamps[i].second;
            out.children.push_back({ fmt::format("Lamp{}", i), position + normal * 0.16f,
                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f),
                                     lampLight });
        }

        // Ladder volumes and skylight wells travel as named, placed markers
        // with their half-extents in the transform scale; the app reads them by
        // name (climb zones, light shafts). Same reason as the spawn: the
        // engine has no component for either concept, and inventing one just to
        // carry an AABB would be worse than a marker.
        for (size_t i = 0; i < S.ladderZones.size(); ++i) {
            const glm::vec3 lo = S.ladderZones[i].first;
            const glm::vec3 hi = S.ladderZones[i].second;
            out.children.push_back({ fmt::format("LadderZone{}", i), (lo + hi) * 0.5f,
                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f), (hi - lo) * 0.5f, "" });
        }
        for (size_t i = 0; i < S.skylights.size(); ++i) {
            const glm::vec4& sky = S.skylights[i];
            out.children.push_back({ fmt::format("Skylight{}", i),
                                     glm::vec3(sky.x, S.ceilingY, sky.y),
                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                     glm::vec3(sky.z, 1.0f, sky.w), "" });
        }
        return out;
    });
}

}  // namespace poolgen
