#include "scene_blueprint.hpp"

#include "asset_manager.hpp"
#include "asset_serializer.hpp"
#include "components.hpp"
#include "file_system.hpp"
#include "fsm.hpp"
#include "mesh_builder.hpp"
#include "meshlet_builder.hpp"
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
        // TerrainSystem creates on first sight — exactly the shape the PFR
        // applier authors.
        r.registerComponent<StreamingTerrainComponent>("streamingTerrain");
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

// One declared material. Texture fields hold the asset path; parse creates a
// uri-only Image stub per unique path (pure — no I/O), and loadSceneBlueprint
// fills in the pixels afterwards.
static void parseMaterial(const json& j, SceneBlueprint& out) {
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
    if (root.contains("materials")) {
        for (const auto& m : root.at("materials"))
            parseMaterial(m, bp);
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

    for (auto& e : sub.entities) {
        e.parent = e.parent < 0 ? parentIndex : e.parent + entityBase;
        for (int& m : e.meshes)
            m += meshBase;
        for (int& l : e.lights)
            l += lightBase;
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
