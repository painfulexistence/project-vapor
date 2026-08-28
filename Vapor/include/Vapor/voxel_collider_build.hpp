#pragma once
#include "physics_3d.hpp"
#include "task_scheduler.hpp"
#include "voxel_world.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace Vapor {

    // ========================================================================
    // Asynchronous voxel-collider extraction (the Vapor port of Atmospheric's
    // worker-thread collider build).
    //
    // Extracting a collider is the expensive half of giving a volume physics:
    // greedy-meshing the exposed faces and cooking the Jolt shape (the mesh
    // BVH build) — a quarter second and change for a full 256^3 terrain, which
    // used to land inline on the frame the volume finished generating. Both
    // steps read finished voxel data and touch no physics state, so they move
    // to the task scheduler, mirroring how the volume's own generation runs
    // (one lambda per unit of work, shared state kept alive by capture); only
    // body creation, which mutates the Jolt world, stays on the main thread.
    //
    // Lifetime is capture-based, so nothing ever has to wait on teardown: each
    // task holds a shared_ptr to the build AND to the VoxelWorld it reads. An
    // entity destroyed mid-build just drops its reference; the tasks finish
    // into a result nobody applies, and the last reference frees it. The one
    // hard requirement is that Physics3D (captured by pointer for bake calls)
    // outlives the scheduler's queue — call TaskScheduler::waitForAll() before
    // Physics3D::deinit(), which the demo's shutdown does.
    //
    // Workers hold the world's shared voxel lock for the whole stripe, paired
    // with the exclusive lock carveSphere takes. That is not optional: a carve
    // can materialize bricks and GROW the brick pool, reallocating the very
    // array a reader is walking — use-after-free, not just a torn value.
    // ========================================================================
    struct VoxelColliderBuild {
        // Inputs, immutable once submitted.
        std::shared_ptr<VoxelWorld> world;
        std::vector<int> chunkIndex;// which collider chunks this build covers
        std::vector<glm::ivec3> chunkMin, chunkMax;// voxel boxes, parallel to chunkIndex
        glm::vec3 pivotOffset{ 0.0f };// grid-local -> pivot-local shift
        int hullDirections = 64;
        bool buildHull = false;// hull (dynamic prop) instead of chunk meshes

        // Outputs, valid once ready(): one baked shape per chunk (invalid ref =
        // that chunk holds no surface now), or the hull.
        std::vector<PhysicsShapeRef> chunkShapes;
        PhysicsShapeRef hullShape;
        Uint64 solidAtBuild = 0;// solid count snapshotted at submit

        // Tasks still running. The release store in the last task pairs with
        // the acquire load in ready(), publishing every shape written above.
        std::atomic<Uint32> remaining{ 0 };
        bool ready() const {
            return remaining.load(std::memory_order_acquire) == 0;
        }
    };

    // Queue re-meshing + shape cooking for a set of collider chunks. The dirty
    // list is striped across up to `maxTasks` scheduler tasks — chunks are
    // disjoint, so any number may cook concurrently — and each stripe writes
    // only its own result slots, so the stripes never contend.
    inline std::shared_ptr<VoxelColliderBuild> startChunkColliderBuild(
        TaskScheduler& scheduler,
        Physics3D& physics,
        std::shared_ptr<VoxelWorld> world,
        std::vector<int> dirtyChunks,
        const std::vector<glm::ivec3>& allChunkMin,
        const std::vector<glm::ivec3>& allChunkMax,
        const glm::vec3& pivotOffset,
        Uint32 maxTasks = 8
    ) {
        auto build = std::make_shared<VoxelColliderBuild>();
        build->world = std::move(world);
        build->solidAtBuild = build->world->solidVoxels();
        build->pivotOffset = pivotOffset;
        build->chunkIndex = std::move(dirtyChunks);
        build->chunkMin.reserve(build->chunkIndex.size());
        build->chunkMax.reserve(build->chunkIndex.size());
        for (int idx : build->chunkIndex) {
            build->chunkMin.push_back(allChunkMin[static_cast<size_t>(idx)]);
            build->chunkMax.push_back(allChunkMax[static_cast<size_t>(idx)]);
        }
        build->chunkShapes.resize(build->chunkIndex.size());

        const Uint32 stripes =
            std::max(1u, std::min<Uint32>(maxTasks, static_cast<Uint32>(build->chunkIndex.size())));
        build->remaining.store(stripes, std::memory_order_release);
        for (Uint32 s = 0; s < stripes; s++) {
            Physics3D* phys = &physics;
            scheduler.submitTask([build, phys, s, stripes] {
                // One lock for the whole stripe, not per chunk: a dig landing
                // mid-build waits a fraction of a millisecond instead of
                // interleaving writes into the read.
                const auto voxelLock = build->world->lockVoxelsShared();
                std::vector<glm::vec3> verts;
                std::vector<Uint32> indices;
                for (size_t k = s; k < build->chunkIndex.size(); k += stripes) {
                    build->world->buildSurfaceMeshRegion(verts, indices, build->chunkMin[k], build->chunkMax[k]);
                    if (indices.empty()) continue;// chunk is (or became) empty air
                    for (auto& p : verts) p -= build->pivotOffset;
                    build->chunkShapes[k] = phys->bakeMeshShape(verts, indices);
                }
                build->remaining.fetch_sub(1, std::memory_order_acq_rel);
            });
        }
        return build;
    }

    // Queue a convex-hull cook for a dynamic prop (first build and re-hull
    // alike). One task: a prop hull is a few milliseconds at most.
    inline std::shared_ptr<VoxelColliderBuild> startHullColliderBuild(
        TaskScheduler& scheduler,
        Physics3D& physics,
        std::shared_ptr<VoxelWorld> world,
        int hullDirections,
        const glm::vec3& pivotOffset
    ) {
        auto build = std::make_shared<VoxelColliderBuild>();
        build->world = std::move(world);
        build->solidAtBuild = build->world->solidVoxels();
        build->pivotOffset = pivotOffset;
        build->hullDirections = hullDirections;
        build->buildHull = true;
        build->remaining.store(1, std::memory_order_release);
        Physics3D* phys = &physics;
        scheduler.submitTask([build, phys] {
            const auto voxelLock = build->world->lockVoxelsShared();
            std::vector<glm::vec3> points;
            build->world->buildConvexHullPoints(points, build->hullDirections);
            if (points.size() >= 4) {
                for (auto& p : points) p -= build->pivotOffset;
                build->hullShape = phys->bakeConvexHullShape(points);
            }
            build->remaining.fetch_sub(1, std::memory_order_acq_rel);
        });
        return build;
    }

}// namespace Vapor
