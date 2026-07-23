#include "Vapor/animation_library.hpp"

namespace Vapor {

TimelineHandle AnimationClipLibrary::addTimeline(ActionTimeline timeline) {
    timeline.recompute();
    const std::string name = timeline.name;
    const Uint32 rid = static_cast<Uint32>(m_timelines.size());
    m_timelines.push_back(std::move(timeline));
    if (!name.empty()) m_timelineByName[name] = rid;
    return { rid };
}

SkeletonHandle AnimationClipLibrary::addSkeleton(Skeleton skeleton) {
    const std::string name = skeleton.name;
    const Uint32 rid = static_cast<Uint32>(m_skeletons.size());
    m_skeletons.push_back(std::move(skeleton));
    if (!name.empty()) m_skeletonByName[name] = rid;
    return { rid };
}

SkeletonClipHandle AnimationClipLibrary::addSkeletonClip(SkeletonClip clip) {
    clip.recompute();
    const std::string name = clip.name;
    const Uint32 rid = static_cast<Uint32>(m_skeletonClips.size());
    m_skeletonClips.push_back(std::move(clip));
    if (!name.empty()) m_skeletonClipByName[name] = rid;
    return { rid };
}

const ActionTimeline* AnimationClipLibrary::getTimeline(TimelineHandle h) const {
    if (!h.valid() || h.rid >= m_timelines.size()) return nullptr;
    return &m_timelines[h.rid];
}

const Skeleton* AnimationClipLibrary::getSkeleton(SkeletonHandle h) const {
    if (!h.valid() || h.rid >= m_skeletons.size()) return nullptr;
    return &m_skeletons[h.rid];
}

const SkeletonClip* AnimationClipLibrary::getSkeletonClip(SkeletonClipHandle h) const {
    if (!h.valid() || h.rid >= m_skeletonClips.size()) return nullptr;
    return &m_skeletonClips[h.rid];
}

TimelineHandle AnimationClipLibrary::findTimeline(const std::string& name) const {
    auto it = m_timelineByName.find(name);
    return it != m_timelineByName.end() ? TimelineHandle{ it->second } : TimelineHandle{};
}

SkeletonHandle AnimationClipLibrary::findSkeleton(const std::string& name) const {
    auto it = m_skeletonByName.find(name);
    return it != m_skeletonByName.end() ? SkeletonHandle{ it->second } : SkeletonHandle{};
}

SkeletonClipHandle AnimationClipLibrary::findSkeletonClip(const std::string& name) const {
    auto it = m_skeletonClipByName.find(name);
    return it != m_skeletonClipByName.end() ? SkeletonClipHandle{ it->second } : SkeletonClipHandle{};
}

void AnimationClipLibrary::clear() {
    m_timelines.clear();
    m_skeletons.clear();
    m_skeletonClips.clear();
    m_timelineByName.clear();
    m_skeletonByName.clear();
    m_skeletonClipByName.clear();
}

}// namespace Vapor
