#include "scene_blueprint.hpp"

#include "asset_manager.hpp"
#include "asset_serializer.hpp"
#include "components.hpp"
#include "file_system.hpp"
#include "fsm.hpp"
#include "mesh_builder.hpp"
#include "meshlet_builder.hpp"
#include "procgen_patterns.hpp"  // TilePanelDesc/buildTilePanel for the "tilePanel" generator
#include "render_scene.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>

using namespace Vapor;

namespace Vapor {

using json = nlohmann::json;

// ── Entity name scope (see detail::entityNameScope in the header) ────────────

namespace detail {
    static const std::unordered_map<std::string, entt::entity>* g_entityNameScope = nullptr;

    const std::unordered_map<std::string, entt::entity>* entityNameScope() {
        return g_entityNameScope;
    }
    EntityNameScopeGuard::EntityNameScopeGuard(const std::unordered_map<std::string, entt::entity>* scope) {
        g_entityNameScope = scope;
    }
    EntityNameScopeGuard::~EntityNameScopeGuard() {
        g_entityNameScope = nullptr;
    }
}// namespace detail

// ── JSON field helpers ───────────────────────────────────────────────────────

static glm::vec3 readVec3(const json& j, const char* key, glm::vec3 fallback) {
    if (!j.contains(key)) return fallback;
    const auto& a = j.at(key);
    if (!a.is_array() || a.size() < 3) return fallback;
    return { a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
}

// Rotation: "rotation" is a quaternion [x, y, z, w] (glTF layout);
// "rotationEuler" is XYZ degrees for hand-authored scenes. Quaternion wins if
// both are present.
static glm::quat readRotation(const json& j) {
    if (j.contains("rotation")) {
        const auto& a = j.at("rotation");
        if (a.is_array() && a.size() >= 4)
            return glm::quat(a[3].get<float>(), a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
    }
    if (j.contains("rotationEuler")) {
        const auto& a = j.at("rotationEuler");
        if (a.is_array() && a.size() >= 3) {
            const glm::vec3 rad = glm::radians(glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>()));
            return glm::quat(rad);
        }
    }
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

static LightBlueprint parseLight(const json& j) {
    LightBlueprint light;
    const std::string type = j.value("type", "point");
    if (type == "directional")
        light.type = LightBlueprint::Type::Directional;
    else if (type == "spot")
        light.type = LightBlueprint::Type::Spot;
    else
        light.type = LightBlueprint::Type::Point;
    light.color = readVec3(j, "color", glm::vec3(1.0f));
    light.intensity = j.value("intensity", 1.0f);
    light.range = j.value("range", 0.0f);
    // Cone angles are authored in degrees in scene JSON (human-friendly), but
    // stored in radians (the glTF/USD convention the importers emit).
    if (j.contains("innerConeDeg")) light.innerConeAngle = glm::radians(j.value("innerConeDeg", 0.0f));
    if (j.contains("outerConeDeg")) light.outerConeAngle = glm::radians(j.value("outerConeDeg", 45.0f));
    return light;
}

// ── Parse (pure, no I/O) ─────────────────────────────────────────────────────

// ── Component appliers: engine defaults ──────────────────────────────────────
// Every data-only engine component registers here; gameplay components are the
// app's to register (same API) before its first instantiate(). Physics
// components are intentionally absent for now — a RigidbodyComponent needs a
// live Jolt body, which is a hand-written applier with a Physics3D in hand.

BlueprintComponents& BlueprintComponents::instance() {
    static BlueprintComponents registry = [] {
        BlueprintComponents r;
        r.registerComponent<InactiveComponent>("inactive");
        r.registerComponent<SunComponent>("sun");
        r.registerComponent<MoonComponent>("moon");
        r.registerComponent<PointLightComponent>("pointLight");
        r.registerComponent<DirectionalLightComponent>("directionalLight");
        r.registerComponent<SpotLightComponent>("spotLight");
        r.registerComponent<RectLightComponent>("rectLight");
        r.registerComponent<SkyComponent>("sky");
        r.registerComponent<TimeOfDayComponent>("timeOfDay");
        r.registerComponent<VolumetricFogComponent>("volumetricFog");
        // water: nested grid/look/caustics/spectrum groups, which the PFR path
        // walks as aggregates. WaterSystem pushes it to the renderer.
        r.registerComponent<WaterComponent>("water");
        // weather: hand-written for the string-authored state (the PFR path
        // only reads enums as integers). Runtime blend fields stay at their
        // defaults — a loaded scene starts settled in its authored state.
        r.registerApplier("weather", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
            WeatherComponent w;
            const std::string s = j.value("state", "clear");
            w.state = s == "cloudy"         ? WeatherState::Cloudy
                      : s == "overcast"     ? WeatherState::Overcast
                      : s == "rain"         ? WeatherState::Rain
                      : s == "thunderstorm" ? WeatherState::Thunderstorm
                      : s == "snow"         ? WeatherState::Snow
                                            : WeatherState::Clear;
            w._lastState = static_cast<uint8_t>(w.state);  // no transition on load
            w._from = weatherParamsFor(w.state);
            w.transitionSeconds     = j.value("transitionSeconds", w.transitionSeconds);
            w.enabled               = j.value("enabled", w.enabled);
            w.driveClouds           = j.value("driveClouds", w.driveClouds);
            w.lightningMinInterval  = j.value("lightningMinInterval", w.lightningMinInterval);
            w.lightningMaxInterval  = j.value("lightningMaxInterval", w.lightningMaxInterval);
            w.lightningIntensity    = j.value("lightningIntensity", w.lightningIntensity);
            reg.emplace_or_replace<WeatherComponent>(e, w);
        });
        // precipitation: hand-written for the string-authored kind.
        r.registerApplier("precipitation", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
            PrecipitationComponent p;
            p.kind = j.value("kind", std::string("rain")) == "snow"
                         ? PrecipitationComponent::Kind::Snow
                         : PrecipitationComponent::Kind::Rain;
            p.followCamera = j.value("followCamera", p.followCamera);
            p.followHeight = j.value("followHeight", p.followHeight);
            reg.emplace_or_replace<PrecipitationComponent>(e, p);
        });
        r.registerComponent<LightningComponent>("lightning");
        r.registerComponent<VirtualCameraComponent>("virtualCamera");
        r.registerComponent<FlyCameraComponent>("flyCamera");
        r.registerComponent<FollowCameraComponent>("followCamera");
        r.registerComponent<WindFieldComponent>("windField");
        // Procedural world: a data-only config whose Hidden<> state the owning
        // VoxelVolumeSystem creates on first sight — exactly the shape the PFR
        // applier authors.
        r.registerComponent<VoxelVolumeComponent>("voxelVolume");
        r.registerComponent<ParticleEmitterComponent>("particleEmitter");
        r.registerComponent<ParticleAttractorComponent>("particleAttractor");
        r.registerComponent<ParticleRendererComponent>("particleRenderer");

        r.registerComponent<Text2DComponent>("text2D");
        // shape2D: hand-written for the string-authored kind (the PFR path
        // only reads enums as integers). Parsed manually so the applier also
        // works in no-Boost builds.
        r.registerApplier("shape2D", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
            Shape2DComponent shape;
            const std::string kind = j.value("kind", "quad");
            shape.kind = kind == "rect"       ? Shape2DComponent::Kind::Rect
                         : kind == "circle"   ? Shape2DComponent::Kind::Circle
                         : kind == "triangle" ? Shape2DComponent::Kind::Triangle
                                              : Shape2DComponent::Kind::Quad;
            const auto readV2 = [&](const char* key, glm::vec2 fallback) {
                const auto it = j.find(key);
                if (it == j.end() || !it->is_array() || it->size() < 2) return fallback;
                return glm::vec2{ (*it)[0].get<float>(), (*it)[1].get<float>() };
            };
            shape.size = readV2("size", shape.size);
            shape.p1 = readV2("p1", shape.p1);
            shape.p2 = readV2("p2", shape.p2);
            shape.radius = j.value("radius", shape.radius);
            shape.thickness = j.value("thickness", shape.thickness);
            shape.visible = j.value("visible", shape.visible);
            if (const auto it = j.find("color"); it != j.end() && it->is_array() && it->size() >= 4) {
                shape.color = { (*it)[0].get<float>(), (*it)[1].get<float>(),
                                (*it)[2].get<float>(), (*it)[3].get<float>() };
            }
            reg.emplace_or_replace<Shape2DComponent>(e, shape);
        });

        // Physics: data-only here — no live Jolt body is created. The app's
        // body-create system observes {Rigidbody, Transform, Collider} with an
        // invalid BodyHandle and creates/registers the body reactively.
        r.registerComponent<BoxColliderComponent>("boxCollider");
        r.registerComponent<SphereColliderComponent>("sphereCollider");
        r.registerApplier("rigidbody", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
            RigidbodyComponent rb;
            const std::string motion = j.value("motionType", "dynamic");
            rb.motionType = motion == "static"      ? BodyMotionType::Static
                            : motion == "kinematic" ? BodyMotionType::Kinematic
                                                    : BodyMotionType::Dynamic;
            rb.syncToPhysics = j.value("syncToPhysics", rb.syncToPhysics);
            rb.syncFromPhysics = j.value("syncFromPhysics", rb.syncFromPhysics);
            reg.emplace_or_replace<RigidbodyComponent>(e, rb);
        });

        // FSM: states/transitions authored by name; actions stay out of the
        // data entirely — FSMSystem emits FSMStateChangeEvent and reaction
        // systems consume it. Also seeds the runtime state + event queue so an
        // authored FSM is live without further setup.
        r.registerApplier("fsm", [](entt::registry& reg, entt::entity e, const nlohmann::json& j) {
            FSMDefinition def;
            if (j.contains("states") && j.at("states").is_array())
                for (const auto& s : j.at("states"))
                    if (s.is_string()) def.stateNames.push_back(s.get<std::string>());
            if (def.stateNames.empty()) {
                fmt::print(stderr, "blueprint: fsm with no states — skipped\n");
                return;
            }
            def.initialState = def.getStateIndex(j.value("initial", def.stateNames.front()));
            if (j.contains("transitions") && j.at("transitions").is_array()) {
                for (const auto& t : j.at("transitions")) {
                    if (!t.is_object()) continue;
                    def.eventTransitions.emplace_back(
                        def.getStateIndex(t.value("from", "")), def.getStateIndex(t.value("to", "")),
                        t.value("event", ""), t.value("minTime", 0.0f)
                    );
                }
            }
            if (j.contains("timed") && j.at("timed").is_array()) {
                for (const auto& t : j.at("timed")) {
                    if (!t.is_object()) continue;
                    def.timedTransitions.emplace_back(
                        def.getStateIndex(t.value("from", "")), def.getStateIndex(t.value("to", "")),
                        t.value("duration", 0.0f)
                    );
                }
            }
            // No FSMStateComponent here: FSMInitSystem seeds it on first update
            // (definition-without-state is its trigger) AND emits the initial
            // state-enter event, which reaction systems may rely on.
            reg.emplace_or_replace<FSMEventQueue>(e);
            reg.emplace_or_replace<FSMDefinition>(e, std::move(def));
        });
        return r;
    }();
    return registry;
}

static void parseEntityRec(const json& j, int parentIndex, SceneBlueprint& out) {
    EntityBlueprint e;
    e.name = j.value("name", "");
    e.position = readVec3(j, "position", glm::vec3(0.0f));
    e.rotation = readRotation(j);
    e.scale = readVec3(j, "scale", glm::vec3(1.0f));
    e.parent = parentIndex;
    e.source = j.value("source", "");
    e.prefab = j.value("prefab", "");
    if (j.contains("primitive") && j.at("primitive").is_object()) {
        const auto& p = j.at("primitive");
        const std::string shape = p.value("shape", "");
        if (shape == "cube") e.primitive.shape = PrimitiveBlueprint::Shape::Cube;
        else if (shape == "capsule") e.primitive.shape = PrimitiveBlueprint::Shape::Capsule;
        else if (shape == "cone") e.primitive.shape = PrimitiveBlueprint::Shape::Cone;
        else if (shape == "triforce") e.primitive.shape = PrimitiveBlueprint::Shape::Triforce;
        else fmt::print(stderr, "parseSceneBlueprint: unknown primitive shape '{}'\n", shape);
        e.primitive.size = p.value("size", 1.0f);
        e.primitive.height = p.value("height", 1.0f);
        // Material by declared name — materials[] parses before entities[].
        if (p.contains("material") && p.at("material").is_string()) {
            const std::string matName = p.at("material").get<std::string>();
            for (size_t m = 0; m < out.materials.size(); ++m) {
                if (out.materials[m] && out.materials[m]->name == matName) {
                    e.primitive.material = static_cast<int>(m);
                    break;
                }
            }
            if (e.primitive.material < 0)
                fmt::print(stderr, "parseSceneBlueprint: primitive material '{}' not declared\n", matName);
        }
    }
    if (j.contains("procMesh") && j.at("procMesh").is_object()) {
        const auto& pm = j.at("procMesh");
        // Type-checked rather than pm.value(): that throws type_error.302 on a
        // wrong-typed field, and parse promises ok=false over a throw.
        const auto genIt = pm.find("generator");
        if (genIt != pm.end() && genIt->is_string()) e.procMesh.generator = genIt->get<std::string>();
        if (e.procMesh.generator.empty()) {
            fmt::print(stderr, "parseSceneBlueprint: procMesh needs a \"generator\"\n");
        } else {
            // Params ride as text so the blueprint (and its cook) stay
            // independent of any particular generator's parameter struct.
            if (pm.contains("params") && pm.at("params").is_object())
                e.procMesh.paramsJson = pm.at("params").dump();
            // Bucket name -> material, resolved by declared name like
            // primitives are (materials[] parses before entities[]).
            if (pm.contains("materials") && pm.at("materials").is_object()) {
                for (const auto& [bucket, value] : pm.at("materials").items()) {
                    if (!value.is_string()) continue;
                    const std::string matName = value.get<std::string>();
                    int index = -1;
                    for (size_t m = 0; m < out.materials.size(); ++m) {
                        if (out.materials[m] && out.materials[m]->name == matName) {
                            index = static_cast<int>(m);
                            break;
                        }
                    }
                    if (index < 0)
                        fmt::print(stderr,
                                   "parseSceneBlueprint: procMesh material '{}' (bucket '{}') "
                                   "not declared\n", matName, bucket);
                    e.procMesh.bucketMaterials.emplace_back(bucket, index);
                }
            }
        }
    }
    if (j.contains("light")) {
        out.lights.push_back(parseLight(j.at("light")));
        e.lights.push_back(static_cast<int>(out.lights.size()) - 1);
    }
    if (j.contains("components") && j.at("components").is_object()) e.componentsJson = j.at("components").dump();
    const int selfIndex = static_cast<int>(out.entities.size());
    out.entities.push_back(std::move(e));
    if (j.contains("children")) {
        for (const auto& c : j.at("children"))
            parseEntityRec(c, selfIndex, out);
    }
}

// ── Procedural texture generators ───────────────────────────────────────────

namespace {

// Small typed accessors: scene JSON is hand-authored, so a wrong-typed or
// missing field falls back to the generator's own default rather than throwing.
float pf(const json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<float>() : fallback;
}
// JSON draws no line between 4 and 4.0, so an integer field must accept both:
// rejecting the float spelling substitutes the generator's default without a
// word, and a mistyped "size": 32.0 would then bake a 256px image instead of a
// 32px one. Exact integers keep their exact path; a float is range-checked as
// a double FIRST, because get<int64_t>() on something like 1e300 is undefined.
int pi(const json& j, const char* key, int fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    if (it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        return (v < INT32_MIN || v > INT32_MAX) ? fallback : int(v);
    }
    const double d = it->get<double>();
    if (!(d >= double(INT32_MIN) && d <= double(INT32_MAX))) return fallback;
    return int(d);
}
uint32_t pu(const json& j, const char* key, uint32_t fallback) {
    // Reads the full unsigned range: seeds are naturally uint32, and routing
    // them through int would drop everything at or above 2^31 onto the
    // fallback. Negatives and out-of-range values keep the fallback.
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    if (it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        return (v < 0 || v > int64_t(UINT32_MAX)) ? fallback : uint32_t(v);
    }
    const double d = it->get<double>();
    if (!(d >= 0.0 && d <= double(UINT32_MAX))) return fallback;
    return uint32_t(d);
}
bool pb(const json& j, const char* key, bool fallback) {
    // json::value() would be shorter but throws type_error.302 on a
    // wrong-typed field, and these params come straight off disk.
    const auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}
glm::vec3 pv3(const json& j, const char* key, glm::vec3 fallback) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    // Elements must be type-checked before get<float>(): parseSceneBlueprint
    // parses with allow_exceptions=false and promises ok=false rather than a
    // throw, but get<float>() on a non-number raises nlohmann::type_error.
    if (it->is_array() && it->size() >= 3) {
        const json& a = *it;
        if (!a[0].is_number() || !a[1].is_number() || !a[2].is_number()) return fallback;
        return { a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
    }
    // A scalar is accepted as a grey triple — "rimTint": 0.86 reads naturally.
    if (it->is_number()) return glm::vec3(it->get<float>());
    return fallback;
}

// FNV-1a over the BAKED PIXELS. Hashing the output rather than the params is
// what makes the uri a sound texture-cache key: two entries whose params differ
// only in spelling (`4` vs `4.0`, a field left at its default vs written out) bake
// identical images and must share one GPU upload, while any params change that
// actually alters a texel produces a different key.
std::string contentDigest(const std::vector<Uint8>& pixels) {
    uint64_t h = 1469598103934665603ull;
    for (Uint8 c : pixels) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return fmt::format("{:016x}", h);
}

}  // namespace

TextureGenerators& TextureGenerators::instance() {
    static TextureGenerators registry = [] {
        TextureGenerators r;
        using namespace Vapor::proctex;
        // The version is provenance only (it rides in the uri); the cache key
        // is the pixel hash, so a changed generator invalidates on its own.
        r.add("tileAlbedo", 1, [](const json& p) {
            return tileAlbedo(pv3(p, "base", glm::vec3(0.8f)), pv3(p, "rimTint", glm::vec3(0.85f)),
                              pu(p, "seed", 0u), pu(p, "size", 256u));
        });
        r.add("tileNormal", 1, [](const json& p) {
            return tileNormal(pf(p, "pillow", 0.3f), pf(p, "waviness", 0.1f),
                              pu(p, "seed", 0u), pu(p, "size", 256u));
        });
        r.add("tileRoughness", 1, [](const json& p) {
            return tileRoughness(pf(p, "centerRoughness", 0.1f), pf(p, "rimRoughness", 0.3f),
                                 pu(p, "seed", 0u), pu(p, "size", 256u));
        });
        r.add("noisyAlbedo", 1, [](const json& p) {
            return noisyAlbedo(pv3(p, "base", glm::vec3(0.7f)), pf(p, "variation", 0.1f),
                               pi(p, "period", 8), pu(p, "seed", 0u), pu(p, "size", 256u));
        });
        r.add("noisyNormal", 1, [](const json& p) {
            return noisyNormal(pf(p, "strength", 0.3f), pi(p, "period", 8),
                               pu(p, "seed", 0u), pu(p, "size", 256u));
        });
        r.add("brushedMetalAlbedo", 1, [](const json& p) {
            return brushedMetalAlbedo(pv3(p, "base", glm::vec3(0.75f)), pu(p, "seed", 0u),
                                      pu(p, "size", 256u));
        });
        r.add("brushedMetalRoughness", 1, [](const json& p) {
            return brushedMetalRoughness(pf(p, "base", 0.3f), pu(p, "seed", 0u),
                                         pu(p, "size", 256u));
        });
        return r;
    }();
    return registry;
}

std::shared_ptr<Image> TextureGenerators::generate(const std::string& name,
                                                   const nlohmann::json& params) const {
    const auto it = m_generators.find(name);
    if (it == m_generators.end()) return nullptr;

    // Both arms must be lvalues: a `json::object()` temporary here would make
    // the conditional a prvalue, deep-copying `params` on every call (and
    // leaving the binding's validity resting on lifetime extension).
    static const json kNoParams = json::object();
    const json& p = params.is_object() ? params : kNoParams;
    proctex::TextureData baked = it->second.fn(p);
    if (baked.empty()) {
        fmt::print(stderr, "TextureGenerators: '{}' produced an empty image\n", name);
        return nullptr;
    }

    auto img = std::make_shared<Image>();
    img->width = baked.size;
    img->height = baked.size;
    img->channelCount = 4;
    img->byteArray = std::move(baked.pixels);
    // Synthetic, content-addressed key. Never opened as a file: byteArray is
    // populated, and the stub-resolution loop skips images that already hold
    // pixels. The renderer's texture cache dedupes on exactly this string; the
    // generator name and version ride along as human-readable provenance.
    img->uri = fmt::format("proc://{}#v{}-{}", name, it->second.version,
                           contentDigest(img->byteArray));
    return img;
}

// ── Procedural mesh generators ──────────────────────────────────────────────

MeshGenerators& MeshGenerators::instance() {
    static MeshGenerators registry = [] {
        MeshGenerators r;
        using namespace Vapor::procgen;

        // Thin wrappers over the procgen primitives. Anything more structural
        // than these (a whole room, a staircase) belongs in an app-registered
        // composite generator, where the cross-element decisions live.
        r.add("tilePanel", 1, [](const json& p) {
            GeneratedContent out;
            TilePanelDesc d;
            d.width = pf(p, "width", 1.0f);
            d.height = pf(p, "height", 1.0f);
            d.tileW = pf(p, "tileW", 0.20f);
            d.tileH = pf(p, "tileH", 0.20f);
            d.groutWidth = pf(p, "groutWidth", 0.010f);
            d.groutDepth = pf(p, "groutDepth", 0.005f);
            d.bevel = pf(p, "bevel", 0.007f);
            d.uPhase = pf(p, "uPhase", 0.0f);
            d.vPhase = pf(p, "vPhase", 0.0f);
            buildTilePanel(pv3(p, "origin", glm::vec3(0.0f)),
                           pv3(p, "u", glm::vec3(1, 0, 0)), pv3(p, "v", glm::vec3(0, 1, 0)),
                           d, out.bucket("tiles"), out.bucket("grout"));
            return out;
        });
        r.add("lathe", 1, [](const json& p) {
            GeneratedContent out;
            std::vector<glm::vec2> profile;
            const auto it = p.find("profile");
            if (it != p.end() && it->is_array()) {
                for (const auto& pt : *it)
                    if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number())
                        profile.push_back({ pt[0].get<float>(), pt[1].get<float>() });
            }
            lathe(profile, pi(p, "segments", 24), out.bucket("surface"));
            return out;
        });
        r.add("tube", 1, [](const json& p) {
            GeneratedContent out;
            std::vector<glm::vec3> path;
            const auto it = p.find("path");
            if (it != p.end() && it->is_array()) {
                for (const auto& pt : *it)
                    if (pt.is_array() && pt.size() >= 3 && pt[0].is_number() && pt[1].is_number() &&
                        pt[2].is_number())
                        path.push_back({ pt[0].get<float>(), pt[1].get<float>(), pt[2].get<float>() });
            }
            sweepTube(path, pf(p, "radius", 0.05f), pi(p, "sides", 12), pf(p, "vTile", 0.25f),
                      out.bucket("surface"), pb(p, "capEnds", true));
            return out;
        });
        r.add("extrude", 1, [](const json& p) {
            GeneratedContent out;
            auto readRing = [](const json& arr) {
                std::vector<glm::vec2> ring;
                if (!arr.is_array()) return ring;
                for (const auto& pt : arr)
                    if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number())
                        ring.push_back({ pt[0].get<float>(), pt[1].get<float>() });
                return ring;
            };
            std::vector<glm::vec2> outer;
            std::vector<std::vector<glm::vec2>> holes;
            const auto o = p.find("outline");
            if (o != p.end()) outer = readRing(*o);
            const auto h = p.find("holes");
            if (h != p.end() && h->is_array())
                for (const auto& ring : *h) holes.push_back(readRing(ring));
            const float depth = pf(p, "depth", 0.1f);
            if (depth > 0.0f)
                extrudePolygon(pv3(p, "origin", glm::vec3(0.0f)),
                               pv3(p, "u", glm::vec3(1, 0, 0)), pv3(p, "v", glm::vec3(0, 1, 0)),
                               outer, holes, depth, pf(p, "uvScale", 1.0f), out.bucket("surface"));
            else  // a flat cap: the same outline without the walls
                polygonCap(pv3(p, "origin", glm::vec3(0.0f)),
                           pv3(p, "u", glm::vec3(1, 0, 0)), pv3(p, "v", glm::vec3(0, 1, 0)),
                           outer, holes, pf(p, "uvScale", 1.0f), out.bucket("surface"));
            return out;
        });
        r.add("boxes", 1, [](const json& p) {
            GeneratedContent out;
            std::vector<Box3> solid;
            auto readBoxes = [&](const json& arr, std::vector<Box3>& dst) {
                if (!arr.is_array()) return;
                for (const auto& b : arr) {
                    if (!b.is_object()) continue;
                    const glm::vec3 lo = pv3(b, "min", glm::vec3(0.0f));
                    const glm::vec3 hi = pv3(b, "max", glm::vec3(0.0f));
                    dst.push_back({ glm::min(lo, hi), glm::max(lo, hi) });
                }
            };
            const auto add = p.find("add");
            if (add != p.end()) readBoxes(*add, solid);
            const auto sub = p.find("subtract");
            if (sub != p.end() && sub->is_array()) {
                std::vector<Box3> cutters;
                readBoxes(*sub, cutters);
                for (const Box3& c : cutters) subtractBox(solid, c);
            }
            emitBoxes(solid, pf(p, "uvScale", 1.0f), out.bucket("surface"));
            boxesToColliders(solid, out.colliders);
            return out;
        });
        return r;
    }();
    return registry;
}

GeneratedContent MeshGenerators::generate(const std::string& name,
                                          const nlohmann::json& params) const {
    GeneratedContent out;
    const auto it = m_generators.find(name);
    if (it == m_generators.end()) return out;

    static const json kNoParams = json::object();
    out = it->second.fn(params.is_object() ? params : kNoParams);

    // Drop geometry that would throw out of Mesh::initialize() (MikkTSpace
    // rejects degenerate UV triangles) or render inside-out. A generator bug
    // must cost its own bucket, not the whole scene load.
    for (auto bucketIt = out.buckets.begin(); bucketIt != out.buckets.end();) {
        if (bucketIt->second.empty()) {
            bucketIt = out.buckets.erase(bucketIt);
            continue;
        }
        const procgen::ValidationReport report = procgen::validate(bucketIt->second);
        if (!report.ok()) {
            fmt::print(stderr,
                       "MeshGenerators: '{}' bucket '{}' failed validation "
                       "({} tris: {} OOB, {} non-finite, {} degenerate, {} bad UV, {} inverted) "
                       "— dropped\n",
                       name, bucketIt->first, report.triangles, report.outOfBoundsIndices,
                       report.nonFinite, report.degenerateGeo, report.degenerateUV,
                       report.windingMismatch);
            bucketIt = out.buckets.erase(bucketIt);
            continue;
        }
        ++bucketIt;
    }
    return out;
}

// One declared material. A texture field holds either "@name" (a texture from
// the scene's "textures" block, already baked) or an asset path, for which
// parse creates a uri-only Image stub per unique path (pure — no I/O) that
// loadSceneBlueprint fills in afterwards.
static void parseMaterial(const json& j, SceneBlueprint& out,
                          const std::unordered_map<std::string, std::shared_ptr<Image>>& generated) {
    auto material = std::make_shared<Material>();
    material->name = j.value("name", "");
    if (j.contains("baseColorFactor")) {
        const auto& a = j.at("baseColorFactor");
        if (a.is_array() && a.size() >= 4)
            material->baseColorFactor = { a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>() };
    }
    material->metallicFactor = j.value("metallicFactor", material->metallicFactor);
    material->roughnessFactor = j.value("roughnessFactor", material->roughnessFactor);
    material->clearcoat = j.value("clearcoat", material->clearcoat);
    material->clearcoatGloss = j.value("clearcoatGloss", material->clearcoatGloss);
    material->useIBL = j.value("useIBL", material->useIBL);
    {
        const std::string type = j.value("type", "");
        if (type == "iridescent") material->materialType = MaterialType::Iridescent;
        else if (!type.empty() && type != "pbr")
            fmt::print(stderr, "parseSceneBlueprint: unknown material type '{}'\n", type);
    }
    if (j.contains("emissiveFactor")) {
        const auto& a = j.at("emissiveFactor");
        if (a.is_array() && a.size() >= 3)
            material->emissiveFactor = { a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
    }
    const auto stubFor = [&](const char* key) -> std::shared_ptr<Image> {
        const std::string path = j.value(key, "");
        if (path.empty()) return nullptr;
        // "@name" refers to a generated texture from the scene's "textures"
        // block; anything else is a file path resolved from disk later.
        if (path.front() == '@') {
            const std::string ref = path.substr(1);
            const auto found = generated.find(ref);
            if (found != generated.end()) return found->second;
            fmt::print(stderr,
                       "parseSceneBlueprint: material '{}' references unknown texture '@{}'\n",
                       material->name, ref);
            return nullptr;
        }
        for (const auto& img : out.images)// share one stub per unique path
            if (img && img->uri == path && img->byteArray.empty()) return img;
        auto stub = std::make_shared<Image>(Image{ .uri = path });
        out.images.push_back(stub);
        return stub;
    };
    material->albedoMap = stubFor("albedoMap");
    material->normalMap = stubFor("normalMap");
    material->metallicMap = stubFor("metallicMap");
    material->roughnessMap = stubFor("roughnessMap");
    material->occlusionMap = stubFor("occlusionMap");
    material->emissiveMap = stubFor("emissiveMap");
    out.materials.push_back(std::move(material));
}

SceneBlueprint parseSceneBlueprint(const std::string& jsonText, const std::string& nameHint) {
    SceneBlueprint bp;
    json root = json::parse(jsonText, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        fmt::print(stderr, "parseSceneBlueprint: malformed JSON ({})\n", nameHint);
        return bp;
    }
    bp.name = root.value("name", nameHint);
    // Procedural textures first — materials reference them by "@name", and a
    // generated image is baked here (pixels and all) so the disk-loading pass
    // in loadSceneBlueprint skips right over it.
    std::unordered_map<std::string, std::shared_ptr<Image>> generatedTextures;
    if (root.contains("textures")) {
        const auto& arr = root.at("textures");
        if (!arr.is_array()) {
            fmt::print(stderr, "parseSceneBlueprint: \"textures\" must be an array ({})\n", bp.name);
        } else {
            for (const auto& t : arr) {
                if (!t.is_object()) {
                    fmt::print(stderr,
                               "parseSceneBlueprint: \"textures\" entries must be objects ({})\n",
                               bp.name);
                    continue;
                }
                const std::string name = t.value("name", "");
                const std::string gen = t.value("generator", "");
                if (name.empty() || gen.empty()) {
                    fmt::print(stderr,
                               "parseSceneBlueprint: texture entry needs both \"name\" and "
                               "\"generator\" ({})\n", bp.name);
                    continue;
                }
                if (generatedTextures.count(name)) {
                    fmt::print(stderr, "parseSceneBlueprint: duplicate texture name '{}' ({})\n",
                               name, bp.name);
                    continue;
                }
                const json params = t.contains("params") ? t.at("params") : json::object();
                auto img = TextureGenerators::instance().generate(gen, params);
                if (!img) {
                    // Distinguish "no such generator" from "the generator ran
                    // and produced nothing" — generate() already reported the
                    // latter, and conflating them sends the author hunting for
                    // a registration bug that isn't there.
                    if (!TextureGenerators::instance().has(gen)) {
                        std::string known;
                        for (const auto& n : TextureGenerators::instance().names())
                            known += (known.empty() ? "" : ", ") + n;
                        fmt::print(stderr,
                                   "parseSceneBlueprint: unknown texture generator '{}' for '{}' "
                                   "({}); registered: {}\n",
                                   gen, name, bp.name, known);
                    }
                    continue;
                }
                // The uri is the pixel hash, so an entry that bakes to an
                // image we already hold is the same texture under another
                // name: share the object rather than duplicating its payload
                // through the scene list and the cook.
                for (const auto& existing : bp.images) {
                    if (existing && existing->uri == img->uri) {
                        img = existing;
                        break;
                    }
                }
                if (std::find(bp.images.begin(), bp.images.end(), img) == bp.images.end())
                    bp.images.push_back(img);
                generatedTextures.emplace(name, std::move(img));
            }
        }
    }
    if (root.contains("materials")) {
        for (const auto& m : root.at("materials"))
            parseMaterial(m, bp, generatedTextures);
    }
    if (root.contains("entities")) {
        for (const auto& e : root.at("entities"))
            parseEntityRec(e, -1, bp);
    }
    bp.ok = true;
    return bp;
}

// ── Splice ───────────────────────────────────────────────────────────────────

void appendBlueprint(SceneBlueprint& dst, SceneBlueprint&& sub, int parentIndex) {
    const int entityBase = static_cast<int>(dst.entities.size());
    const int meshBase = static_cast<int>(dst.meshes.size());
    const int lightBase = static_cast<int>(dst.lights.size());
    // sub.materials is concatenated onto dst.materials below, so primitive
    // material indices need rebasing exactly like meshes and lights — without
    // it a spliced prefab's primitives silently adopt the host's materials.
    const int materialBase = static_cast<int>(dst.materials.size());

    for (auto& e : sub.entities) {
        e.parent = e.parent < 0 ? parentIndex : e.parent + entityBase;
        for (int& m : e.meshes)
            m += meshBase;
        for (int& l : e.lights)
            l += lightBase;
        if (e.primitive.material >= 0) e.primitive.material += materialBase;
        for (auto& bm : e.procMesh.bucketMaterials)
            if (bm.second >= 0) bm.second += materialBase;
        dst.entities.push_back(std::move(e));
    }
    std::move(sub.meshes.begin(), sub.meshes.end(), std::back_inserter(dst.meshes));
    std::move(sub.materials.begin(), sub.materials.end(), std::back_inserter(dst.materials));
    std::move(sub.images.begin(), sub.images.end(), std::back_inserter(dst.images));
    std::move(sub.lights.begin(), sub.lights.end(), std::back_inserter(dst.lights));
    std::move(sub.sources.begin(), sub.sources.end(), std::back_inserter(dst.sources));
}

// ── Scene cook (.vscene) ─────────────────────────────────────────────────────
// The cooked artifact is the fully-expanded blueprint (references resolved,
// payload decoded) serialized next to the scene JSON, guarded by a hash over
// every input that shaped it: the JSON text plus the bytes of every expanded
// source/prefab file. Unlike an existence-only cache, editing the JSON or any
// referenced model invalidates the cook automatically.

namespace {

    constexpr char kCookMagic[4] = { 'V', 'B', 'P', '1' };
    constexpr uint32_t kCookVersion = 2;  // v2: meshlet bake config (regularize + sloppy factor)

    uint64_t fnv1a64(const void* data, size_t n, uint64_t h) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // Hash = JSON text ++ (path, bytes) of every expanded source, chained. The
    // path itself is folded in so renames invalidate even with identical bytes.
    uint64_t computeSourceHash(const std::string& jsonText, const std::vector<std::string>& sources) {
        uint64_t h = fnv1a64(jsonText.data(), jsonText.size(), 14695981039346656037ull);
        h = fnv1a64(&kCookVersion, sizeof(kCookVersion), h);
        // Generated textures are baked into the cook as pixels and never
        // re-run on a cache hit, so the registry has to participate in the
        // key: without this, editing a generator (or bumping its version)
        // would leave every already-cooked scene serving the old image. The
        // params themselves need no special handling — they live in jsonText.
        for (const auto& name : TextureGenerators::instance().names()) {
            h = fnv1a64(name.data(), name.size(), h);
            const int v = TextureGenerators::instance().version(name);
            h = fnv1a64(&v, sizeof(v), h);
        }
        for (const auto& src : sources) {
            h = fnv1a64(src.data(), src.size(), h);
            auto resolved = FileSystem::instance().resolvePath(src);
            if (!resolved) continue;// missing file still perturbs the hash via the path
            std::ifstream f(*resolved, std::ios::binary);
            char buf[64 * 1024];
            while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
                h = fnv1a64(buf, static_cast<size_t>(f.gcount()), h);
        }
        return h;
    }

    std::string cookPathFor(const std::string& resolvedJsonPath) {
        std::filesystem::path p(resolvedJsonPath);
        p.replace_extension(".vscene");
        return p.string();
    }

    SceneBlueprint tryLoadCook(const std::string& cookPath, const std::string& jsonText) {
        std::ifstream in(cookPath, std::ios::binary);
        if (!in.is_open()) return {};
        char magic[4] = {};
        uint32_t version = 0;
        uint64_t storedHash = 0;
        in.read(magic, sizeof(magic));
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        in.read(reinterpret_cast<char*>(&storedHash), sizeof(storedHash));
        if (!in || std::memcmp(magic, kCookMagic, sizeof(magic)) != 0 || version != kCookVersion) return {};

        SceneBlueprint cooked;
        try {
            cereal::BinaryInputArchive archive(in);
            cooked = AssetSerializer::deserializeBlueprint(archive);
        } catch (const std::exception& e) {
            fmt::print(stderr, "scene cook '{}' unreadable ({}); re-cooking\n", cookPath, e.what());
            return {};
        }
        if (!cooked.ok) return {};
        // Freshness: the cooked blueprint carries the source list it was built
        // from; recompute the hash over the CURRENT files and compare.
        if (computeSourceHash(jsonText, cooked.sources) != storedHash) return {};
        return cooked;
    }

    void writeCook(const std::string& cookPath, const SceneBlueprint& bp, uint64_t hash) {
        std::ofstream out(cookPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            fmt::print(stderr, "scene cook: cannot write '{}'\n", cookPath);
            return;
        }
        out.write(kCookMagic, sizeof(kCookMagic));
        out.write(reinterpret_cast<const char*>(&kCookVersion), sizeof(kCookVersion));
        out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        try {
            cereal::BinaryOutputArchive archive(out);
            AssetSerializer::serializeBlueprint(archive, bp);
        } catch (const std::exception& e) {
            fmt::print(stderr, "scene cook: serialize failed for '{}' ({})\n", cookPath, e.what());
        }
    }

}// namespace

// ── Load + expand ────────────────────────────────────────────────────────────

SceneBlueprint loadSceneBlueprint(const std::string& path) {
    auto resolved = FileSystem::instance().resolvePath(path);
    if (!resolved) {
        fmt::print(stderr, "loadSceneBlueprint: '{}' not found in any search path\n", path);
        return {};
    }
    std::ifstream file(*resolved, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    // Cook fast path: hash-guarded, so a stale artifact can never be replayed.
    const std::string cookPath = cookPathFor(*resolved);
    if (SceneBlueprint cooked = tryLoadCook(cookPath, text); cooked.ok) {
        fmt::print("loadSceneBlueprint '{}': cook hit ({} entities)\n", path, cooked.entities.size());
        return cooked;
    }

    SceneBlueprint bp = parseSceneBlueprint(text, path);
    if (!bp.ok) return bp;

    // Expand source/prefab references. Iterate by index over the original
    // entity count only: appendBlueprint grows the array at the end, and
    // freshly spliced sub-blueprints arrive already expanded.
    const size_t originalCount = bp.entities.size();
    for (size_t i = 0; i < originalCount; ++i) {
        // Copies, not references: appendBlueprint may reallocate bp.entities.
        const std::string source = bp.entities[i].source;
        const std::string prefab = bp.entities[i].prefab;
        if (!source.empty()) {
            SceneBlueprint model = AssetManager::loadModel(source);
            if (model.ok) {
                bp.sources.push_back(source);
                appendBlueprint(bp, std::move(model), static_cast<int>(i));
            } else {
                fmt::print(stderr, "loadSceneBlueprint: '{}' failed to import source '{}'\n", path, source);
            }
        }
        if (!prefab.empty()) {
            SceneBlueprint nested = loadSceneBlueprint(prefab);
            if (nested.ok) {
                bp.sources.push_back(prefab);
                appendBlueprint(bp, std::move(nested), static_cast<int>(i));
            } else {
                fmt::print(stderr, "loadSceneBlueprint: '{}' failed to load prefab '{}'\n", path, prefab);
            }
        }
    }

    // Load the declared materials' textures into their uri-stub Images. The
    // paths join bp.sources so the cook hash covers texture edits too. A path
    // that fails to load logs and clears the material reference (renderer
    // falls back to defaults) rather than shipping an empty image.
    for (auto& img : bp.images) {
        if (!img || !img->byteArray.empty() || img->uri.empty()) continue;
        bp.sources.push_back(img->uri);
        if (auto loaded = AssetManager::loadImage(img->uri)) {
            *img = *loaded;
            continue;
        }
        for (auto& mat : bp.materials) {
            if (!mat) continue;
            for (auto* slot : { &mat->albedoMap, &mat->normalMap, &mat->metallicMap, &mat->roughnessMap,
                                &mat->occlusionMap, &mat->emissiveMap })
                if (*slot == img) slot->reset();
        }
    }

    // Bake meshlets + cluster-LOD per mesh (offline) so the mesh-shader path gets
    // them straight from the cook — otherwise Renderer::registerMesh rebuilds them
    // on every load. No-op for empty / non-triangle meshes; the result rides the
    // .vscene via the shared (de)serializeMesh meshletData fields.
    for (auto& m : bp.meshes) {
        if (m) MeshletBuilder::build(*m);
    }

    // Write the cook so the next load skips parsing and model decode entirely.
    writeCook(cookPath, bp, computeSourceHash(text, bp.sources));
    return bp;
}

// ── Transform decompose ──────────────────────────────────────────────────────

void decomposeTransform(const glm::mat4& m, glm::vec3& position, glm::quat& rotation, glm::vec3& scale) {
    position = glm::vec3(m[3]);
    scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    // A negative determinant means one axis is mirrored; fold the flip into X
    // so the rotation part stays proper (quat_cast requires det == +1).
    if (glm::determinant(glm::mat3(m)) < 0.0f) scale.x = -scale.x;
    glm::mat3 rot(1.0f);
    if (scale.x != 0.0f) rot[0] = glm::vec3(m[0]) / scale.x;
    if (scale.y != 0.0f) rot[1] = glm::vec3(m[1]) / scale.y;
    if (scale.z != 0.0f) rot[2] = glm::vec3(m[2]) / scale.z;
    rotation = glm::quat_cast(rot);
}

// ── Instantiate ──────────────────────────────────────────────────────────────

entt::entity instantiate(
    entt::registry& registry,
    RenderScene& scene,
    const SceneBlueprint& blueprint,
    entt::entity parent,
    const std::string& name,
    std::vector<entt::entity>* outEntities
) {
    if (!blueprint.ok) return entt::null;

    // Stage every referenced mesh into the scene's world pool exactly once.
    // Renderer::stage() registers geometry from mesh->vertices and the material
    // straight off mesh->material, so staging is just membership here; the
    // baked-transform slot is unused on the ECS path (drawable transforms come
    // from the entities).
    std::unordered_set<const Mesh*> staged;
    staged.reserve(scene.stagedMeshes.size());
    for (const auto& m : scene.stagedMeshes)
        staged.insert(m.get());
    for (const auto& e : blueprint.entities) {
        for (int mi : e.meshes) {
            const auto& mesh = blueprint.meshes[static_cast<size_t>(mi)];
            if (!mesh || mesh->renderMeshId != UINT32_MAX || staged.count(mesh.get())) continue;
            scene.stagedMeshes.push_back(mesh);
            scene.stagedMeshTransforms.push_back(glm::mat4(1.0f));
            staged.insert(mesh.get());
        }
    }
    // Mirror the payload into the scene lists so the ImGui Scene Materials /
    // textures editors see it (rendering itself doesn't read these).
    {
        std::unordered_set<const Material*> knownMats;
        for (const auto& m : scene.materials)
            knownMats.insert(m.get());
        for (const auto& m : blueprint.materials)
            if (m && !knownMats.count(m.get())) scene.materials.push_back(m);
        std::unordered_set<const Image*> knownImages;
        for (const auto& img : scene.images)
            knownImages.insert(img.get());
        for (const auto& img : blueprint.images)
            if (img && !knownImages.count(img.get())) scene.images.push_back(img);
    }

    // Root entity: the blueprint's mount point under `parent`.
    const entt::entity root = registry.create();
    registry.emplace<NameComponent>(root, NameComponent{ name.empty() ? blueprint.name : name });
    auto& rootTc = registry.emplace<TransformComponent>(root);
    rootTc.parent = parent;
    rootTc.isDirty = true;
    if (outEntities) outEntities->push_back(root);

    // Entities. Parents precede children in the array, so the parent's entt
    // entity (and accumulated world rotation, needed for directional lights)
    // is always available by the time a child is created.
    std::vector<entt::entity> created(blueprint.entities.size());
    std::vector<glm::quat> worldRot(blueprint.entities.size());
    for (size_t i = 0; i < blueprint.entities.size(); ++i) {
        const EntityBlueprint& e = blueprint.entities[i];
        const entt::entity ent = registry.create();
        created[i] = ent;
        const glm::quat parentRot = e.parent < 0 ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                                 : worldRot[static_cast<size_t>(e.parent)];
        worldRot[i] = parentRot * e.rotation;

        registry.emplace<NameComponent>(
            ent, NameComponent{ e.name.empty() ? fmt::format("{}_{}", blueprint.name, i) : e.name }
        );
        auto& tc = registry.emplace<TransformComponent>(ent);
        tc.position = e.position;
        tc.rotation = e.rotation;
        tc.scale = e.scale;
        tc.parent = e.parent < 0 ? root : created[static_cast<size_t>(e.parent)];
        tc.isDirty = true;

        if (!e.meshes.empty()) {
            auto& mrc = registry.emplace<MeshRendererComponent>(ent);
            mrc.meshes.reserve(e.meshes.size());
            for (int mi : e.meshes)
                mrc.meshes.push_back(blueprint.meshes[static_cast<size_t>(mi)]);
        }

        // Procedural primitive: built fresh per entity (each gets its own mesh
        // so per-entity materials stay independent) and staged like any other.
        if (e.primitive.shape != PrimitiveBlueprint::Shape::None) {
            std::shared_ptr<Material> mat =
                e.primitive.material >= 0 ? blueprint.materials[static_cast<size_t>(e.primitive.material)] : nullptr;
            std::shared_ptr<Mesh> mesh;
            switch (e.primitive.shape) {
            case PrimitiveBlueprint::Shape::Cube: mesh = MeshBuilder::buildCube(e.primitive.size, mat); break;
            case PrimitiveBlueprint::Shape::Capsule:
                mesh = MeshBuilder::buildCapsule(e.primitive.height, e.primitive.size, 16, 8, mat);
                break;
            case PrimitiveBlueprint::Shape::Cone: mesh = MeshBuilder::buildCone(e.primitive.size, mat); break;
            case PrimitiveBlueprint::Shape::Triforce: mesh = MeshBuilder::buildTriforce(mat); break;
            case PrimitiveBlueprint::Shape::None: break;
            }
            if (mesh) {
                scene.stagedMeshes.push_back(mesh);
                scene.stagedMeshTransforms.push_back(glm::mat4(1.0f));
                registry.get_or_emplace<MeshRendererComponent>(ent).meshes.push_back(std::move(mesh));
            }
        }

        // Procedural mesh: run the registered generator and hand each bucket
        // to the renderer as its own mesh, so one entity can carry several
        // materials (tiles + grout) without inventing child entities.
        if (!e.procMesh.empty()) {
            json params = json::object();
            if (!e.procMesh.paramsJson.empty()) {
                params = json::parse(e.procMesh.paramsJson, nullptr, /*allow_exceptions=*/false);
                if (params.is_discarded()) params = json::object();
            }
            if (!MeshGenerators::instance().has(e.procMesh.generator)) {
                std::string known;
                for (const auto& n : MeshGenerators::instance().names())
                    known += (known.empty() ? "" : ", ") + n;
                fmt::print(stderr, "instantiate: unknown mesh generator '{}'; registered: {}\n",
                           e.procMesh.generator, known);
            } else {
                GeneratedContent content =
                    MeshGenerators::instance().generate(e.procMesh.generator, params);
                for (auto& [bucketName, data] : content.buckets) {
                    std::shared_ptr<Material> mat;
                    for (const auto& [name, index] : e.procMesh.bucketMaterials) {
                        if (name != bucketName) continue;
                        if (index >= 0 && size_t(index) < blueprint.materials.size())
                            mat = blueprint.materials[static_cast<size_t>(index)];
                        break;
                    }
                    std::vector<VertexData> verts(data.positions.size());
                    for (size_t vi = 0; vi < data.positions.size(); ++vi) {
                        verts[vi].position = data.positions[vi];
                        verts[vi].uv = data.uvs[vi];
                        verts[vi].normal = data.normals[vi];
                        // MikkTSpace overwrites this during initialize().
                        verts[vi].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                    }
                    auto procMesh = std::make_shared<Mesh>();
                    procMesh->hasPosition = true;
                    procMesh->hasUV0 = true;
                    procMesh->hasNormal = true;
                    procMesh->hasTangent = true;
                    procMesh->primitiveMode = PrimitiveMode::TRIANGLES;
                    procMesh->initialize(verts, data.indices);
                    procMesh->material = std::move(mat);
                    scene.stagedMeshes.push_back(procMesh);
                    scene.stagedMeshTransforms.push_back(glm::mat4(1.0f));
                    registry.get_or_emplace<MeshRendererComponent>(ent).meshes.push_back(
                        std::move(procMesh));
                }
            }
        }

        for (int li : e.lights) {
            const LightBlueprint& light = blueprint.lights[static_cast<size_t>(li)];
            switch (light.type) {
            case LightBlueprint::Type::Point:
                registry.emplace<PointLightComponent>(
                    ent,
                    PointLightComponent{
                        .color = light.color,
                        .intensity = light.intensity,
                        // 0 means "unbounded" in glTF; the renderer wants a
                        // finite falloff, so give unbounded lights a generous one.
                        .radius = light.range > 0.0f ? light.range : 10.0f,
                    }
                );
                break;
            case LightBlueprint::Type::Directional:
                // KHR_lights_punctual: a directional light shines along the
                // node's world -Z.
                registry.emplace<DirectionalLightComponent>(
                    ent,
                    DirectionalLightComponent{
                        .direction = glm::normalize(worldRot[i] * glm::vec3(0.0f, 0.0f, -1.0f)),
                        .color = light.color,
                        .intensity = light.intensity,
                    }
                );
                break;
            case LightBlueprint::Type::Spot:
                // SpotLightComponent aims along the entity transform's forward
                // (rotation * -Z) — already carried by the TransformComponent.
                registry.emplace<SpotLightComponent>(
                    ent,
                    SpotLightComponent{
                        .color = light.color,
                        .intensity = light.intensity,
                        .radius = light.range > 0.0f ? light.range : 12.0f,
                        .innerAngle = glm::degrees(light.innerConeAngle),
                        .outerAngle = glm::degrees(light.outerConeAngle),
                    }
                );
                break;
            }
        }

        if (outEntities) outEntities->push_back(ent);
    }

    // Generic components, applied in a second pass now that every entity of
    // this batch exists: entt::entity fields authored as name strings resolve
    // through the scope regardless of declaration order.
    {
        std::unordered_map<std::string, entt::entity> nameScope;
        nameScope.reserve(blueprint.entities.size());
        for (size_t i = 0; i < blueprint.entities.size(); ++i)
            if (!blueprint.entities[i].name.empty()) nameScope.emplace(blueprint.entities[i].name, created[i]);
        const detail::EntityNameScopeGuard scopeGuard(&nameScope);

        for (size_t i = 0; i < blueprint.entities.size(); ++i) {
            const EntityBlueprint& e = blueprint.entities[i];
            if (e.componentsJson.empty()) continue;
            const json components = json::parse(e.componentsJson, /*cb=*/nullptr, /*allow_exceptions=*/false);
            if (!components.is_object()) continue;
            for (const auto& [key, value] : components.items()) {
                if (!BlueprintComponents::instance().apply(key, registry, created[i], value))
                    fmt::print(stderr, "instantiate: no applier registered for component '{}' (entity '{}')\n",
                               key, e.name);
            }
        }
    }

    return root;
}

}// namespace Vapor
