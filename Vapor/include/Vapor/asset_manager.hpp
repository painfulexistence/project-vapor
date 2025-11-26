#pragma once
#include <memory>
#include <string>

#include "graphics.hpp"
#include "scene.hpp"

class AssetManager {
public:
    static std::shared_ptr<Image> loadImage(const std::string& filename);
    static std::shared_ptr<Mesh> loadOBJ(const std::string& filename, const std::string& mtl_basedir = "");
    static std::shared_ptr<Scene> loadGLTF(const std::string& filename);
    static std::shared_ptr<Scene> loadGLTFOptimized(const std::string& filename);
private:

};