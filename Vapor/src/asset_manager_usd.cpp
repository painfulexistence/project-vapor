// USD (.usd/.usda/.usdc/.usdz) -> SceneBlueprint importer via TinyUSDZ + Tydra.
// Ported from Atmospheric's prefab_usd.cpp, adapted to vapor: native-only file
// I/O (no web/VFS resolver — tinyusdz's default asset resolution reads disk and
// honours the referencing layer's working path itself), 32-bit indices (no
// 65535-vertex chunking), and Vapor::Mesh/Material/Image as the payload types.
// Flattens references/payloads/variants, preserves the USD (Xform) hierarchy as
// flat parent-indexed EntityBlueprints, extracts UsdPreviewSurface materials
// with decoded textures (or synthesizes a fallback from the displayColor
// primvar), and applies stage upAxis / metersPerUnit on the top-level entity.
// Pure CPU; compiled to a warning stub without VAPOR_USE_TINYUSDZ.
#include "asset_manager.hpp"
#include "scene_blueprint.hpp"

#include <fmt/core.h>

#ifndef VAPOR_USE_TINYUSDZ

auto AssetManager::loadUSD(const std::string& filename) -> Vapor::SceneBlueprint {
    fmt::print("loadUSD '{}': USD support not compiled in (build with -DVAPOR_USE_TINYUSDZ=ON)\n", filename);
    return {};
}

#else

#include "file_system.hpp"

#include <tinyusdz.hh>

#include <algorithm>
#include <asset-resolution.hh>
#include <composition.hh>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <tydra/render-data.hh>

namespace {

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

// Resolve an asset path the way tinyusdz's LoadAsset does — against the
// PrimSpec's own asset-resolution state first (set as composition descends
// into sub-layers), then the root base dir.
std::optional<std::string>
    peekResolve(const std::string& assetPath, const tinyusdz::PrimSpec& ps, const std::string& baseDir) {
    std::vector<std::string> dirs;
    const std::string& cwp = ps.get_current_working_path();
    if (!cwp.empty()) dirs.push_back(cwp);
    for (const auto& s : ps.get_asset_search_paths())
        dirs.push_back(s);
    if (!baseDir.empty()) dirs.push_back(baseDir);
    dirs.push_back(".");
    for (const auto& d : dirs) {
        const std::filesystem::path cand = std::filesystem::path(d) / assetPath;
        if (std::filesystem::exists(cand)) return cand.string();
    }
    if (std::filesystem::exists(assetPath)) return assetPath;
    return std::nullopt;
}

// Read the effective root prim of a referenced/payloaded layer (defaultPrim,
// else the first prim) — the prim the arc implicitly targets. Cached by path.
bool peekDefaultPrim(const std::string& resolved, tinyusdz::Path* out, std::map<std::string, tinyusdz::Path>& cache) {
    auto it = cache.find(resolved);
    if (it != cache.end()) {
        *out = it->second;
        return it->second.is_valid();
    }
    tinyusdz::Path result;// invalid by default
    const std::vector<uint8_t> b = readFileBytes(resolved);
    if (!b.empty()) {
        tinyusdz::Layer layer;
        std::string w, e;
        if (tinyusdz::LoadLayerFromMemory(b.data(), b.size(), resolved, &layer, &w, &e) && !layer.primspecs().empty()) {
            const std::string name =
                layer.metas().defaultPrim.valid() ? layer.metas().defaultPrim.str() : layer.primspecs().begin()->first;
            result = tinyusdz::Path("/" + name, "");
        }
    }
    cache[resolved] = result;
    *out = result;
    return result.is_valid();
}

// Fill in the implicit prim_path (the referenced layer's defaultPrim) on every
// reference/payload arc that omits one. tinyusdz translates a referenced
// layer's internal target/connection paths across the arc via
// ReplaceRootPrimPathRec(srcPrefix = arc.prim_path, ...) — but when the arc
// relies on defaultPrim (the common `@file.usd@` form, no `</Prim>`),
// arc.prim_path is empty, so the translation no-ops and bindings such as
// `material:binding = </Prop/Looks/Mat>` dangle after flattening (the mesh
// survives, its material is dropped). Supplying the defaultPrim as the
// prefix — exactly what the arc means — makes the existing translation fire.
void fillArcPrimPaths(tinyusdz::PrimSpec& ps, const std::string& baseDir, std::map<std::string, tinyusdz::Path>& cache) {
    auto fix = [&](auto& arcsPair) {
        // The pinned compositor rejects `add`-qualified arcs outright
        // (Kitchen_set.usd uses `add references`); on a flatten of a single
        // layer stack `add` and `prepend` produce the same result.
        if (arcsPair.first == tinyusdz::ListEditQual::Add) arcsPair.first = tinyusdz::ListEditQual::Prepend;
        for (auto& arc : arcsPair.second) {
            if (arc.asset_path.GetAssetPath().empty()) continue;// internal (inherit-like) arc
            if (arc.prim_path.is_valid()) continue;// already explicit
            const auto resolved = peekResolve(arc.asset_path.GetAssetPath(), ps, baseDir);
            if (!resolved) continue;
            tinyusdz::Path dp;
            if (peekDefaultPrim(*resolved, &dp, cache)) arc.prim_path = dp;
        }
    };
    if (ps.metas().references) fix(ps.metas().references.value());
    if (ps.metas().payload) fix(ps.metas().payload.value());
    for (auto& c : ps.children())
        fillArcPrimPaths(c, baseDir, cache);
}

// ── Post-composition dangling-target repair ──────────────────────────────
// tinyusdz retargets a referenced layer's internal paths but does NOT descend
// into variantSet content, so a `material:binding` (and the material's shader
// `.connect`) authored inside a variant keeps pointing at the pre-flatten
// namespace after the variant is composed — the mesh loads but its material is
// dropped (Kitchen_set wraps every prop in a modelingVariant). Repair any
// still-dangling absolute target: strip its root component and rebind it to
// the matching subtree under the nearest existing ancestor of the holder.
void collectPrimPaths(const tinyusdz::PrimSpec& ps, const std::string& parent, std::set<std::string>& out) {
    const std::string path = parent + "/" + ps.name();
    out.insert(path);
    for (const auto& c : ps.children())
        collectPrimPaths(c, path, out);
}

void retargetDangling(tinyusdz::Path& target, const std::string& holder, const std::set<std::string>& prims) {
    if (!target.is_absolute_path()) return;
    const std::string t = target.prim_part();
    if (t.empty() || t == "/" || prims.count(t)) return;// unresolved only
    const size_t split = t.find('/', 1);
    if (split == std::string::npos) return;
    const std::string rootComp = t.substr(0, split);// e.g. "/Prop"
    const std::string suffix = t.substr(split);// e.g. "/Looks/Mat"
    std::string anc = holder;
    while (true) {
        if (prims.count(anc + suffix)) {
            target.replace_prefix(tinyusdz::Path(rootComp, ""), tinyusdz::Path(anc, ""));
            return;
        }
        const size_t s = anc.find_last_of('/');
        if (s == std::string::npos || s == 0) break;
        anc = anc.substr(0, s);
    }
}

void fixDanglingTargets(tinyusdz::PrimSpec& ps, const std::string& parent, const std::set<std::string>& prims) {
    const std::string self = parent + "/" + ps.name();
    for (auto& [name, prop] : ps.props()) {
        if (prop.is_relationship()) {
            auto& rel = prop.relationship();
            if (rel.is_path()) {
                retargetDangling(rel.targetPath, self, prims);
            } else if (rel.is_pathvector()) {
                for (auto& p : rel.targetPathVector)
                    retargetDangling(p, self, prims);
            }
        } else if (prop.is_attribute_connection()) {
            for (auto& c : prop.attribute().connections())
                retargetDangling(c, self, prims);
        }
    }
    for (auto& c : ps.children())
        fixDanglingTargets(c, self, prims);
}

// Load a USDA/USDC root as a Layer and flatten LIVRPS composition arcs
// (subLayers, references, payloads, inherits, variants) into a single Stage.
// tinyusdz does NOT compose by default — a plain load of a file built from
// references/payloads (e.g. Pixar's Kitchen_set) yields zero geometry.
bool composeStage(
    const std::vector<uint8_t>& bytes,
    const std::string& realPath,
    const std::string& baseDir,
    tinyusdz::Stage* outStage,
    std::string* warn,
    std::string* err
) {
    tinyusdz::Layer root;
    if (!tinyusdz::LoadLayerFromMemory(bytes.data(), bytes.size(), realPath, &root, warn, err)) return false;

    tinyusdz::AssetResolutionResolver resolver;// default: reads straight from disk
    const std::string cwd = baseDir.empty() ? "." : baseDir;
    resolver.set_search_paths({ cwd });
    resolver.set_current_working_path(cwd);

    tinyusdz::Layer src = root;
    {
        tinyusdz::Layer composited;
        if (tinyusdz::CompositeSublayers(resolver, src, &composited, warn, err)) src = std::move(composited);
    }
    // References/payloads can nest (a referenced layer references more); loop
    // until no arc remains unresolved (bounded to avoid a pathological cycle).
    constexpr int kMaxIterations = 128;
    std::map<std::string, tinyusdz::Path> defaultPrimCache;
    for (int i = 0; i < kMaxIterations; ++i) {
        bool unresolved = false;
        for (auto& kv : src.primspecs())
            fillArcPrimPaths(kv.second, baseDir, defaultPrimCache);
        if (src.check_unresolved_references()) {
            unresolved = true;
            tinyusdz::Layer composited;
            if (!tinyusdz::CompositeReferences(resolver, src, &composited, warn, err)) return false;
            src = std::move(composited);
        }
        if (src.check_unresolved_payload()) {
            unresolved = true;
            tinyusdz::Layer composited;
            if (!tinyusdz::CompositePayload(resolver, src, &composited, warn, err)) return false;
            src = std::move(composited);
        }
        if (src.check_unresolved_inherits()) {
            unresolved = true;
            tinyusdz::Layer composited;
            if (tinyusdz::CompositeInherits(src, &composited, warn, err)) src = std::move(composited);
        }
        // Defer variant composition until references/payloads are fully
        // settled: the variant *selection* often sits on an outer prim whose
        // variantSet blocks arrive from a deeper reference->payload chain.
        // Consuming the selection while the blocks are still empty drops the
        // late-arriving content (meshes import, materials vanish).
        if (unresolved) continue;
        if (src.check_unresolved_variant()) {
            unresolved = true;
            tinyusdz::Layer composited;
            if (tinyusdz::CompositeVariant(src, &composited, warn, err)) src = std::move(composited);
        }
        if (!unresolved) break;
    }

    // Rebind any binding/connection paths left dangling by variant composition.
    {
        std::set<std::string> prims;
        for (auto& [name, ps] : src.primspecs())
            collectPrimPaths(ps, "", prims);
        for (auto& [name, ps] : src.primspecs())
            fixDanglingTargets(ps, "", prims);
    }

    // Preserve stage metas (upAxis / metersPerUnit) — LayerToStage keeps what
    // we seed here, and the importer reads them below for the root transform.
    outStage->metas() = root.metas();
    return tinyusdz::LayerToStage(src, outStage, warn, err);
}

// tinyusdz matrices are row-major with USD's row-vector (pre-multiply)
// convention; copying rows into glm columns yields the equivalent
// column-vector matrix (row i = image of basis i = glm column i).
glm::mat4 toGlm(const tinyusdz::value::matrix4d& m) {
    glm::mat4 g(1.0f);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            g[i][j] = static_cast<float>(m.m[i][j]);
    return g;
}

// Extract a Tydra RenderMesh into a Vapor::Mesh, expanding to a non-indexed
// triangle list (one vertex per face-vertex — copes with both 'vertex' and
// 'facevarying' attribute variability). 32-bit indices, so no chunking.
std::shared_ptr<Vapor::Mesh> extractMesh(const tinyusdz::tydra::RenderMesh& rmesh) {
    const std::vector<uint32_t>& idx = rmesh.faceVertexIndices();// triangulated
    const size_t pointCount = rmesh.points.size();
    if (idx.empty() || pointCount == 0) return nullptr;

    auto normalAt = [&](size_t k, uint32_t pointIndex) -> glm::vec3 {
        const auto& a = rmesh.normals;
        const size_t vc = a.vertex_count();
        if (vc == 0 || a.format_size() < sizeof(float) * 3) return glm::vec3(0, 1, 0);
        const auto* f = reinterpret_cast<const float*>(a.buffer());
        const size_t at = (vc == pointCount) ? pointIndex : k;
        if (at >= vc) return glm::vec3(0, 1, 0);
        return glm::vec3(f[at * 3 + 0], f[at * 3 + 1], f[at * 3 + 2]);
    };
    auto uvAt = [&](size_t k, uint32_t pointIndex) -> glm::vec2 {
        auto it = rmesh.texcoords.find(0);
        if (it == rmesh.texcoords.end()) return glm::vec2(0);
        const auto& a = it->second;
        const size_t vc = a.vertex_count();
        if (vc == 0 || a.format_size() < sizeof(float) * 2) return glm::vec2(0);
        const auto* f = reinterpret_cast<const float*>(a.buffer());
        const size_t at = (vc == pointCount) ? pointIndex : k;
        if (at >= vc) return glm::vec2(0);
        return glm::vec2(f[at * 2 + 0], f[at * 2 + 1]);
    };

    auto mesh = std::make_shared<Vapor::Mesh>();
    mesh->vertices.resize(idx.size());
    for (size_t k = 0; k < idx.size(); ++k) {
        const uint32_t pointIndex = idx[k];
        Vapor::VertexData& v = mesh->vertices[k];
        if (pointIndex < pointCount) {
            // Tydra points are std::array<float,3>-like, not .x/.y/.z.
            const auto& p = rmesh.points[pointIndex];
            v.position = glm::vec3(p[0], p[1], p[2]);
        } else {
            v.position = glm::vec3(0.0f);
        }
        v.normal = normalAt(k, pointIndex);
        v.uv = uvAt(k, pointIndex);
        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    mesh->indices.resize(idx.size());
    for (size_t k = 0; k < idx.size(); ++k)
        mesh->indices[k] = static_cast<Uint32>(k);

    mesh->hasPosition = true;
    mesh->hasNormal = rmesh.normals.vertex_count() > 0;
    mesh->hasUV0 = rmesh.texcoords.count(0) > 0;
    mesh->vertexCount = static_cast<Uint32>(mesh->vertices.size());
    mesh->indexCount = static_cast<Uint32>(mesh->indices.size());
    mesh->isGeometryDirty = false;
    mesh->primitiveMode = Vapor::PrimitiveMode::TRIANGLES;
    mesh->calculateLocalAABB();
    return mesh;
}

}// namespace

auto AssetManager::loadUSD(const std::string& filename) -> Vapor::SceneBlueprint {
    Vapor::SceneBlueprint bp;
    auto resolvedOpt = FileSystem::instance().resolvePath(filename);
    if (!resolvedOpt) {
        fmt::print("loadUSD '{}': not found in any search path\n", filename);
        return bp;
    }
    const std::string realPath = *resolvedOpt;
    const std::vector<uint8_t> bytes = readFileBytes(realPath);
    if (bytes.empty()) {
        fmt::print("loadUSD '{}': read failed\n", realPath);
        return bp;
    }
    const std::string baseDir = std::filesystem::path(realPath).parent_path().string();

    // USDZ is a self-contained zip archive; its internal assets are resolved by
    // the USDZ loader itself, so the memory loader is the right path there.
    // Everything else goes through composition so files built from
    // references/payloads (e.g. Kitchen_set) actually pull in their geometry.
    const std::string ext = std::filesystem::path(realPath).extension().string();
    const bool isUsdz = ext == ".usdz";

    tinyusdz::Stage stage;
    std::string warn, err;
    const bool loaded = isUsdz ? tinyusdz::LoadUSDFromMemory(bytes.data(), bytes.size(), realPath, &stage, &warn, &err)
                               : composeStage(bytes, realPath, baseDir, &stage, &warn, &err);
    if (!loaded) {
        if (!warn.empty()) fmt::print("loadUSD '{}': {}\n", filename, warn);
        if (!err.empty()) fmt::print("loadUSD '{}' error: {}\n", filename, err);
        return bp;
    }
    if (!warn.empty()) fmt::print("loadUSD '{}': {}\n", filename, warn);

    tinyusdz::tydra::RenderSceneConverter converter;
    tinyusdz::tydra::RenderSceneConverterEnv env(stage);
    if (!baseDir.empty()) env.set_search_paths({ baseDir });
    env.scene_config.load_texture_assets = true;// decode textures for materials
    // Keep 8-bit texels 8-bit — Tydra otherwise converts textures to fp32.
    env.material_config.preserve_texel_bitdepth = true;

    tinyusdz::tydra::RenderScene rscene;
    if (!converter.ConvertToRenderScene(env, &rscene)) {
        fmt::print("loadUSD '{}' convert error: {}\n", filename, converter.GetError());
        return bp;
    }

    // ── Images (Tydra decodes into rscene.buffers via its builtin loader) ────
    // Map rscene image index -> bp.images index (-1 when undecodable). 3-channel
    // texels are expanded to RGBA, matching loadImage's channel policy.
    std::vector<int> imageRemap(rscene.images.size(), -1);
    for (size_t i = 0; i < rscene.images.size(); ++i) {
        const auto& ti = rscene.images[i];
        if (ti.buffer_id < 0 || ti.buffer_id >= static_cast<int64_t>(rscene.buffers.size())) continue;
        const auto& buf = rscene.buffers[ti.buffer_id];
        if (!ti.decoded || ti.width <= 0 || ti.height <= 0 || ti.channels <= 0) continue;
        const size_t texels = static_cast<size_t>(ti.width) * ti.height * ti.channels;

        std::vector<Uint8> pixels;
        if (buf.componentType == tinyusdz::tydra::ComponentType::UInt8 && buf.data.size() >= texels) {
            pixels.assign(buf.data.begin(), buf.data.begin() + texels);
        } else if (buf.componentType == tinyusdz::tydra::ComponentType::Float
                   && buf.data.size() >= texels * sizeof(float)) {
            // Fallback for Tydra's fp32 conversion path: quantize back to 8-bit.
            const float* f = reinterpret_cast<const float*>(buf.data.data());
            pixels.resize(texels);
            for (size_t t = 0; t < texels; ++t)
                pixels[t] = static_cast<Uint8>(std::clamp(f[t], 0.0f, 1.0f) * 255.0f + 0.5f);
        } else {
            continue;// unsupported texel layout
        }

        Uint32 channels = static_cast<Uint32>(ti.channels);
        if (channels == 3) {
            std::vector<Uint8> rgba(static_cast<size_t>(ti.width) * ti.height * 4);
            for (size_t t = 0; t < static_cast<size_t>(ti.width) * ti.height; ++t) {
                rgba[t * 4 + 0] = pixels[t * 3 + 0];
                rgba[t * 4 + 1] = pixels[t * 3 + 1];
                rgba[t * 4 + 2] = pixels[t * 3 + 2];
                rgba[t * 4 + 3] = 255;
            }
            pixels = std::move(rgba);
            channels = 4;
        }

        imageRemap[i] = static_cast<int>(bp.images.size());
        bp.images.push_back(std::make_shared<Vapor::Image>(Vapor::Image{
            .uri = ti.asset_identifier.empty() ? fmt::format("{}#image{}", filename, i) : ti.asset_identifier,
            .width = static_cast<Uint32>(ti.width),
            .height = static_cast<Uint32>(ti.height),
            .channelCount = channels,
            .byteArray = std::move(pixels),
        }));
    }

    // ── Materials (UsdPreviewSurface -> Vapor::Material) ─────────────────────
    auto imageOfTexture = [&](int64_t texId) -> std::shared_ptr<Vapor::Image> {
        if (texId < 0 || texId >= static_cast<int64_t>(rscene.textures.size())) return nullptr;
        const int64_t img = rscene.textures[texId].texture_image_id;
        if (img < 0 || img >= static_cast<int64_t>(imageRemap.size())) return nullptr;
        const int remapped = imageRemap[img];
        return remapped >= 0 ? bp.images[remapped] : nullptr;
    };
    for (const auto& rm : rscene.materials) {
        const auto& s = rm.surfaceShader;
        auto material = std::make_shared<Vapor::Material>();
        material->name = rm.name.empty() ? rm.abs_path : rm.name;
        material->baseColorFactor =
            glm::vec4(s.diffuseColor.value[0], s.diffuseColor.value[1], s.diffuseColor.value[2], 1.0f);
        material->metallicFactor = s.metallic.value;
        material->roughnessFactor = s.roughness.value;
        material->emissiveFactor = glm::vec3(s.emissiveColor.value[0], s.emissiveColor.value[1], s.emissiveColor.value[2]);
        if (s.diffuseColor.is_texture()) material->albedoMap = imageOfTexture(s.diffuseColor.texture_id);
        if (s.normal.is_texture()) material->normalMap = imageOfTexture(s.normal.texture_id);
        if (s.metallic.is_texture()) material->metallicMap = imageOfTexture(s.metallic.texture_id);
        if (s.roughness.is_texture()) material->roughnessMap = imageOfTexture(s.roughness.texture_id);
        if (s.occlusion.is_texture()) material->occlusionMap = imageOfTexture(s.occlusion.texture_id);
        if (s.emissiveColor.is_texture()) material->emissiveMap = imageOfTexture(s.emissiveColor.texture_id);
        bp.materials.push_back(std::move(material));
    }

    // ── Meshes ───────────────────────────────────────────────────────────────
    // Meshes with no bound material fall back to their `displayColor` primvar,
    // synthesized into a shared material per distinct color — how
    // pre-UsdPreviewSurface assets (e.g. Pixar's 2016 Kitchen_set) keep their
    // authored look (usdview colors them from displayColor too).
    std::map<std::tuple<int, int, int>, std::shared_ptr<Vapor::Material>> displayColorMats;
    auto displayColorMaterial = [&](const tinyusdz::tydra::RenderMesh& rmesh) -> std::shared_ptr<Vapor::Material> {
        const glm::vec3 c(rmesh.displayColor[0], rmesh.displayColor[1], rmesh.displayColor[2]);
        const auto key = std::make_tuple(
            static_cast<int>(c.r * 255.0f + 0.5f),
            static_cast<int>(c.g * 255.0f + 0.5f),
            static_cast<int>(c.b * 255.0f + 0.5f)
        );
        auto it = displayColorMats.find(key);
        if (it != displayColorMats.end()) return it->second;
        auto material = std::make_shared<Vapor::Material>();
        material->name =
            fmt::format("displayColor_{:02x}{:02x}{:02x}", std::get<0>(key), std::get<1>(key), std::get<2>(key));
        material->baseColorFactor = glm::vec4(c, 1.0f);
        material->roughnessFactor = 0.8f;// matte, close to usdview's default shading
        material->metallicFactor = 0.0f;
        bp.materials.push_back(material);
        displayColorMats.emplace(key, material);
        return material;
    };

    // rscene mesh index -> blueprint mesh index (-1 when extraction failed).
    std::vector<int> meshRemap(rscene.meshes.size(), -1);
    for (size_t i = 0; i < rscene.meshes.size(); ++i) {
        const auto& rmesh = rscene.meshes[i];
        auto mesh = extractMesh(rmesh);
        if (!mesh) continue;
        const bool bound = rmesh.material_id >= 0 && rmesh.material_id < static_cast<int>(rscene.materials.size());
        mesh->material = bound ? bp.materials[rmesh.material_id] : displayColorMaterial(rmesh);
        meshRemap[i] = static_cast<int>(bp.meshes.size());
        bp.meshes.push_back(std::move(mesh));
    }

    // ── Node hierarchy (mesh points are local; nodes carry the transforms) ───
    // Top-level entity applies the stage metadata: metersPerUnit scale and
    // upAxis rotation (Z-up / X-up -> Y-up). Read from the Stage itself —
    // Tydra's RenderScene.meta is not populated from the stage metas.
    bp.name = std::filesystem::path(realPath).stem().string();
    {
        glm::mat4 rootXf(1.0f);
        const float mpu = static_cast<float>(stage.metas().metersPerUnit.get_value());
        if (mpu > 0.0f && std::abs(mpu - 1.0f) > 1e-6f) rootXf = glm::scale(rootXf, glm::vec3(mpu));
        const tinyusdz::Axis up = stage.metas().upAxis.get_value();
        if (up == tinyusdz::Axis::Z)
            rootXf = glm::rotate(rootXf, glm::radians(-90.0f), glm::vec3(1, 0, 0));// Z-up -> Y-up
        else if (up == tinyusdz::Axis::X)
            rootXf = glm::rotate(rootXf, glm::radians(-90.0f), glm::vec3(0, 0, 1));// X-up -> Y-up
        Vapor::EntityBlueprint rootEntity;
        rootEntity.name = bp.name;
        Vapor::decomposeTransform(rootXf, rootEntity.position, rootEntity.rotation, rootEntity.scale);
        bp.entities.push_back(std::move(rootEntity));
    }

    std::function<void(const tinyusdz::tydra::Node&, int)> buildNode = [&](const tinyusdz::tydra::Node& n,
                                                                           int parentIndex) {
        Vapor::EntityBlueprint e;
        e.name = n.prim_name.empty() ? n.abs_path : n.prim_name;
        e.parent = parentIndex;
        Vapor::decomposeTransform(toGlm(n.local_matrix), e.position, e.rotation, e.scale);
        if (n.nodeType == tinyusdz::tydra::NodeType::Mesh && n.id >= 0
            && n.id < static_cast<int32_t>(meshRemap.size()) && meshRemap[n.id] >= 0)
            e.meshes.push_back(meshRemap[n.id]);
        const int selfIndex = static_cast<int>(bp.entities.size());
        bp.entities.push_back(std::move(e));
        for (const auto& c : n.children)
            buildNode(c, selfIndex);
    };

    if (rscene.default_root_node < rscene.nodes.size()) {
        buildNode(rscene.nodes[rscene.default_root_node], 0);
    } else {
        // No node hierarchy — attach every extracted mesh to the root entity.
        for (int m : meshRemap)
            if (m >= 0) bp.entities[0].meshes.push_back(m);
    }

    bp.ok = !bp.meshes.empty();
    fmt::print(
        "loadUSD '{}': {} entities, {} meshes, {} materials, {} images\n",
        filename,
        bp.entities.size(),
        bp.meshes.size(),
        bp.materials.size(),
        bp.images.size()
    );
    return bp;
}

#endif// VAPOR_USE_TINYUSDZ
