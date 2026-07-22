#pragma once
#include "Vapor/hidden.hpp"
#include "character_controller.hpp"
#include "graphics_handles.hpp"
#include "graphics_sprite.hpp"
#include "physics_3d.hpp"
#include "render_data.hpp"   // SkyType
#include "vehicle_controller.hpp"
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

    // Entity-level "disabled" tag (the ECS idiom: presence = off, systems
    // exclude it). An inactive entity is skipped by rendering and by the systems
    // that opt in via entt::exclude<InactiveComponent>. This is the whole-entity
    // switch (Unity's SetActive); it is separate from per-drawable `visible`
    // (render-only) and per-component `enabled` (that one component's logic).
    // Toggle it from the inspector's entity "Active" checkbox.
    struct InactiveComponent {};

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

    // Tags the directional light that acts as the moon. TimeOfDaySystem drives
    // its direction (opposite the sun), a cool dim colour, and an intensity that
    // ramps up while the moon is above the horizon (i.e. at night). Put it on a
    // second directional-light entity; LightGatherSystem gathers it after the sun.
    struct MoonComponent {};

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
        // Night-sky visuals (Atmosphere mode): stars + moon that fade in at night.
        float starDensity    = 1000.0f;  // more = smaller/denser stars
        float starBrightness = 15.0f;
        glm::vec3 moonColor  = glm::vec3(0.92f, 0.93f, 1.0f);
        float moonSize       = 0.0010f;  // angular size (1 - cos radius)
        float moonBrightness = 1.2f;
        bool dirty = true;  // set when edited; SkySystem re-pushes to the renderer

        // IBL rebake throttle: SkySystem re-bakes the environment IBL when the
        // sun has moved more than this many degrees since the last bake (a moving
        // sun restales the captured sky). 0 disables it. _lastIblSunDir is the
        // sun direction the IBL was last baked for (runtime, inspector-hidden).
        float iblSunThresholdDeg = 5.0f;
        Hidden<glm::vec3> _lastIblSunDir = {glm::vec3(0.0f)};
    };

    // ── Weather ──────────────────────────────────────────────────────────────

    enum class WeatherState : uint8_t {
        Clear = 0, Cloudy, Overcast, Rain, Thunderstorm, Snow,
    };

    // The blendable slice of a weather state. WeatherSystem lerps between the
    // outgoing and incoming state's params over the transition, so the sky
    // thickens/clears smoothly instead of popping. Defaults = the Clear preset
    // (a default-constructed WeatherComponent resolves to Clear), anchored to
    // the hand-tuned native-Metal cloud settings: a deep 2000-12000 m layer,
    // low coverage, near-zero ambient (high ambient milks out the whole sky).
    struct WeatherParams {
        float cloudCoverage    = 0.25f;
        float cloudDensity     = 0.3f;
        float cloudType        = 0.5f;      // 0 stratus → 1 cumulus
        float cloudLayerBottom = 2000.0f;   // storm states lower the ceiling
        float cloudLayerTop    = 12000.0f;
        float cloudAmbient     = 0.001f;
        float sunDim           = 1.0f;      // multiplies sun/moon light intensity
        float fogDensityMul    = 1.0f;      // multiplies VolumetricFogComponent density
        float windMul          = 1.0f;      // multiplies WindFieldComponent strength
        float iblDim           = 1.0f;      // scales the baked environment (IBL) ambience
        // Multiplies the renderer's cloud sunLightScale: the artistic stand-in
        // for the extinction depth the 6-step light march can't afford — storm
        // decks read darker than self-shadowing alone provides.
        float cloudSunMul      = 1.0f;
        // Cloud ambient (sky-fill) tint: blue under a clear sky, neutral gray
        // overcast, sickly green in a supercell thunderstorm.
        glm::vec3 cloudAmbientColor = glm::vec3(0.5f, 0.6f, 0.9f);
    };

    inline WeatherParams weatherParamsFor(WeatherState s) {
        switch (s) {
            // Anchored to the tuned Clear base (0.25/0.3/0.5, 2000-12000 m,
            // ambient 0.001); storms raise coverage/density, drop the ceiling
            // and flatten toward stratus. Ambient stays in the tuned ~0.001-0.01
            // range — it is a full-sky glow, not a per-cloud fill.
            // cloudSun multiplies the panel base sunLightScale (0.85): storms
            // steepen it hard so the deck's ABSOLUTE lit brightness stays dark
            // (0.85 x 0.12 ≈ 0.10) even though the base is bright for clear skies.
            //                     coverage density type   bottom   top      ambient sunDim fogMul windMul iblDim cloudSun ambientTint
            case WeatherState::Cloudy:       return { 0.45f, 0.35f, 0.40f, 1600.0f, 10000.0f, 0.002f, 0.80f, 1.2f, 1.3f, 0.85f, 0.80f, {0.55f, 0.60f, 0.78f} };
            case WeatherState::Overcast:     return { 0.75f, 0.50f, 0.20f, 1000.0f,  6000.0f, 0.004f, 0.40f, 1.6f, 1.6f, 0.50f, 0.42f, {0.55f, 0.57f, 0.62f} };
            case WeatherState::Rain:         return { 0.85f, 0.65f, 0.15f,  800.0f,  5000.0f, 0.003f, 0.25f, 2.2f, 2.0f, 0.40f, 0.30f, {0.45f, 0.50f, 0.58f} };
            case WeatherState::Thunderstorm: return { 0.95f, 0.85f, 0.10f,  600.0f,  4500.0f, 0.002f, 0.12f, 2.5f, 3.0f, 0.25f, 0.12f, {0.42f, 0.50f, 0.42f} };
            case WeatherState::Snow:         return { 0.70f, 0.45f, 0.30f, 1100.0f,  6000.0f, 0.006f, 0.45f, 1.8f, 1.2f, 0.60f, 0.62f, {0.72f, 0.76f, 0.85f} };
            case WeatherState::Clear:
            default:                         return {};
        }
    }

    inline WeatherParams mixWeatherParams(const WeatherParams& a, const WeatherParams& b, float t) {
        auto L = [t](float x, float y) { return x + (y - x) * t; };
        WeatherParams r;
        r.cloudCoverage    = L(a.cloudCoverage,    b.cloudCoverage);
        r.cloudDensity     = L(a.cloudDensity,     b.cloudDensity);
        r.cloudType        = L(a.cloudType,        b.cloudType);
        r.cloudLayerBottom = L(a.cloudLayerBottom, b.cloudLayerBottom);
        r.cloudLayerTop    = L(a.cloudLayerTop,    b.cloudLayerTop);
        r.cloudAmbient     = L(a.cloudAmbient,     b.cloudAmbient);
        r.sunDim           = L(a.sunDim,           b.sunDim);
        r.fogDensityMul    = L(a.fogDensityMul,    b.fogDensityMul);
        r.windMul          = L(a.windMul,          b.windMul);
        r.iblDim           = L(a.iblDim,           b.iblDim);
        r.cloudSunMul      = L(a.cloudSunMul,      b.cloudSunMul);
        r.cloudAmbientColor = a.cloudAmbientColor + (b.cloudAmbientColor - a.cloudAmbientColor) * t;
        return r;
    }

    // Weather authoring — the scene's weather state machine (singleton, put it
    // on the environment/Sky entity next to SkyComponent). WeatherSystem blends
    // toward `state` over `transitionSeconds` and orchestrates the downstream
    // media: volumetric clouds (IRenderer::setClouds — owning them while
    // driveClouds is set), sun/moon dimming (consumed by TimeOfDaySystem), fog
    // density and wind strength multipliers (consumed by VolumetricFogSystem /
    // WindSystem), precipitation emitters (toggled via InactiveComponent on
    // PrecipitationComponent-tagged entities) and lightning flashes
    // (LightningComponent-tagged point lights + a cloud-interior glow).
    struct WeatherComponent {
        WeatherState state = WeatherState::Clear;  // target; blends over transitionSeconds
        float transitionSeconds = 15.0f;
        bool  enabled = true;
        bool  driveClouds = true;  // push cloud params (turns the clouds pass on)
        // Lightning (Thunderstorm): random strike interval + flash strength.
        float lightningMinInterval = 4.0f;   // seconds
        float lightningMaxInterval = 14.0f;
        float lightningIntensity   = 300.0f; // peak intensity on tagged lights

        // Runtime (WeatherSystem-owned, inspector-hidden).
        Hidden<WeatherParams> _from = {};        // params blending FROM (snapshotted on state change)
        Hidden<float>    _blend = {1.0f};        // 0→1 progress toward `state`
        Hidden<uint8_t>  _lastState = {0};       // change detection (== WeatherState::Clear)
        Hidden<WeatherParams> _resolved = {};    // this frame's blended params (consumers read this)
        Hidden<float>    _lightningTimer = {0.0f};  // countdown to the next strike
        Hidden<float>    _flashAge = {1e9f};        // seconds since the strike began
        Hidden<uint32_t> _rng = {0u};               // xorshift32 state (0 = seed on first use)
    };

    // Tags a particle emitter entity as weather precipitation. WeatherSystem
    // starts it (removes InactiveComponent, sets emitting) while the weather
    // calls for its kind — Rain during Rain/Thunderstorm, Snow during Snow —
    // and gracefully stops it otherwise (emitting = false, so airborne
    // particles finish falling). Author precipitation emitters with
    // "inactive": {} so they cost nothing until their first storm.
    struct PrecipitationComponent {
        enum class Kind : uint8_t { Rain = 0, Snow };
        Kind kind = Kind::Rain;
        // Keep the emitter centered above the active camera so precipitation
        // exists wherever the player looks (WeatherSystem moves the transform;
        // pair with a box emitExtents so the sheet has area).
        bool  followCamera = true;
        float followHeight = 18.0f;  // meters above the camera
    };

    // Tags a (point) light as a lightning flash source. Intensity is OWNED by
    // WeatherSystem: zero except during Thunderstorm strikes, when it spikes
    // with a double-pulse envelope. Author the light with intensity 0.
    struct LightningComponent {};

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
        // Moonlight: drives the MoonComponent-tagged directional light. The moon
        // sits opposite the sun, so it is up (and lit) while the sun is down.
        float maxMoonIntensity = 0.4f;    // moon intensity when high at night
        glm::vec3 moonLightColor = glm::vec3(0.55f, 0.65f, 0.9f);  // cool blue
        bool  paused = false;
    };

    // Screen-space 2D sprite (atlas frame drawn via the 2D canvas batch).
    // Position comes from TransformComponent, in screen pixels under a
    // perspective camera.
    struct Sprite2DComponent {
        AtlasHandle atlas;
        uint16_t frameIndex = 0;

        glm::vec2 size = {1.0f, 1.0f};       // Screen pixels
        glm::vec2 pivot = {0.5f, 0.5f};      // Anchor point (0-1)
        glm::vec4 tint = {1, 1, 1, 1};       // Color tint
        int sortingLayer = 0;
        int orderInLayer = 0;
        bool flipX = false;
        bool flipY = false;
        bool visible = true;
    };

    // World-space 3D sprite: an atlas frame on a quad placed in the world.
    // billboard = true re-orients it to face the camera every frame (particles,
    // markers, floating labels); false uses the entity's TransformComponent
    // orientation (posters, decals, ground quads). size is in world units.
    struct Sprite3DComponent {
        AtlasHandle atlas;
        uint16_t frameIndex = 0;

        glm::vec2 size = {1.0f, 1.0f};       // World units
        glm::vec4 tint = {1, 1, 1, 1};       // Color tint
        bool billboard = false;
        bool flipX = false;
        bool flipY = false;
        bool visible = true;
    };

    // 2D canvas text. Position comes from TransformComponent (screen-space
    // pixels under a perspective camera, same convention as Sprite2DComponent).
    // The font is referenced by path; the render system resolves and caches
    // the FontHandle, so this stays a pure data component.
    struct Text2DComponent {
        std::string text;
        std::string font;                    // e.g. "fonts/NotoSans-SemiBold.ttf"
        float fontSize = 48.0f;              // rasterized base size (loadFont)
        float scale = 1.0f;
        glm::vec4 color = {1, 1, 1, 1};
        bool visible = true;
    };

    // 2D canvas shape primitive; position from TransformComponent.
    struct Shape2DComponent {
        enum class Kind : uint8_t {
            Quad,      // filled quad of `size`
            Rect,      // outline of `size` at `thickness`
            Circle,    // filled circle of `radius`
            Triangle,  // filled; position + p1/p2 offsets are the three verts
        };
        Kind kind = Kind::Quad;
        glm::vec2 size = {20.0f, 20.0f};
        float radius = 10.0f;
        float thickness = 1.0f;
        glm::vec2 p1 = {0.0f, 0.0f};
        glm::vec2 p2 = {0.0f, 0.0f};
        glm::vec4 color = {1, 1, 1, 1};
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

    // Opt-in per-light volumetric fog (the expensive raymarch). One singleton per
    // scene; VolumetricFogSystem pushes it to the renderer each frame. The cheap
    // always-on global fog is renderer-side "Height Fog"; this is the upgrade you
    // add only where you want light shafts. First version is a single global
    // volume — bounds + volume-blend come later.
    struct VolumetricFogComponent {
        bool  enabled = true;
        float density = 0.02f;
        float heightFalloff = 0.1f;
        float baseHeight = 0.0f;
        float maxHeight = 100.0f;
        float anisotropy = 0.6f;
        float ambientIntensity = 0.3f;
        float noiseScale = 0.01f;
        float noiseIntensity = 0.5f;
        float windSpeed = 1.0f;
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
        // Emission volume: zero = point emitter (cone around emitDirection);
        // non-zero half-extents spawn particles anywhere in a box centered on
        // the entity (rain/snow sheets, area steam).
        glm::vec3 emitExtents = glm::vec3(0.0f);
        // Particles below this world Y die on the spot (cheap ground collision
        // for rain/sparks). Very negative = off. Finite-lifetime only — the
        // GPU sim ignores it for immortal particles.
        float groundKillY = -1.0e9f;
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
        // 0 = camera-facing billboard. > 0 stretches the quad along the
        // particle's velocity (screen-aligned): half-length becomes
        // size * (1 + velocityStretch * speed). Rain streaks ≈ 0.1-0.2.
        float velocityStretch = 0.0f;
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
