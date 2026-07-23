#pragma once
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Skeleton (a shared, data-only asset)
//
// The reference structure that ties skinned meshes and skeletal clips together:
// a flat, topologically-ordered joint array. A SkeletonClip's channels index
// into this array, and the inverse-bind matrices here turn posed joint
// transforms into skinning matrices. Kept separate from animation clips (like
// glTF's skin vs animations[], and USD's Skeleton prim vs SkelAnimation)
// because one skeleton is shared by many clips and many meshes.
//
// Imported by the glTF/USD loaders and stored in AnimationClipLibrary so the
// data survives the import; a skinning render path is future work.
// ─────────────────────────────────────────────────────────────────────────────

namespace Vapor {

    struct Joint {
        std::string name;
        int parent = -1;// index into Skeleton::joints; -1 for a root

        // Bind (rest) local transform, stored as TRS so a clip that animates
        // only one channel (e.g. rotation) can fall back to the bind value for
        // the rest.
        glm::vec3 bindTranslation{ 0.0f };
        glm::quat bindRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 bindScale{ 1.0f };

        // Maps a vertex from model space into this joint's bind-local space;
        // the skinning matrix is jointModel * inverseBind.
        glm::mat4 inverseBind{ 1.0f };
    };

    struct Skeleton {
        std::string name;
        // Topologically ordered (every joint appears after its parent), so a
        // single forward pass computes model-space matrices.
        std::vector<Joint> joints;

        size_t jointCount() const {
            return joints.size();
        }
    };

}// namespace Vapor
