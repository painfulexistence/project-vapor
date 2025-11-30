#pragma once

#include <glm/glm.hpp>
#include <string>
#include <functional>
#include <array>
#include <vector>
#include <mutex>

// Forward declare miniaudio types
struct ma_engine;
struct ma_sound;

namespace Vapor {

// Audio ID type
using AudioID = int;
constexpr AudioID AUDIO_ID_INVALID = -1;
constexpr int MAX_AUDIO_INSTANCES = 32;

// ============================================================
// Audio State
// ============================================================

enum class AudioState {
    Error = -1,
    Initial,
    Playing,
    Paused,
    Stopped
};

// ============================================================
// Distance Model for 3D Audio
// ============================================================

enum class DistanceModel {
    None,           // No distance attenuation
    Linear,         // Linear distance attenuation
    Inverse,        // Inverse distance attenuation (default)
    Exponential     // Exponential distance attenuation
};

// ============================================================
// 3D Audio Configuration
// ============================================================

struct Audio3DConfig {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};       // For Doppler effect
    glm::vec3 direction{0.0f, 0.0f, -1.0f};

    float minDistance = 1.0f;
    float maxDistance = 100.0f;
    float rolloffFactor = 1.0f;

    // Cone for directional audio
    float coneInnerAngle = 360.0f;  // Full volume (degrees)
    float coneOuterAngle = 360.0f;  // Outer cone (degrees)
    float coneOuterGain = 0.0f;     // Volume outside outer cone

    DistanceModel distanceModel = DistanceModel::Inverse;

    Audio3DConfig() = default;
    explicit Audio3DConfig(const glm::vec3& pos) : position(pos) {}
};

// ============================================================
// Listener Configuration
// ============================================================

struct AudioListener {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

// ============================================================
// AudioManager - High-level Audio API with Spatial Audio
// ============================================================

/**
 * Audio manager with miniaudio backend.
 * Supports 2D and 3D spatial audio playback.
 *
 * Usage:
 *     auto& audio = engineCore.getAudioManager();
 *
 *     // 2D audio
 *     AudioID id = audio.play2d("music.wav", true, 0.8f);
 *
 *     // 3D spatial audio
 *     AudioID id = audio.play3d("explosion.wav", position);
 *     audio.setPosition3d(id, newPosition);
 *
 *     // Listener (camera)
 *     audio.setListenerPosition(cameraPos);
 *     audio.setListenerOrientation(forward, up);
 */
class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    // Non-copyable
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // ============================================================
    // Lifecycle
    // ============================================================

    bool init();
    void shutdown();
    void update(float deltaTime);
    bool isInitialized() const { return m_initialized; }

    // ============================================================
    // 2D Audio Playback
    // ============================================================

    AudioID play2d(const std::string& filename, bool loop = false, float volume = 1.0f);

    // ============================================================
    // 3D Spatial Audio Playback
    // ============================================================

    AudioID play3d(const std::string& filename, const Audio3DConfig& config,
                   bool loop = false, float volume = 1.0f);

    AudioID play3d(const std::string& filename, const glm::vec3& position,
                   bool loop = false, float volume = 1.0f);

    // ============================================================
    // Playback Control
    // ============================================================

    void stop(AudioID id);
    void stopAll();
    void pause(AudioID id);
    void pauseAll();
    void resume(AudioID id);
    void resumeAll();

    // ============================================================
    // Audio Properties
    // ============================================================

    void setVolume(AudioID id, float volume);
    float getVolume(AudioID id) const;

    void setLoop(AudioID id, bool loop);
    bool isLoop(AudioID id) const;

    void setPitch(AudioID id, float pitch);
    float getPitch(AudioID id) const;

    float getCurrentTime(AudioID id) const;
    void setCurrentTime(AudioID id, float time);

    float getDuration(AudioID id) const;
    AudioState getState(AudioID id) const;

    // ============================================================
    // 3D Audio Source Control
    // ============================================================

    void setPosition3d(AudioID id, const glm::vec3& position);
    glm::vec3 getPosition3d(AudioID id) const;

    void setVelocity3d(AudioID id, const glm::vec3& velocity);
    void setDirection3d(AudioID id, const glm::vec3& direction);

    void setDistanceParameters(AudioID id, float minDist, float maxDist, float rolloff = 1.0f);
    void setDistanceModel(AudioID id, DistanceModel model);
    void setCone(AudioID id, float innerAngle, float outerAngle, float outerGain);

    void set3DConfig(AudioID id, const Audio3DConfig& config);

    // ============================================================
    // Listener Control (Camera/Player)
    // ============================================================

    void setListenerPosition(const glm::vec3& position);
    glm::vec3 getListenerPosition() const { return m_listener.position; }

    void setListenerVelocity(const glm::vec3& velocity);
    void setListenerOrientation(const glm::vec3& forward, const glm::vec3& up);

    void setListener(const AudioListener& listener);
    const AudioListener& getListener() const { return m_listener; }

    // ============================================================
    // Global Settings
    // ============================================================

    void setMasterVolume(float volume);
    float getMasterVolume() const { return m_masterVolume; }

    void setDopplerFactor(float factor);

    // ============================================================
    // Callbacks
    // ============================================================

    void setFinishCallback(AudioID id, std::function<void(AudioID, const std::string&)> callback);

    // ============================================================
    // Utility
    // ============================================================

    int getPlayingCount() const;
    std::string getFilePath(AudioID id) const;

private:
    struct AudioInstance {
        ma_sound* sound = nullptr;
        std::string filePath;
        AudioID id = AUDIO_ID_INVALID;
        AudioState state = AudioState::Initial;
        bool is3D = false;
        float volume = 1.0f;
        Audio3DConfig config3D;
        std::function<void(AudioID, const std::string&)> finishCallback;
    };

    AudioInstance* getInstance(AudioID id);
    const AudioInstance* getInstance(AudioID id) const;
    AudioID allocateInstance();
    void cleanupInstance(AudioInstance& inst);

    ma_engine* m_engine = nullptr;
    std::array<AudioInstance, MAX_AUDIO_INSTANCES> m_instances;
    AudioID m_nextID = 0;

    AudioListener m_listener;
    float m_masterVolume = 1.0f;
    bool m_initialized = false;

    mutable std::mutex m_mutex;

    // Pending callbacks to invoke on main thread (outside mutex)
    struct PendingCallback {
        std::function<void(AudioID, const std::string&)> callback;
        AudioID id;
        std::string filePath;
    };
    std::vector<PendingCallback> m_pendingCallbacks;
};

} // namespace Vapor
