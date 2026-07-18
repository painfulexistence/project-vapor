#pragma once
#include "graphics.hpp"
#include "render_scene.hpp"
#include "scene_blueprint.hpp"
#include <cereal/archives/binary.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/array.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <string>


namespace cereal {
    template<class Archive> void serialize(Archive& archive, glm::vec2& vec) {
        archive(vec.x, vec.y);
    }

    template<class Archive> void serialize(Archive& archive, glm::vec3& vec) {
        archive(vec.x, vec.y, vec.z);
    }

    template<class Archive> void serialize(Archive& archive, glm::vec4& vec) {
        archive(vec.x, vec.y, vec.z, vec.w);
    }

    template<class Archive> void serialize(Archive& archive, glm::mat4& mat) {
        archive(mat[0], mat[1], mat[2], mat[3]);
    }

    template<class Archive> void serialize(Archive& archive, glm::quat& quat) {
        archive(quat.x, quat.y, quat.z, quat.w);
    }

    template<class Archive> void serialize(Archive& archive, Vapor::VertexData& vertex) {
        archive(vertex.position, vertex.uv, vertex.normal, vertex.tangent);
    }
}// namespace cereal

class AssetSerializer {
public:
    // v3: material names now serialize (the inspector's Scene Materials editor
    // and the blueprint cook both want identity, not just factors). The
    // meshlet/cluster-LOD data model exists on Mesh but is not serialized here —
    // the mesh-shader draw path and its offline bake will own the next bump.
    static constexpr uint32_t SCENE_FORMAT_VERSION = 3;

    static void serializeScene(const std::shared_ptr<RenderScene>& scene, const std::string& path);
    static std::shared_ptr<RenderScene> deserializeScene(const std::string& path);

    // SceneBlueprint payload serialization (entities + meshes/materials/images/
    // lights + sources). Used by the scene cook (.vscene): the cook header
    // (magic/version/source-hash) is owned by scene_blueprint.cpp; these
    // (de)serialize just the blueprint body on an open archive.
    static constexpr uint32_t BLUEPRINT_FORMAT_VERSION = 1;
    static void serializeBlueprint(cereal::BinaryOutputArchive& archive, const Vapor::SceneBlueprint& blueprint);
    // Returns ok == false on a version mismatch.
    static Vapor::SceneBlueprint deserializeBlueprint(cereal::BinaryInputArchive& archive);

private:
    static void serializeMaterial(
        cereal::BinaryOutputArchive& archive,
        const std::shared_ptr<Vapor::Material>& material,
        const std::unordered_map<std::shared_ptr<Vapor::Image>, Uint32>& imageIDs
    );
    static std::shared_ptr<Vapor::Material> deserializeMaterial(
        cereal::BinaryInputArchive& archive, const std::unordered_map<Uint32, std::shared_ptr<Vapor::Image>>& images
    );

    static void serializeImage(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Vapor::Image>& image);
    static std::shared_ptr<Vapor::Image> deserializeImage(cereal::BinaryInputArchive& archive);

    static void serializeMesh(
        cereal::BinaryOutputArchive& archive,
        const std::shared_ptr<Vapor::Mesh>& mesh,
        const std::unordered_map<std::shared_ptr<Vapor::Material>, Uint32>& materialIDs
    );
    static std::shared_ptr<Vapor::Mesh> deserializeMesh(
        cereal::BinaryInputArchive& archive, const std::unordered_map<Uint32, std::shared_ptr<Vapor::Material>>& materials
    );

    static void serializeDirectionalLight(cereal::BinaryOutputArchive& archive, const Vapor::DirectionalLight& light);
    static Vapor::DirectionalLight deserializeDirectionalLight(cereal::BinaryInputArchive& archive);

    static void serializePointLight(cereal::BinaryOutputArchive& archive, const Vapor::PointLight& light);
    static Vapor::PointLight deserializePointLight(cereal::BinaryInputArchive& archive);
};