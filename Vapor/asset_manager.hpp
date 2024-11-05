#pragma once
#include <memory>
#include <string>

class Image;
class Mesh;

class AssetManager {
public:
    static std::shared_ptr<Image> loadImage(const std::string& filename);
    static std::shared_ptr<Mesh> loadOBJ(const std::string& filename);
private:

};