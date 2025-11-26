#pragma once
#include <string>
#include <memory>
#include <cereal/cereal.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/array.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>
#include "graphics.hpp"
#include "scene.hpp"


namespace cereal {
    template<class Archive>
    void serialize(Archive& archive, glm::vec2& vec) {
        archive(vec.x, vec.y);
    }

    template<class Archive>
    void serialize(Archive& archive, glm::vec3& vec) {
        archive(vec.x, vec.y, vec.z);
    }

    template<class Archive>
    void serialize(Archive& archive, glm::vec4& vec) {
        archive(vec.x, vec.y, vec.z, vec.w);
    }

    template<class Archive>
    void serialize(Archive& archive, glm::mat4& mat) {
        archive(mat[0], mat[1], mat[2], mat[3]);
    }

    template<class Archive>
    void serialize(Archive& archive, glm::quat& quat) {
        archive(quat.x, quat.y, quat.z, quat.w);
    }

    template<class Archive>
    void serialize(Archive& archive, VertexData& vertex) {
        archive(vertex.position, vertex.uv, vertex.normal, vertex.tangent);
    }
}

class AssetSerializer {
public:
    static void serializeScene(const std::shared_ptr<Scene>& scene, const std::string& path);
    static std::shared_ptr<Scene> deserializeScene(const std::string& path);

private:
    static void serializeMaterial(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Material>& material,
                                  const std::unordered_map<std::shared_ptr<Image>, Uint32>& imageIDs);
    static std::shared_ptr<Material> deserializeMaterial(cereal::BinaryInputArchive& archive,
                                                         const std::unordered_map<Uint32, std::shared_ptr<Image>>& images);

    static void serializeImage(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Image>& image);
    static std::shared_ptr<Image> deserializeImage(cereal::BinaryInputArchive& archive);

    static void serializeNode(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Node>& node,
                              const std::unordered_map<std::shared_ptr<Material>, Uint32>& materialIDs);
    static std::shared_ptr<Node> deserializeNode(cereal::BinaryInputArchive& archive,
                                                 const std::unordered_map<Uint32, std::shared_ptr<Material>>& materials);

    static void serializeMesh(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Mesh>& mesh,
                              const std::unordered_map<std::shared_ptr<Material>, Uint32>& materialIDs);
    static std::shared_ptr<Mesh> deserializeMesh(cereal::BinaryInputArchive& archive,
                                                 const std::unordered_map<Uint32, std::shared_ptr<Material>>& materials);

    static void serializeDirectionalLight(cereal::BinaryOutputArchive& archive, const DirectionalLight& light);
    static DirectionalLight deserializeDirectionalLight(cereal::BinaryInputArchive& archive);

    static void serializePointLight(cereal::BinaryOutputArchive& archive, const PointLight& light);
    static PointLight deserializePointLight(cereal::BinaryInputArchive& archive);
};