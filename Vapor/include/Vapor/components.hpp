#pragma once
#include "Vapor/hidden.hpp"
#include "character_controller.hpp"
#include "graphics_handles.hpp"
#include "graphics_sprite.hpp"
#include "physics_3d.hpp"
#include "render_data.hpp"   // SkyType
#include "terrain_world.hpp" // StreamingTerrainComponent's shared_ptr<TerrainWorld> needs the complete type
#include "vehicle_controller.hpp"
#include "voxel_world.hpp"   // VoxelVolumeComponent's shared_ptr<VoxelWorld> needs the complete type
#include <entt/entt.hpp>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Vapor {

    // ============================================================================
    // Core
    // ============================================================================
    struct NameComponent {
        std::string name;
    };

    struct TransformComponent {
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale = glm::vec3(1.0f);
        glm::mat4    worldTransform = glm::mat4(1.0f);  // computed; mat4 skipped by inspector
        entt::entity parent = entt::null;               // shown read-only in inspector
        Hidden<bool> isDirty = {true};                  // internal bool hidden from inspector
    };

    struct Mesh;
    struct MeshRendererComponent {
        std::vector<std::shared_ptr<Mesh>> meshes;
        bool visible = true;
    };

    // ============================================================================
    // Physics
    // ============================================================================
    struct RigidbodyComponent {
        BodyHandle body;
        BodyMotionType motionType = BodyMotionType::Dynamic;
        bool syncToPhysics = false;
        bool syncFromPhysics = true;
    };

    struct BoxColliderComponent {
        glm::vec3 halfSize = glm::vec3(0.5f);
    };

    struct SphereColliderComponent {
        float radius = 0.5f;
    };

    // ============================================================================
    // Camera
    // ============================================================================
    struct VirtualCameraComponent {
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        float fov = glm::radians(60.0f);
        float aspect = 1.0f;
        float near = 0.05f;
        float far = 500.0f;
        glm::mat4 viewMatrix = glm::mat4(1.0f);
        glm::mat4 projectionMatrix = glm::mat4(1.0f);
        bool isActive = false;
    };

    struct FlyCameraComponent {
        float moveSpeed = 5.0f;
        float rotateSpeed = 90.0f;
        float yaw = 90.0f;  // Look toward -Z (forward)
        float pitch = 0.0f;
    };

    struct FollowCameraComponent {
        entt::entity target = entt::null;
        glm::vec3 offset = glm::vec3(0.0f, 2.0f, 5.0f);
        float smoothFactor = 0.1f;
        float deadzone = 0.1f;
    };

    // ============================================================================
    // Control intent
    // ============================================================================
    // Per-entity control intent, written each frame by the app's input-mapping
    // layer (or synthesized by CameraControlSystem's InputManager adapter).
    // Systems consume intent instead of raw input, so bindings, gamepads,
    // replays, and AI drivers all feed the same path.
    struct CharacterIntent {
        glm::vec2 lookVector = glm::vec2(0.0f);
        glm::vec2 moveVector = glm::vec2(0.0f);
        float moveVerticalAxis = 0.0f;
        bool jump = false;
        bool sprint = false;
        bool interact = false;
    };

    // ============================================================================
    // Grab / Interaction
    // ============================================================================
    struct GrabberComponent {
        entt::entity heldEntity = entt::null;
        float maxPickupRange = 20.0f;
    };

    struct HeldByComponent {
        entt::entity holder = entt::null;
        float originalGravityFactor = 1.0f;
        float holdDistance = 3.0f;
    };

    // ============================================================================
    // Character Controller (ECS-owned)
    // ============================================================================
    struct CharacterBodyComponent {
        CharacterControllerSettings settings;
        std::unique_ptr<CharacterController> controller;
        glm::vec3 desiredVelocity = glm::vec3(0.0f);
        bool jumpRequested = false;
    };

    // ============================================================================
    // Vehicle Controller (ECS-owned)
    // ============================================================================
    struct VehicleBodyComponent {
        VehicleSettings settings;
        std::unique_ptr<VehicleController> controller;
        float throttle  = 0.0f;
        float steering  = 0.0f;
        float brake     = 0.0f;
        bool  handbrake = false;
    };

    // ============================================================================
    // Trigger Volume (ECS-owned) - Pure data, no callbacks
    // ============================================================================
    struct TriggerVolumeComponent {
        TriggerHandle trigger;
    };

    // Event components emitted by TriggerSystem when entities enter/exit triggers
    struct TriggerEnterEvent {
        entt::entity triggerEntity;  // The trigger volume entity
        entt::entity otherEntity;    // The entity that entered
    };

    struct TriggerExitEvent {
        entt::entity triggerEntity;  // The trigger volume entity
        entt::entity otherEntity;    // The entity that exited
    };

    // ============================================================================
    // Lighting
    // ============================================================================

    struct PointLightComponent {
        glm::vec3 color    = glm::vec3(1.0f);
        float     intensity = 1.0f;
        float     radius   = 0.5f;
    };

    struct DirectionalLightComponent {
        glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 color     = glm::vec3(1.0f);
        float     intensity = 1.0f;
    };

    // Cone spot light. Position comes from the TransformComponent; the beam points
    // along the transform's forward axis (rotation * -Z). Angles are in degrees.
    struct SpotLightComponent {
        glm::vec3 color      = glm::vec3(1.0f);
        float     intensity  = 10.0f;
        float     radius     = 12.0f;   // range (world units)
        float     innerAngle = 20.0f;   // full-intensity half-angle (deg)
        float     outerAngle = 30.0f;   // falloff-to-zero half-angle (deg)
};
    struct RectLightComponent {
        glm::vec2 size = {1.0f, 1.0f};         // total width × height (world units)
        glm::vec3 color = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        bool useVideoTexture = false;
    };

    // Tag marking the authoritative sun among directional lights. LightGatherSystem
    // gathers the tagged entity into directionalLights[0]; the atmosphere/sky and
    // the time-of-day driver identify the sun by this tag, never by list order.
    struct SunComponent {};

    // Sky authoring — the gameplay layer's choice of sky and its tunables. Put
    // one on an environment entity (singleton). SkySystem resolves it into a
    // SkyRenderData and pushes it to the renderer whenever `dirty` is set. The
    // sun is NOT here — it stays on the SunComponent-tagged directional light.
    // Gradient/HDRI *visible* passes are still WIP; today `type` selects the IBL
    // source and the atmosphere tunables drive the procedural sky.
    struct SkyComponent {
        SkyType type = SkyType::Atmosphere;
        // Atmosphere tunables (used when type == Atmosphere).
        glm::vec3 rayleighCoefficients = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);
        float rayleighScaleHeight = 8500.0f;
        float mieCoefficient = 21e-6f;
        float mieScaleHeight = 1200.0f;
        float miePreferredDirection = 0.758f;
        float planetRadius = 6371e3f;
        float atmosphereRadius = 6471e3f;
        float exposure = 1.0f;
        glm::vec3 groundColor = glm::vec3(0.015f, 0.015f, 0.02f);
        // Gradient sky colors (used when type == Gradient).
        glm::vec3 gradientZenith  = glm::vec3(0.18f, 0.34f, 0.62f);
        glm::vec3 gradientHorizon = glm::vec3(0.62f, 0.74f, 0.88f);
        glm::vec3 gradientGround  = glm::vec3(0.20f, 0.18f, 0.16f);
        bool dirty = true;  // set when edited; SkySystem re-pushes to the renderer

        // IBL rebake throttle: SkySystem re-bakes the environment IBL when the
        // sun has moved more than this many degrees since the last bake (a moving
        // sun restales the captured sky). 0 disables it. _lastIblSunDir is the
        // sun direction the IBL was last baked for (runtime, inspector-hidden).
        float iblSunThresholdDeg = 5.0f;
        Hidden<glm::vec3> _lastIblSunDir = {glm::vec3(0.0f)};
    };

    // A raymarched micro-voxel volume (see voxel_world.hpp). VoxelVolumeSystem
    // creates and generates the VoxelWorld on first sight (chunk jobs on the
    // task scheduler, so big worlds stream in without blocking a frame) and
    // pushes the live volume list to the renderer every frame. The entity's
    // TransformComponent places it: the grid is centered over the position in
    // x/z and rises from its y, translation only — like the original.
    struct VoxelVolumeComponent {
        glm::ivec3 gridDim = glm::ivec3(256, 256, 256);  // voxels; multiples of 8
        float voxelSize = 0.05f;                         // meters per voxel (5 cm)
        Uint32 seed = 1337u;
        Uint32 brickCapacity = 262144u;                  // pool budget (x 576 bytes)
        bool regenerate = false;   // set true (e.g. from the inspector) to rebuild
        Hidden<std::shared_ptr<VoxelWorld>> world = {};  // owned; created by the system
        Hidden<Uint32> _generatedSeed = {0u};            // seed the world was built with
    };

    // A streamed heightfield terrain (see terrain_world.hpp). TerrainSystem
    // owns the TerrainWorld: on first sight it prewarms the whole world at
    // the coarsest LOD (full horizon on frame one), then refines concentric
    // detail rings around the active camera on task-scheduler workers,
    // rewriting a fixed per-LOD mesh slot pool in place — streaming never
    // allocates GPU geometry. Deterministic tree/rock scatter spawns
    // instanced-mesh entities in a ring. One per world (singleton).
    struct StreamingTerrainComponent {
        float worldSize = 10240.0f;    // meters per axis (rounded to tiles)
        float tileSize = 512.0f;
        float heightScale = 500.0f;
        float noiseFrequency = 0.0007f;
        int noiseOctaves = 9;
        Uint32 seed = 20260705u;
        int lod0RadiusTiles = 2;
        int lod1RadiusTiles = 4;
        int lod2RadiusTiles = 8;
        int scatterRadiusTiles = 3;
        int scatterPerTile = 90;       // placement attempts per tile
        bool regenerate = false;       // set true (e.g. from the inspector) to rebuild
        Hidden<std::shared_ptr<TerrainWorld>> world = {};  // owned; created by the system
    };

    // Time-of-day clock. TimeOfDaySystem advances it and drives the
    // SunComponent-tagged directional light's direction/color/intensity each
    // frame, so the sky, fog, clouds and shadows all follow one moving sun. Put
    // one on the environment entity (singleton). When present it OWNS the sun's
    // direction — don't also animate that light from game logic.
    struct TimeOfDayComponent {
        float timeOfDay = 10.0f;          // hours in [0, 24)
        float dayLengthSeconds = 120.0f;  // real seconds per in-game day; 0 = frozen
        float latitudeDeg = 25.0f;        // tilts the sun's arc toward +Z (south)
        float maxSunIntensity = 10.0f;    // sun intensity at the zenith
        bool  paused = false;
    };

    // 2D Sprite rendering component
    struct SpriteComponent {
        AtlasHandle atlas;
        uint16_t frameIndex = 0;

        glm::vec2 size = {1.0f, 1.0f};       // World units
        glm::vec2 pivot = {0.5f, 0.5f};      // Anchor point (0-1)
        glm::vec4 tint = {1, 1, 1, 1};       // Color tint
        int sortingLayer = 0;
        int orderInLayer = 0;
        bool flipX = false;
        bool flipY = false;
        bool visible = true;
    };

    // Flipbook animation component (drives any frame-based animation)
    struct FlipbookComponent {
        std::vector<uint16_t> frameIndices;
        float frameTime = 0.1f;              // Seconds per frame
        float timer = 0.0f;
        uint16_t currentIndex = 0;           // Index into frameIndices
        bool loop = true;
        bool playing = true;

        uint16_t getCurrentFrame() const {
            return frameIndices.empty() ? 0 : frameIndices[currentIndex];
        }
    };

    // ============================================================================
    // Particle ECS Components
    // ============================================================================

    // Attached to any entity that should act as a particle attractor.
    struct ParticleAttractorComponent {
        float strength = 5.0f;
    };

    // World-space wind — not particle-specific; vegetation, cloth, audio all read it.
    struct WindFieldComponent {
        glm::vec3 direction  = glm::vec3(1.0f, 0.0f, 0.0f);
        float     strength   = 0.0f;
        float     turbulence = 0.0f; // curl noise strength for the particle sim
    };

    // Per-emitter configuration.
    struct ParticleEmitterComponent {
        uint32_t maxParticles = 128;
        float emitRate = 30.0f;         // particles per second
        float particleLifetime = 2.0f;  // -1 for immortal
        float speed = 2.0f;
        float spread = 0.3f;            // half-cone angle in radians
        glm::vec3 emitDirection = glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec4 color = glm::vec4(1.0f);
        bool enabled = true;   // false = immediate clear (Clear semantic)
        bool emitting = true;  // false = graceful stop: stop spawning, let existing finish (Stop semantic)
        bool oneShot = false;  // emit all maxParticles at once, then idle

        // Runtime state (managed by ParticleEmitterSystem) — hidden from inspector
        Hidden<float>    _accumulator = {0.0f};
        Hidden<uint32_t> _slotBegin   = {~0u};  // ~0u = not yet allocated
        Hidden<uint32_t> _slotCount   = {0};
        Hidden<uint32_t> _ringCursor  = {0};    // next slot to overwrite (ring buffer)
        Hidden<float>    _reclaimTimer = {-1.0f}; // >=0: draining, countdown to free+clear slots
        Hidden<bool>     _hasFired     = {false}; // one-shot already emitted its batch
        Hidden<bool>     _cleared      = {true};  // query: true once all emitted particles are gone
    };

    // How a particle emitter's billboard sprites are blended over the scene.
    // Values must stay in sync with the renderer's per-blend pipeline table.
    enum class ParticleBlendMode : uint8_t {
        Additive = 0,   // glow / energy (default; no sort dependency)
        AlphaBlend = 1, // smoke / dust
        Multiply = 2,   // darkening / shadow wisps
    };

    // Per-emitter appearance (the Niagara-style renderer module). Optional —
    // an emitter without one draws with the defaults below. Gameplay-owned;
    // ParticleRenderSystem gathers these into per-material draw packets each
    // frame, so blend mode and texture are per-emitter (one draw per emitter).
    struct ParticleRendererComponent {
        ParticleBlendMode blendMode = ParticleBlendMode::Additive;
        uint32_t texture = 0xFFFFFFFFu; // renderer TextureId; ~0u = procedural soft disc
        float size = 0.1f;              // world-space billboard half-extent
    };

    // One-shot burst of particles at the entity's current position.
    struct ParticleBurstRequest {
        uint32_t  count    = 64;
        float     speed    = 3.0f;
        float     spread   = 3.14159f;  // full sphere
        float     lifetime = 1.0f;
        glm::vec4 color    = glm::vec4(1.0f);
    };

    // Emotion/mood state that modulates particle emitters via EmitterModulatorComponent.
    enum class EmotionState { Neutral, Joy, Rage, Sorrow, Fear };

    // Drives emitter parameters from an EmotionState (skeleton — extend per project).
    struct EmitterModulatorComponent {
        EmotionState state = EmotionState::Neutral;
        float           transitionSpeed = 1.0f;
        Hidden<float>   _blendTimer     = {0.0f};
    };

    // Flying spell bolt: moves from origin to target along a Bezier arc,
    // emitting particles along the way and bursting on arrival.
    struct SpellBoltComponent {
        glm::vec3 origin;
        glm::vec3 target;
        float         speed     = 15.0f;
        float         arcHeight = 0.5f;
        Hidden<float> _progress = {0.0f}; // [0,1], managed by SpellBoltSystem
    };

}// namespace Vapor
