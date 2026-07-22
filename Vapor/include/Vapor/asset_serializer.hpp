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

    template<class Archive> void serialize(Archive& archive, Vapor::Meshlet& m) {
        archive(m.vertexOffset, m.triangleOffset, m.vertexCount, m.triangleCount);
    }

    template<class Archive> void serialize(Archive& archive, Vapor::MeshletBounds& b) {
        archive(b.cullSphere, b.coneApex, b.coneAxisCutoff, b.lodSphere, b.parentSphere,
                b.lodError, b.parentError, b.group, b.refined, b.depth);
    }
}// namespace cereal

namespace Vapor {

class AssetSerializer {
public:
    // SceneBlueprint payload serialization (entities + meshes/materials/images/
    // lights + sources). Used by the scene cook (.vscene): the cook header
    // (magic/version/source-hash) is owned by scene_blueprint.cpp; these
    // (de)serialize just the blueprint body on an open archive.
    // v2: EntityBlueprint carries a per-entity "components" JSON blob.
    // v4: the shared (de)serializeMesh now round-trips Mesh::meshletData, so the
    // per-mesh layout the blueprint cook writes changed. Bump so a stale v3 .vscene
    // (whose meshes have no meshlet fields) is rejected and re-cooked instead of
    // misreading later bytes as a meshlet count (huge alloc -> crash).
    static constexpr uint32_t BLUEPRINT_FORMAT_VERSION = 4; // v3: EntityBlueprint::primitive; v4: mesh meshletData
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

} // namespace Vapor

// Transitional shim: these types lived at global scope before the namespace
// unification; unqualified call sites keep compiling while they migrate to
// Vapor:: qualification. Remove once call sites are migrated.
using namespace Vapor;
