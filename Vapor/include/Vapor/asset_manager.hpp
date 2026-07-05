#pragma once
#include <memory>
#include <string>

#include "graphics.hpp"
#include "scene.hpp"

class AssetManager {
public:
    static std::shared_ptr<Vapor::Image> loadImage(const std::string& filename);
    static std::shared_ptr<Vapor::HDRImage> loadHDRI(const std::string& filename);
    static std::shared_ptr<Vapor::Mesh> loadOBJ(const std::string& filename, const std::string& mtl_basedir = "");
    // Imports a GLTF into a flat Scene: meshes with baked world transforms in
    // the shared geometry pool (meshopt-processed, cached as .bin next to the
    // source). The GLTF node hierarchy is flattened at import; entity-level
    // hierarchy belongs to the ECS, not the asset container.
    static std::shared_ptr<Scene> loadGLTF(const std::string& filename);

private:
};