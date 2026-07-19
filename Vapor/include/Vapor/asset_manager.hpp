#pragma once
#include <memory>
#include <string>

#include "graphics.hpp"
#include "scene_blueprint.hpp"

namespace Vapor {

class AssetManager {
public:
    static std::shared_ptr<Vapor::Image> loadImage(const std::string& filename);
    static std::shared_ptr<Vapor::HDRImage> loadHDRI(const std::string& filename);
    static std::shared_ptr<Vapor::Mesh> loadOBJ(const std::string& filename, const std::string& mtl_basedir = "");

    // Model importers decode into a SceneBlueprint: the node hierarchy becomes
    // flat parent-indexed EntityBlueprints (local TRS preserved), and the
    // meshes/materials/images/lights land in the blueprint payload. Nothing is
    // baked or flattened here — geometry enters the RenderScene world pool at
    // instantiate() time, once per shared mesh. Returns ok == false on failure.
    static Vapor::SceneBlueprint loadGLTF(const std::string& filename);

    // USD/USDA/USDC/USDZ importer via TinyUSDZ (VAPOR_USE_TINYUSDZ; a warning
    // stub otherwise). Composes references/payloads/variants, extracts
    // UsdPreviewSurface materials (or synthesizes them from displayColor), and
    // applies stage upAxis/metersPerUnit on the blueprint's top-level entity.
    static Vapor::SceneBlueprint loadUSD(const std::string& filename);

    // Extension dispatch: .gltf/.glb -> loadGLTF, .usd/.usda/.usdc/.usdz -> loadUSD.
    static Vapor::SceneBlueprint loadModel(const std::string& filename);

private:
};

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
