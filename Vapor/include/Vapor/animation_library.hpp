#pragma once
#include "animation_clip.hpp"
#include "skeleton.hpp"
#include <SDL3/SDL_stdinc.h>
#include <deque>
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// AnimationClipLibrary — the shared clip store
//
// Owns every animation asset (action timelines, skeletons, skeletal clips) so
// instances can share one copy. Owned by EngineCore (next to ActionManager);
// systems and instantiate() reach it through EngineCore::Get(). Handles are
// stable for the lifetime of the library.
//
// Mirrors Atmospheric's AnimationLibrary (same "library owns clips /
// components own playheads" split); the handle spelling follows this repo's
// house style (rid + UINT32_MAX sentinel).
// ─────────────────────────────────────────────────────────────────────────────

namespace Vapor {

    // Global + per-group playback rates for TimelineSystem. Effective dt for a
    // player = dt * global * group(player.groupId), so e.g. gameplay timelines
    // (group 0) can freeze under a pause menu while UI timelines (another
    // group) keep running.
    struct TimelineTimeScales {
        float global = 1.0f;
        std::unordered_map<Uint32, float> groups;

        void setGroup(Uint32 groupId, float scale) {
            groups[groupId] = scale;
        }
        float forGroup(Uint32 groupId) const {
            auto it = groups.find(groupId);
            return global * (it != groups.end() ? it->second : 1.0f);
        }
    };

    class AnimationClipLibrary {
    public:
        // add* recomputes durations and registers the clip's name for find*.
        // Duplicate names overwrite the by-name entry (last registration wins);
        // existing handles keep pointing at their original clips.
        TimelineHandle addTimeline(ActionTimeline timeline);
        SkeletonHandle addSkeleton(Skeleton skeleton);
        SkeletonClipHandle addSkeletonClip(SkeletonClip clip);

        const ActionTimeline* getTimeline(TimelineHandle h) const;
        const Skeleton* getSkeleton(SkeletonHandle h) const;
        const SkeletonClip* getSkeletonClip(SkeletonClipHandle h) const;

        // By-name lookups (return an invalid handle if absent).
        TimelineHandle findTimeline(const std::string& name) const;
        SkeletonHandle findSkeleton(const std::string& name) const;
        SkeletonClipHandle findSkeletonClip(const std::string& name) const;

        size_t timelineCount() const {
            return m_timelines.size();
        }
        size_t skeletonCount() const {
            return m_skeletons.size();
        }
        size_t skeletonClipCount() const {
            return m_skeletonClips.size();
        }

        void clear();

    private:
        // deque, not vector: get*() hands out raw pointers into these
        // containers that callers may hold across later add*() calls. A vector
        // reallocation would dangle them; deque keeps element addresses stable
        // across push_back while preserving index access.
        std::deque<ActionTimeline> m_timelines;// index = handle.rid
        std::deque<Skeleton> m_skeletons;
        std::deque<SkeletonClip> m_skeletonClips;

        std::unordered_map<std::string, Uint32> m_timelineByName;
        std::unordered_map<std::string, Uint32> m_skeletonByName;
        std::unordered_map<std::string, Uint32> m_skeletonClipByName;
    };

}// namespace Vapor
