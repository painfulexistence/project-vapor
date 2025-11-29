#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Vapor {

// Forward declarations
class AudioEngineImpl;

// Audio ID type (similar to cocos2d-x)
using AudioID = int;
constexpr AudioID AUDIO_ID_INVALID = -1;

// Maximum number of simultaneous audio instances
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
    Inverse,        // Inverse distance attenuation (default, realistic)
    Exponential     // Exponential distance attenuation
};

// ============================================================
// Audio Profile (for audio caching and configuration)
// ============================================================

struct AudioProfile {
    std::string name;
    int maxInstances = 1;           // Max simultaneous instances of this audio
    float minDelay = 0.0f;          // Min delay between instances (seconds)

    AudioProfile() = default;
    explicit AudioProfile(const std::string& profileName, int maxInst = 1, float delay = 0.0f)
        : name(profileName), maxInstances(maxInst), minDelay(delay) {}
};

// ============================================================
// 3D Audio Source Configuration
// ============================================================

struct Audio3DConfig {
    glm::vec3 position{0.0f};       // Position in world space
    glm::vec3 velocity{0.0f};       // Velocity for Doppler effect
    glm::vec3 direction{0.0f, 0.0f, -1.0f}; // Direction for directional sources

    float minDistance = 1.0f;       // Distance at which volume is 100%
    float maxDistance = 100.0f;     // Distance at which volume is 0% (or min)
    float rolloffFactor = 1.0f;     // How quickly sound attenuates with distance

    // Cone for directional audio
    float coneInnerAngle = 360.0f;  // Full volume cone angle (degrees)
    float coneOuterAngle = 360.0f;  // Outer cone angle (degrees)
    float coneOuterGain = 0.0f;     // Volume outside outer cone (0.0 - 1.0)

    DistanceModel distanceModel = DistanceModel::Inverse;

    Audio3DConfig() = default;
    explicit Audio3DConfig(const glm::vec3& pos)
        : position(pos) {}
};

// ============================================================
// Listener Configuration for 3D Audio
// ============================================================

struct AudioListener {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

// ============================================================
// AudioEngine - High-level Audio API (cocos2d-x style + 3D)
// ============================================================

/**
 * High-level audio engine API similar to cocos2d-x AudioEngine
 * with added support for 3D spatial audio.
 *
 * Basic usage (2D audio):
 *     AudioEngine::init();
 *     int id = AudioEngine::play2d("sound.wav");
 *     AudioEngine::setVolume(id, 0.5f);
 *     AudioEngine::stop(id);
 *
 * 3D Audio usage:
 *     Audio3DConfig config;
 *     config.position = glm::vec3(10.0f, 0.0f, 0.0f);
 *     int id = AudioEngine::play3d("explosion.wav", config);
 *     AudioEngine::setPosition3d(id, newPosition);
 *
 * Listener (camera/player):
 *     AudioEngine::setListenerPosition(cameraPos);
 *     AudioEngine::setListenerOrientation(forward, up);
 */
class AudioEngine {
public:
    // ============================================================
    // Lifecycle
    // ============================================================

    /**
     * Initialize the audio engine.
     * Must be called before any audio operations.
     * @return true if initialization succeeded
     */
    static bool init();

    /**
     * Shutdown the audio engine and release all resources.
     */
    static void shutdown();

    /**
     * Update the audio engine (call each frame for proper cleanup).
     * @param deltaTime Time since last frame in seconds
     */
    static void update(float deltaTime);

    /**
     * Check if the audio engine is initialized.
     */
    static bool isInitialized();

    // ============================================================
    // 2D Audio Playback (similar to cocos2d-x)
    // ============================================================

    /**
     * Play a 2D audio file.
     * @param filePath Path to the audio file (WAV, MP3, OGG, FLAC)
     * @param loop Whether to loop the audio
     * @param volume Volume (0.0 - 1.0)
     * @param profile Optional audio profile for instance limiting
     * @return AudioID for controlling playback, AUDIO_ID_INVALID on failure
     */
    static AudioID play2d(
        const std::string& filePath,
        bool loop = false,
        float volume = 1.0f,
        const AudioProfile* profile = nullptr
    );

    // ============================================================
    // 3D Spatial Audio Playback
    // ============================================================

    /**
     * Play a 3D audio file with spatial positioning.
     * @param filePath Path to the audio file
     * @param config 3D audio configuration (position, attenuation, etc.)
     * @param loop Whether to loop the audio
     * @param volume Base volume (0.0 - 1.0), will be modified by distance
     * @param profile Optional audio profile
     * @return AudioID for controlling playback
     */
    static AudioID play3d(
        const std::string& filePath,
        const Audio3DConfig& config,
        bool loop = false,
        float volume = 1.0f,
        const AudioProfile* profile = nullptr
    );

    // ============================================================
    // Playback Control
    // ============================================================

    /**
     * Stop a specific audio instance.
     */
    static void stop(AudioID audioID);

    /**
     * Stop all audio instances.
     */
    static void stopAll();

    /**
     * Pause a specific audio instance.
     */
    static void pause(AudioID audioID);

    /**
     * Pause all audio instances.
     */
    static void pauseAll();

    /**
     * Resume a paused audio instance.
     */
    static void resume(AudioID audioID);

    /**
     * Resume all paused audio instances.
     */
    static void resumeAll();

    // ============================================================
    // Audio Properties
    // ============================================================

    /**
     * Set the volume for an audio instance.
     * @param audioID The audio instance
     * @param volume Volume (0.0 - 1.0)
     */
    static void setVolume(AudioID audioID, float volume);

    /**
     * Get the volume of an audio instance.
     */
    static float getVolume(AudioID audioID);

    /**
     * Set whether the audio should loop.
     */
    static void setLoop(AudioID audioID, bool loop);

    /**
     * Check if audio is looping.
     */
    static bool isLoop(AudioID audioID);

    /**
     * Set the playback pitch (speed).
     * @param pitch Pitch multiplier (1.0 = normal, 2.0 = double speed)
     */
    static void setPitch(AudioID audioID, float pitch);

    /**
     * Get the playback pitch.
     */
    static float getPitch(AudioID audioID);

    /**
     * Get the current playback position in seconds.
     */
    static float getCurrentTime(AudioID audioID);

    /**
     * Seek to a specific position in seconds.
     */
    static void setCurrentTime(AudioID audioID, float time);

    /**
     * Get the total duration of the audio in seconds.
     */
    static float getDuration(AudioID audioID);

    /**
     * Get the current state of an audio instance.
     */
    static AudioState getState(AudioID audioID);

    // ============================================================
    // 3D Audio Source Control
    // ============================================================

    /**
     * Set the 3D position of an audio source.
     */
    static void setPosition3d(AudioID audioID, const glm::vec3& position);

    /**
     * Get the 3D position of an audio source.
     */
    static glm::vec3 getPosition3d(AudioID audioID);

    /**
     * Set the velocity of an audio source (for Doppler effect).
     */
    static void setVelocity3d(AudioID audioID, const glm::vec3& velocity);

    /**
     * Get the velocity of an audio source.
     */
    static glm::vec3 getVelocity3d(AudioID audioID);

    /**
     * Set the direction of an audio source (for directional audio).
     */
    static void setDirection3d(AudioID audioID, const glm::vec3& direction);

    /**
     * Set distance attenuation parameters.
     */
    static void setDistanceParameters(
        AudioID audioID,
        float minDistance,
        float maxDistance,
        float rolloffFactor = 1.0f
    );

    /**
     * Set the distance model for an audio source.
     */
    static void setDistanceModel(AudioID audioID, DistanceModel model);

    /**
     * Set cone parameters for directional audio.
     * @param innerAngle Inner cone angle in degrees (full volume)
     * @param outerAngle Outer cone angle in degrees
     * @param outerGain Gain outside outer cone (0.0 - 1.0)
     */
    static void setCone(
        AudioID audioID,
        float innerAngle,
        float outerAngle,
        float outerGain
    );

    /**
     * Update full 3D configuration for an audio source.
     */
    static void set3DConfig(AudioID audioID, const Audio3DConfig& config);

    // ============================================================
    // Listener Control (Camera/Player position)
    // ============================================================

    /**
     * Set the listener position in world space.
     */
    static void setListenerPosition(const glm::vec3& position);

    /**
     * Get the listener position.
     */
    static glm::vec3 getListenerPosition();

    /**
     * Set the listener velocity (for Doppler effect).
     */
    static void setListenerVelocity(const glm::vec3& velocity);

    /**
     * Set the listener orientation.
     * @param forward Forward direction vector
     * @param up Up direction vector
     */
    static void setListenerOrientation(const glm::vec3& forward, const glm::vec3& up);

    /**
     * Update the full listener configuration.
     */
    static void setListener(const AudioListener& listener);

    /**
     * Get the current listener configuration.
     */
    static const AudioListener& getListener();

    // ============================================================
    // Global Settings
    // ============================================================

    /**
     * Set the master volume for all audio.
     * @param volume Master volume (0.0 - 1.0)
     */
    static void setMasterVolume(float volume);

    /**
     * Get the master volume.
     */
    static float getMasterVolume();

    /**
     * Set the global distance model (default for new 3D sources).
     */
    static void setGlobalDistanceModel(DistanceModel model);

    /**
     * Set the speed of sound (for Doppler calculations).
     * @param speed Speed of sound in units per second (default: 343.0)
     */
    static void setSpeedOfSound(float speed);

    /**
     * Set the Doppler factor (0 = disabled, 1 = normal, >1 = exaggerated).
     */
    static void setDopplerFactor(float factor);

    // ============================================================
    // Audio Caching / Preloading
    // ============================================================

    /**
     * Preload an audio file into memory for faster playback.
     * @param filePath Path to the audio file
     * @param callback Optional callback when preload completes (success flag)
     */
    static void preload(
        const std::string& filePath,
        std::function<void(bool)> callback = nullptr
    );

    /**
     * Remove a preloaded audio file from cache.
     */
    static void uncache(const std::string& filePath);

    /**
     * Remove all preloaded audio files from cache.
     */
    static void uncacheAll();

    /**
     * Check if an audio file is preloaded.
     */
    static bool isPreloaded(const std::string& filePath);

    // ============================================================
    // Callbacks
    // ============================================================

    /**
     * Set a callback to be invoked when audio playback finishes.
     * @param audioID The audio instance
     * @param callback Function called when playback completes
     */
    static void setFinishCallback(
        AudioID audioID,
        std::function<void(AudioID, const std::string&)> callback
    );

    // ============================================================
    // Utility
    // ============================================================

    /**
     * Get the number of currently playing audio instances.
     */
    static int getPlayingAudioCount();

    /**
     * Get list of all currently playing audio IDs.
     */
    static std::vector<AudioID> getPlayingAudioIDs();

    /**
     * Get the maximum number of simultaneous audio instances.
     */
    static int getMaxAudioInstances();

    /**
     * Get the file path associated with an audio ID.
     */
    static std::string getFilePath(AudioID audioID);

private:
    AudioEngine() = delete;  // Static-only class, no instantiation

    static std::unique_ptr<AudioEngineImpl> s_impl;
};

// ============================================================
// AudioManager - Manager class for EngineCore integration
// ============================================================

/**
 * AudioManager class that integrates with EngineCore.
 * Wraps AudioEngine with manager lifecycle pattern.
 *
 * Usage (automatic via EngineCore):
 *     auto& audioManager = engineCore.getAudioManager();
 *     audioManager.play2d("sound.wav");
 */
class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    // Lifecycle
    bool init();
    void shutdown();
    void update(float deltaTime);

    bool isInitialized() const { return m_initialized; }

    // ============================================================
    // Convenience wrappers (delegates to AudioEngine)
    // ============================================================

    // 2D Audio
    AudioID play2d(const std::string& filePath, bool loop = false, float volume = 1.0f) {
        return AudioEngine::play2d(filePath, loop, volume);
    }

    // 3D Audio
    AudioID play3d(const std::string& filePath, const Audio3DConfig& config,
                   bool loop = false, float volume = 1.0f) {
        return AudioEngine::play3d(filePath, config, loop, volume);
    }

    AudioID play3d(const std::string& filePath, const glm::vec3& position,
                   bool loop = false, float volume = 1.0f) {
        return AudioEngine::play3d(filePath, Audio3DConfig(position), loop, volume);
    }

    // Playback control
    void stop(AudioID id) { AudioEngine::stop(id); }
    void stopAll() { AudioEngine::stopAll(); }
    void pause(AudioID id) { AudioEngine::pause(id); }
    void pauseAll() { AudioEngine::pauseAll(); }
    void resume(AudioID id) { AudioEngine::resume(id); }
    void resumeAll() { AudioEngine::resumeAll(); }

    // Properties
    void setVolume(AudioID id, float vol) { AudioEngine::setVolume(id, vol); }
    float getVolume(AudioID id) { return AudioEngine::getVolume(id); }
    void setLoop(AudioID id, bool loop) { AudioEngine::setLoop(id, loop); }
    void setMasterVolume(float vol) { AudioEngine::setMasterVolume(vol); }

    // 3D Audio
    void setPosition3d(AudioID id, const glm::vec3& pos) { AudioEngine::setPosition3d(id, pos); }
    void setListenerPosition(const glm::vec3& pos) { AudioEngine::setListenerPosition(pos); }
    void setListenerOrientation(const glm::vec3& fwd, const glm::vec3& up) {
        AudioEngine::setListenerOrientation(fwd, up);
    }

    // Preloading
    void preload(const std::string& path) { AudioEngine::preload(path); }
    void uncache(const std::string& path) { AudioEngine::uncache(path); }

private:
    bool m_initialized{false};
};

} // namespace Vapor
