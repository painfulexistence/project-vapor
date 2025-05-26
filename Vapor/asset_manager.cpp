#include "asset_manager.hpp"
#include "graphics.hpp"

#include "fmt/core.h"
#include "SDL3/SDL_filesystem.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tinyobjloader/tiny_obj_loader.h"


std::shared_ptr<Image> AssetManager::loadImage(const std::string& filename) {
    int width, height, numChannels;
    if (!stbi_info((SDL_GetBasePath() + filename).c_str(), &width, &height, &numChannels)) {
        throw std::runtime_error(fmt::format("Failed to load image at {}!\n", filename));
    }
    int desiredChannels = 0;
    switch (numChannels) {
    case 1:
        desiredChannels = 1;
        break;
    case 3:
        desiredChannels = 4;
        break;
    case 4:
        desiredChannels = 4;
        break;
    default:
        throw std::runtime_error(fmt::format("Unknown texture format at {}\n", filename));
        break;
    }
    uint8_t* data = stbi_load((SDL_GetBasePath() + filename).c_str(), &width, &height, &numChannels, desiredChannels);
    if (data) {
        auto image = std::make_shared<Image>(width, height, desiredChannels, data);
        stbi_image_free(data);
        return image;
    } else {
        stbi_image_free(data);
        return nullptr;
    }
}

std::shared_ptr<Mesh> AssetManager::loadOBJ(const std::string& filename) {
    std::vector<VertexData> vertices;
    std::vector<uint16_t> indices;

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, (SDL_GetBasePath() + filename).c_str())) {
        throw std::runtime_error(fmt::format("Failed to load model: {}", err));
    }

    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            VertexData vert = {};
            vert.position = { attrib.vertices[3 * index.vertex_index + 0],
                              attrib.vertices[3 * index.vertex_index + 1],
                              attrib.vertices[3 * index.vertex_index + 2] };
            vert.normal = { attrib.normals[3 * index.normal_index + 0],
                            attrib.normals[3 * index.normal_index + 1],
                            attrib.normals[3 * index.normal_index + 2] };
            vert.uv = { attrib.texcoords[2 * index.texcoord_index + 0],
                        1.0 - attrib.texcoords[2 * index.texcoord_index + 1] };
            vertices.push_back(vert);
            indices.push_back(indices.size());
        }
    }

    auto mesh = std::make_shared<Mesh>();
    mesh->initialize({ vertices, indices });

    return mesh;
}