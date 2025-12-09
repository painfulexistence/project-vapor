#pragma once

#include "action_components.hpp"
#include <entt/entt.hpp>
#include <string>
#include <vector>

// ============================================================
// Timeline System - Multi-track time-based action execution
//
// Unlike ActionQueue (sequential, single-track), Timeline is:
// - Time-based (actions trigger at specific times)
// - Multi-track (multiple entities animated in parallel)
// ============================================================

// ============================================================
// Timeline Entry - A single action at a specific time
// ============================================================

struct TimelineEntry {
    float startTime = 0.0f;  // When to trigger (seconds from timeline start)
    Action action;           // What to do
};

// ============================================================
// Timeline Track - All entries for one entity
// ============================================================

struct TimelineTrack {
    entt::entity target = entt::null;  // Which entity this track controls
    std::vector<TimelineEntry> entries;

    // Builder
    TimelineTrack& at(float time, Action action) {
        entries.push_back({ time, std::move(action) });
        return *this;
    }
};

// ============================================================
// TimelineComponent - Multi-track timeline attached to entity
// ============================================================

struct TimelineComponent {
    std::vector<TimelineTrack> tracks;
    float elapsed = 0.0f;
    float duration = 0.0f;
    bool playing = false;
    bool loop = false;
    uint32_t completionTag = 0;

    // Track which entries have been triggered (reset on loop)
    std::vector<std::vector<bool>> triggered;

    void play() {
        playing = true;
        elapsed = 0.0f;
        resetTriggers();
    }

    void stop() {
        playing = false;
    }

    void resetTriggers() {
        triggered.resize(tracks.size());
        for (size_t i = 0; i < tracks.size(); i++) {
            triggered[i].assign(tracks[i].entries.size(), false);
        }
    }

    // Calculate duration from tracks
    void calculateDuration() {
        duration = 0.0f;
        for (const auto& track : tracks) {
            for (const auto& entry : track.entries) {
                float endTime = entry.startTime + entry.action.duration;
                duration = std::max(duration, endTime);
            }
        }
    }
};

// ============================================================
// Timeline Builder - Fluent API
// ============================================================

class TimelineBuilder {
public:
    TimelineBuilder& track(entt::entity target) {
        m_tracks.push_back({ .target = target });
        m_currentTrack = m_tracks.size() - 1;
        return *this;
    }

    TimelineBuilder& at(float time, Action action) {
        if (m_currentTrack < m_tracks.size()) {
            m_tracks[m_currentTrack].entries.push_back({ time, std::move(action) });
        }
        return *this;
    }

    TimelineBuilder& loop(bool enable = true) {
        m_loop = enable;
        return *this;
    }

    TimelineBuilder& onComplete(uint32_t tag) {
        m_completionTag = tag;
        return *this;
    }

    TimelineComponent build() {
        TimelineComponent timeline;
        timeline.tracks = std::move(m_tracks);
        timeline.loop = m_loop;
        timeline.completionTag = m_completionTag;
        timeline.calculateDuration();
        timeline.resetTriggers();
        timeline.playing = true;  // Auto-play on build
        return timeline;
    }

private:
    std::vector<TimelineTrack> m_tracks;
    size_t m_currentTrack = 0;
    bool m_loop = false;
    uint32_t m_completionTag = 0;
};

// Convenience function
inline TimelineBuilder Timeline() { return TimelineBuilder(); }

// ============================================================
// Cinematic System - High-level cutscene orchestration
//
// A cinematic is a sequence of "shots", each containing:
// - Camera configuration
// - Timeline of actions
// - Optional dialogue
// ============================================================

struct CinematicShot {
    std::string name;
    float duration = 0.0f;           // Auto-calculated or explicit

    // Camera
    entt::entity camera = entt::null;
    glm::vec3 cameraPosition{ 0.0f };
    glm::quat cameraRotation{ 1, 0, 0, 0 };
    float cameraFOV = 60.0f;

    // Timeline for this shot
    std::vector<TimelineTrack> tracks;

    // Dialogue (optional)
    std::string dialogueSpeaker;
    std::string dialogueText;
};

struct CinematicComponent {
    std::string name;
    std::vector<CinematicShot> shots;
    size_t currentShot = 0;
    float shotElapsed = 0.0f;
    bool playing = false;
    bool skippable = true;
    uint32_t completionTag = 0;

    // Runtime state for current shot's timeline
    std::vector<std::vector<bool>> triggered;

    void play() {
        playing = true;
        currentShot = 0;
        shotElapsed = 0.0f;
        if (!shots.empty()) {
            resetTriggersForShot(0);
        }
    }

    void skip() {
        if (skippable && playing) {
            playing = false;
        }
    }

    bool isComplete() const {
        return currentShot >= shots.size();
    }

    CinematicShot* getCurrentShot() {
        if (currentShot < shots.size()) {
            return &shots[currentShot];
        }
        return nullptr;
    }

    void advanceShot() {
        currentShot++;
        shotElapsed = 0.0f;
        if (currentShot < shots.size()) {
            resetTriggersForShot(currentShot);
        }
    }

    void resetTriggersForShot(size_t shotIndex) {
        if (shotIndex >= shots.size()) return;
        const auto& shot = shots[shotIndex];
        triggered.resize(shot.tracks.size());
        for (size_t i = 0; i < shot.tracks.size(); i++) {
            triggered[i].assign(shot.tracks[i].entries.size(), false);
        }
    }
};

// ============================================================
// Cinematic Builder - Fluent API
// ============================================================

class CinematicBuilder {
public:
    CinematicBuilder& name(const std::string& n) {
        m_name = n;
        return *this;
    }

    CinematicBuilder& shot(const std::string& shotName) {
        m_shots.push_back({ .name = shotName });
        m_currentShot = m_shots.size() - 1;
        return *this;
    }

    CinematicBuilder& duration(float d) {
        if (m_currentShot < m_shots.size()) {
            m_shots[m_currentShot].duration = d;
        }
        return *this;
    }

    CinematicBuilder& camera(entt::entity cam, const glm::vec3& pos, const glm::quat& rot = glm::quat(1,0,0,0), float fov = 60.0f) {
        if (m_currentShot < m_shots.size()) {
            m_shots[m_currentShot].camera = cam;
            m_shots[m_currentShot].cameraPosition = pos;
            m_shots[m_currentShot].cameraRotation = rot;
            m_shots[m_currentShot].cameraFOV = fov;
        }
        return *this;
    }

    CinematicBuilder& track(entt::entity target) {
        if (m_currentShot < m_shots.size()) {
            m_shots[m_currentShot].tracks.push_back({ .target = target });
            m_currentTrack = m_shots[m_currentShot].tracks.size() - 1;
        }
        return *this;
    }

    CinematicBuilder& at(float time, Action action) {
        if (m_currentShot < m_shots.size() && m_currentTrack < m_shots[m_currentShot].tracks.size()) {
            m_shots[m_currentShot].tracks[m_currentTrack].entries.push_back({ time, std::move(action) });
        }
        return *this;
    }

    CinematicBuilder& dialogue(const std::string& speaker, const std::string& text) {
        if (m_currentShot < m_shots.size()) {
            m_shots[m_currentShot].dialogueSpeaker = speaker;
            m_shots[m_currentShot].dialogueText = text;
        }
        return *this;
    }

    CinematicBuilder& skippable(bool s) {
        m_skippable = s;
        return *this;
    }

    CinematicBuilder& onComplete(uint32_t tag) {
        m_completionTag = tag;
        return *this;
    }

    CinematicComponent build() {
        CinematicComponent cinematic;
        cinematic.name = m_name;
        cinematic.shots = std::move(m_shots);
        cinematic.skippable = m_skippable;
        cinematic.completionTag = m_completionTag;

        // Calculate shot durations if not set
        for (auto& shot : cinematic.shots) {
            if (shot.duration <= 0.0f) {
                for (const auto& track : shot.tracks) {
                    for (const auto& entry : track.entries) {
                        float endTime = entry.startTime + entry.action.duration;
                        shot.duration = std::max(shot.duration, endTime);
                    }
                }
                if (shot.duration <= 0.0f) shot.duration = 1.0f; // Minimum
            }
        }

        cinematic.playing = true;
        if (!cinematic.shots.empty()) {
            cinematic.resetTriggersForShot(0);
        }
        return cinematic;
    }

private:
    std::string m_name;
    std::vector<CinematicShot> m_shots;
    size_t m_currentShot = 0;
    size_t m_currentTrack = 0;
    bool m_skippable = true;
    uint32_t m_completionTag = 0;
};

inline CinematicBuilder Cinematic() { return CinematicBuilder(); }
