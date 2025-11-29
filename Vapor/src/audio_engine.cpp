#define MINIAUDIO_IMPLEMENTATION
#include "../miniaudio/miniaudio.h"

#include "Vapor/audio_engine.hpp"
#include <fmt/core.h>
#include <mutex>
#include <array>
#include <cmath>

namespace Vapor {

// ============================================================
// Audio Instance - Internal representation of a playing sound
// ============================================================

struct AudioInstance {
    ma_sound sound;
    std::string filePath;
    AudioID id = AUDIO_ID_INVALID;
    AudioState state = AudioState::Initial;
    bool is3D = false;
    bool isValid = false;
    float volume = 1.0f;
    float pitch = 1.0f;
    Audio3DConfig config3D;
    std::function<void(AudioID, const std::string&)> finishCallback;
};

// ============================================================
// Cached Audio Data
// ============================================================

struct CachedAudio {
    ma_decoder decoder;
    std::vector<float> pcmData;
    ma_uint32 sampleRate;
    ma_uint32 channels;
    bool loaded = false;
};

// ============================================================
// AudioEngineImpl - Implementation class
// ============================================================

class AudioEngineImpl {
public:
    AudioEngineImpl() = default;
    ~AudioEngineImpl() { shutdown(); }

    bool init();
    void shutdown();
    void update(float deltaTime);
    bool isInitialized() const { return m_initialized; }

    // 2D Playback
    AudioID play2d(const std::string& filePath, bool loop, float volume, const AudioProfile* profile);

    // 3D Playback
    AudioID play3d(const std::string& filePath, const Audio3DConfig& config,
                   bool loop, float volume, const AudioProfile* profile);

    // Playback control
    void stop(AudioID id);
    void stopAll();
    void pause(AudioID id);
    void pauseAll();
    void resume(AudioID id);
    void resumeAll();

    // Properties
    void setVolume(AudioID id, float volume);
    float getVolume(AudioID id);
    void setLoop(AudioID id, bool loop);
    bool isLoop(AudioID id);
    void setPitch(AudioID id, float pitch);
    float getPitch(AudioID id);
    float getCurrentTime(AudioID id);
    void setCurrentTime(AudioID id, float time);
    float getDuration(AudioID id);
    AudioState getState(AudioID id);

    // 3D Audio
    void setPosition3d(AudioID id, const glm::vec3& position);
    glm::vec3 getPosition3d(AudioID id);
    void setVelocity3d(AudioID id, const glm::vec3& velocity);
    glm::vec3 getVelocity3d(AudioID id);
    void setDirection3d(AudioID id, const glm::vec3& direction);
    void setDistanceParameters(AudioID id, float minDist, float maxDist, float rolloff);
    void setDistanceModel(AudioID id, DistanceModel model);
    void setCone(AudioID id, float innerAngle, float outerAngle, float outerGain);
    void set3DConfig(AudioID id, const Audio3DConfig& config);

    // Listener
    void setListenerPosition(const glm::vec3& position);
    glm::vec3 getListenerPosition();
    void setListenerVelocity(const glm::vec3& velocity);
    void setListenerOrientation(const glm::vec3& forward, const glm::vec3& up);
    void setListener(const AudioListener& listener);
    const AudioListener& getListener() const { return m_listener; }

    // Global settings
    void setMasterVolume(float volume);
    float getMasterVolume() const { return m_masterVolume; }
    void setGlobalDistanceModel(DistanceModel model);
    void setSpeedOfSound(float speed);
    void setDopplerFactor(float factor);

    // Caching
    void preload(const std::string& filePath, std::function<void(bool)> callback);
    void uncache(const std::string& filePath);
    void uncacheAll();
    bool isPreloaded(const std::string& filePath);

    // Callbacks
    void setFinishCallback(AudioID id, std::function<void(AudioID, const std::string&)> callback);

    // Utility
    int getPlayingAudioCount();
    std::vector<AudioID> getPlayingAudioIDs();
    std::string getFilePath(AudioID id);

private:
    AudioInstance* getInstance(AudioID id);
    AudioID allocateInstance();
    void freeInstance(AudioID id);
    void applyDistanceModel(ma_sound* sound, DistanceModel model);
    ma_attenuation_model toMiniaudioAttenuationModel(DistanceModel model);

    ma_engine m_engine;
    bool m_initialized = false;

    std::array<AudioInstance, MAX_AUDIO_INSTANCES> m_instances;
    AudioID m_nextID = 0;

    std::unordered_map<std::string, std::unique_ptr<CachedAudio>> m_cache;

    AudioListener m_listener;
    float m_masterVolume = 1.0f;
    DistanceModel m_globalDistanceModel = DistanceModel::Inverse;
    float m_speedOfSound = 343.0f;
    float m_dopplerFactor = 1.0f;

    mutable std::mutex m_mutex;
};

// ============================================================
// AudioEngineImpl Implementation
// ============================================================

bool AudioEngineImpl::init() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized) {
        fmt::print("AudioEngine already initialized\n");
        return true;
    }

    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = 2;
    engineConfig.sampleRate = 44100;
    engineConfig.listenerCount = 1;

    ma_result result = ma_engine_init(&engineConfig, &m_engine);
    if (result != MA_SUCCESS) {
        fmt::print("Failed to initialize audio engine: {}\n", static_cast<int>(result));
        return false;
    }

    // Initialize all instances as invalid
    for (auto& instance : m_instances) {
        instance.isValid = false;
        instance.id = AUDIO_ID_INVALID;
    }

    m_initialized = true;
    fmt::print("AudioEngine initialized successfully\n");
    return true;
}

void AudioEngineImpl::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        return;
    }

    fmt::print("Shutting down AudioEngine...\n");

    // Stop and uninit all sounds
    for (auto& instance : m_instances) {
        if (instance.isValid) {
            ma_sound_uninit(&instance.sound);
            instance.isValid = false;
        }
    }

    // Clear cache
    m_cache.clear();

    ma_engine_uninit(&m_engine);
    m_initialized = false;

    fmt::print("AudioEngine shutdown complete\n");
}

void AudioEngineImpl::update(float deltaTime) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        return;
    }

    // Check for finished sounds and invoke callbacks
    for (auto& instance : m_instances) {
        if (instance.isValid && instance.state == AudioState::Playing) {
            if (ma_sound_at_end(&instance.sound)) {
                // Sound finished
                if (!ma_sound_is_looping(&instance.sound)) {
                    instance.state = AudioState::Stopped;

                    // Invoke callback
                    if (instance.finishCallback) {
                        auto callback = instance.finishCallback;
                        auto id = instance.id;
                        auto path = instance.filePath;

                        // Call callback outside the lock
                        m_mutex.unlock();
                        callback(id, path);
                        m_mutex.lock();
                    }

                    // Free the instance
                    ma_sound_uninit(&instance.sound);
                    instance.isValid = false;
                }
            }
        }
    }
}

AudioID AudioEngineImpl::play2d(const std::string& filePath, bool loop, float volume, const AudioProfile* profile) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        fmt::print("AudioEngine not initialized\n");
        return AUDIO_ID_INVALID;
    }

    AudioID id = allocateInstance();
    if (id == AUDIO_ID_INVALID) {
        fmt::print("No available audio instance slots\n");
        return AUDIO_ID_INVALID;
    }

    AudioInstance& instance = m_instances[id % MAX_AUDIO_INSTANCES];

    // Initialize sound from file
    ma_uint32 flags = MA_SOUND_FLAG_DECODE;
    ma_result result = ma_sound_init_from_file(&m_engine, filePath.c_str(), flags, nullptr, nullptr, &instance.sound);
    if (result != MA_SUCCESS) {
        fmt::print("Failed to load audio file: {} (error: {})\n", filePath, static_cast<int>(result));
        instance.isValid = false;
        return AUDIO_ID_INVALID;
    }

    instance.id = id;
    instance.filePath = filePath;
    instance.is3D = false;
    instance.isValid = true;
    instance.volume = volume;
    instance.state = AudioState::Playing;

    // Configure sound
    ma_sound_set_volume(&instance.sound, volume * m_masterVolume);
    ma_sound_set_looping(&instance.sound, loop);

    // Disable spatialization for 2D sounds
    ma_sound_set_spatialization_enabled(&instance.sound, MA_FALSE);

    // Start playback
    result = ma_sound_start(&instance.sound);
    if (result != MA_SUCCESS) {
        fmt::print("Failed to start audio playback: {}\n", static_cast<int>(result));
        ma_sound_uninit(&instance.sound);
        instance.isValid = false;
        return AUDIO_ID_INVALID;
    }

    return id;
}

AudioID AudioEngineImpl::play3d(const std::string& filePath, const Audio3DConfig& config,
                                 bool loop, float volume, const AudioProfile* profile) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        fmt::print("AudioEngine not initialized\n");
        return AUDIO_ID_INVALID;
    }

    AudioID id = allocateInstance();
    if (id == AUDIO_ID_INVALID) {
        fmt::print("No available audio instance slots\n");
        return AUDIO_ID_INVALID;
    }

    AudioInstance& instance = m_instances[id % MAX_AUDIO_INSTANCES];

    // Initialize sound from file
    ma_uint32 flags = MA_SOUND_FLAG_DECODE;
    ma_result result = ma_sound_init_from_file(&m_engine, filePath.c_str(), flags, nullptr, nullptr, &instance.sound);
    if (result != MA_SUCCESS) {
        fmt::print("Failed to load audio file: {} (error: {})\n", filePath, static_cast<int>(result));
        instance.isValid = false;
        return AUDIO_ID_INVALID;
    }

    instance.id = id;
    instance.filePath = filePath;
    instance.is3D = true;
    instance.isValid = true;
    instance.volume = volume;
    instance.config3D = config;
    instance.state = AudioState::Playing;

    // Configure sound
    ma_sound_set_volume(&instance.sound, volume * m_masterVolume);
    ma_sound_set_looping(&instance.sound, loop);

    // Enable spatialization for 3D sounds
    ma_sound_set_spatialization_enabled(&instance.sound, MA_TRUE);

    // Set 3D position
    ma_sound_set_position(&instance.sound, config.position.x, config.position.y, config.position.z);

    // Set velocity for Doppler
    ma_sound_set_velocity(&instance.sound, config.velocity.x, config.velocity.y, config.velocity.z);

    // Set direction
    ma_sound_set_direction(&instance.sound, config.direction.x, config.direction.y, config.direction.z);

    // Set distance attenuation
    applyDistanceModel(&instance.sound, config.distanceModel);
    ma_sound_set_min_distance(&instance.sound, config.minDistance);
    ma_sound_set_max_distance(&instance.sound, config.maxDistance);
    ma_sound_set_rolloff(&instance.sound, config.rolloffFactor);

    // Set cone for directional audio
    ma_sound_set_cone(&instance.sound,
                      config.coneInnerAngle * (MA_PI / 180.0f),
                      config.coneOuterAngle * (MA_PI / 180.0f),
                      config.coneOuterGain);

    // Start playback
    result = ma_sound_start(&instance.sound);
    if (result != MA_SUCCESS) {
        fmt::print("Failed to start 3D audio playback: {}\n", static_cast<int>(result));
        ma_sound_uninit(&instance.sound);
        instance.isValid = false;
        return AUDIO_ID_INVALID;
    }

    return id;
}

void AudioEngineImpl::stop(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return;

    ma_sound_stop(&instance->sound);
    instance->state = AudioState::Stopped;
    ma_sound_uninit(&instance->sound);
    instance->isValid = false;
}

void AudioEngineImpl::stopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& instance : m_instances) {
        if (instance.isValid) {
            ma_sound_stop(&instance.sound);
            instance.state = AudioState::Stopped;
            ma_sound_uninit(&instance.sound);
            instance.isValid = false;
        }
    }
}

void AudioEngineImpl::pause(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || instance->state != AudioState::Playing) return;

    ma_sound_stop(&instance->sound);
    instance->state = AudioState::Paused;
}

void AudioEngineImpl::pauseAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& instance : m_instances) {
        if (instance.isValid && instance.state == AudioState::Playing) {
            ma_sound_stop(&instance.sound);
            instance.state = AudioState::Paused;
        }
    }
}

void AudioEngineImpl::resume(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || instance->state != AudioState::Paused) return;

    ma_sound_start(&instance->sound);
    instance->state = AudioState::Playing;
}

void AudioEngineImpl::resumeAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& instance : m_instances) {
        if (instance.isValid && instance.state == AudioState::Paused) {
            ma_sound_start(&instance.sound);
            instance.state = AudioState::Playing;
        }
    }
}

void AudioEngineImpl::setVolume(AudioID id, float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return;

    instance->volume = volume;
    ma_sound_set_volume(&instance->sound, volume * m_masterVolume);
}

float AudioEngineImpl::getVolume(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return 0.0f;

    return instance->volume;
}

void AudioEngineImpl::setLoop(AudioID id, bool loop) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return;

    ma_sound_set_looping(&instance->sound, loop);
}

bool AudioEngineImpl::isLoop(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return false;

    return ma_sound_is_looping(&instance->sound);
}

void AudioEngineImpl::setPitch(AudioID id, float pitch) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return;

    instance->pitch = pitch;
    ma_sound_set_pitch(&instance->sound, pitch);
}

float AudioEngineImpl::getPitch(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return 1.0f;

    return instance->pitch;
}

float AudioEngineImpl::getCurrentTime(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return 0.0f;

    ma_uint64 cursor;
    ma_sound_get_cursor_in_pcm_frames(&instance->sound, &cursor);

    ma_uint32 sampleRate = ma_engine_get_sample_rate(&m_engine);
    return static_cast<float>(cursor) / static_cast<float>(sampleRate);
}

void AudioEngineImpl::setCurrentTime(AudioID id, float time) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return;

    ma_uint32 sampleRate = ma_engine_get_sample_rate(&m_engine);
    ma_uint64 cursor = static_cast<ma_uint64>(time * sampleRate);
    ma_sound_seek_to_pcm_frame(&instance->sound, cursor);
}

float AudioEngineImpl::getDuration(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return 0.0f;

    float length = 0.0f;
    ma_sound_get_length_in_seconds(&instance->sound, &length);
    return length;
}

AudioState AudioEngineImpl::getState(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return AudioState::Error;

    return instance->state;
}

void AudioEngineImpl::setPosition3d(AudioID id, const glm::vec3& position) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || !instance->is3D) return;

    instance->config3D.position = position;
    ma_sound_set_position(&instance->sound, position.x, position.y, position.z);
}

glm::vec3 AudioEngineImpl::getPosition3d(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return glm::vec3(0.0f);

    return instance->config3D.position;
}

void AudioEngineImpl::setVelocity3d(AudioID id, const glm::vec3& velocity) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || !instance->is3D) return;

    instance->config3D.velocity = velocity;
    ma_sound_set_velocity(&instance->sound, velocity.x, velocity.y, velocity.z);
}

glm::vec3 AudioEngineImpl::getVelocity3d(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return glm::vec3(0.0f);

    return instance->config3D.velocity;
}

void AudioEngineImpl::setDirection3d(AudioID id, const glm::vec3& direction) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || !instance->is3D) return;

    instance->config3D.direction = direction;
    ma_sound_set_direction(&instance->sound, direction.x, direction.y, direction.z);
}

void AudioEngineImpl::setDistanceParameters(AudioID id, float minDist, float maxDist, float rolloff) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || !instance->is3D) return;

    instance->config3D.minDistance = minDist;
    instance->config3D.maxDistance = maxDist;
    instance->config3D.rolloffFactor = rolloff;

    ma_sound_set_min_distance(&instance->sound, minDist);
    ma_sound_set_max_distance(&instance->sound, maxDist);
    ma_sound_set_rolloff(&instance->sound, rolloff);
}

void AudioEngineImpl::setDistanceModel(AudioID id, DistanceModel model) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || !instance->is3D) return;

    instance->config3D.distanceModel = model;
    applyDistanceModel(&instance->sound, model);
}

void AudioEngineImpl::setCone(AudioID id, float innerAngle, float outerAngle, float outerGain) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || !instance->is3D) return;

    instance->config3D.coneInnerAngle = innerAngle;
    instance->config3D.coneOuterAngle = outerAngle;
    instance->config3D.coneOuterGain = outerGain;

    ma_sound_set_cone(&instance->sound,
                      innerAngle * (MA_PI / 180.0f),
                      outerAngle * (MA_PI / 180.0f),
                      outerGain);
}

void AudioEngineImpl::set3DConfig(AudioID id, const Audio3DConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance || !instance->is3D) return;

    instance->config3D = config;

    ma_sound_set_position(&instance->sound, config.position.x, config.position.y, config.position.z);
    ma_sound_set_velocity(&instance->sound, config.velocity.x, config.velocity.y, config.velocity.z);
    ma_sound_set_direction(&instance->sound, config.direction.x, config.direction.y, config.direction.z);

    applyDistanceModel(&instance->sound, config.distanceModel);
    ma_sound_set_min_distance(&instance->sound, config.minDistance);
    ma_sound_set_max_distance(&instance->sound, config.maxDistance);
    ma_sound_set_rolloff(&instance->sound, config.rolloffFactor);

    ma_sound_set_cone(&instance->sound,
                      config.coneInnerAngle * (MA_PI / 180.0f),
                      config.coneOuterAngle * (MA_PI / 180.0f),
                      config.coneOuterGain);
}

void AudioEngineImpl::setListenerPosition(const glm::vec3& position) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;

    m_listener.position = position;
    ma_engine_listener_set_position(&m_engine, 0, position.x, position.y, position.z);
}

glm::vec3 AudioEngineImpl::getListenerPosition() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_listener.position;
}

void AudioEngineImpl::setListenerVelocity(const glm::vec3& velocity) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;

    m_listener.velocity = velocity;
    ma_engine_listener_set_velocity(&m_engine, 0, velocity.x, velocity.y, velocity.z);
}

void AudioEngineImpl::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;

    m_listener.forward = forward;
    m_listener.up = up;
    ma_engine_listener_set_direction(&m_engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&m_engine, 0, up.x, up.y, up.z);
}

void AudioEngineImpl::setListener(const AudioListener& listener) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;

    m_listener = listener;
    ma_engine_listener_set_position(&m_engine, 0, listener.position.x, listener.position.y, listener.position.z);
    ma_engine_listener_set_velocity(&m_engine, 0, listener.velocity.x, listener.velocity.y, listener.velocity.z);
    ma_engine_listener_set_direction(&m_engine, 0, listener.forward.x, listener.forward.y, listener.forward.z);
    ma_engine_listener_set_world_up(&m_engine, 0, listener.up.x, listener.up.y, listener.up.z);
}

void AudioEngineImpl::setMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_masterVolume = volume;

    if (!m_initialized) return;

    ma_engine_set_volume(&m_engine, volume);
}

void AudioEngineImpl::setGlobalDistanceModel(DistanceModel model) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_globalDistanceModel = model;
}

void AudioEngineImpl::setSpeedOfSound(float speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_speedOfSound = speed;
    // Note: miniaudio handles Doppler internally
}

void AudioEngineImpl::setDopplerFactor(float factor) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dopplerFactor = factor;

    if (!m_initialized) return;

    // Apply to all 3D sounds
    for (auto& instance : m_instances) {
        if (instance.isValid && instance.is3D) {
            ma_sound_set_doppler_factor(&instance.sound, factor);
        }
    }
}

void AudioEngineImpl::preload(const std::string& filePath, std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Check if already cached
    if (m_cache.find(filePath) != m_cache.end()) {
        if (callback) callback(true);
        return;
    }

    // For now, just mark as preloaded
    // miniaudio handles its own caching efficiently
    auto cached = std::make_unique<CachedAudio>();
    cached->loaded = true;
    m_cache[filePath] = std::move(cached);

    if (callback) callback(true);
}

void AudioEngineImpl::uncache(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(filePath);
}

void AudioEngineImpl::uncacheAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
}

bool AudioEngineImpl::isPreloaded(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.find(filePath) != m_cache.end();
}

void AudioEngineImpl::setFinishCallback(AudioID id, std::function<void(AudioID, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return;

    instance->finishCallback = std::move(callback);
}

int AudioEngineImpl::getPlayingAudioCount() {
    std::lock_guard<std::mutex> lock(m_mutex);

    int count = 0;
    for (const auto& instance : m_instances) {
        if (instance.isValid && instance.state == AudioState::Playing) {
            count++;
        }
    }
    return count;
}

std::vector<AudioID> AudioEngineImpl::getPlayingAudioIDs() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<AudioID> ids;
    for (const auto& instance : m_instances) {
        if (instance.isValid && instance.state == AudioState::Playing) {
            ids.push_back(instance.id);
        }
    }
    return ids;
}

std::string AudioEngineImpl::getFilePath(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioInstance* instance = getInstance(id);
    if (!instance) return "";

    return instance->filePath;
}

AudioInstance* AudioEngineImpl::getInstance(AudioID id) {
    if (id == AUDIO_ID_INVALID) return nullptr;

    int index = id % MAX_AUDIO_INSTANCES;
    AudioInstance& instance = m_instances[index];

    if (!instance.isValid || instance.id != id) {
        return nullptr;
    }

    return &instance;
}

AudioID AudioEngineImpl::allocateInstance() {
    for (int i = 0; i < MAX_AUDIO_INSTANCES; i++) {
        int index = (m_nextID + i) % MAX_AUDIO_INSTANCES;
        if (!m_instances[index].isValid) {
            AudioID id = m_nextID++;
            return id;
        }
    }
    return AUDIO_ID_INVALID;
}

void AudioEngineImpl::freeInstance(AudioID id) {
    AudioInstance* instance = getInstance(id);
    if (instance) {
        instance->isValid = false;
    }
}

void AudioEngineImpl::applyDistanceModel(ma_sound* sound, DistanceModel model) {
    ma_sound_set_attenuation_model(sound, toMiniaudioAttenuationModel(model));
}

ma_attenuation_model AudioEngineImpl::toMiniaudioAttenuationModel(DistanceModel model) {
    switch (model) {
        case DistanceModel::None:
            return ma_attenuation_model_none;
        case DistanceModel::Linear:
            return ma_attenuation_model_linear;
        case DistanceModel::Inverse:
            return ma_attenuation_model_inverse;
        case DistanceModel::Exponential:
            return ma_attenuation_model_exponential;
        default:
            return ma_attenuation_model_inverse;
    }
}

// ============================================================
// AudioEngine Static Implementation
// ============================================================

std::unique_ptr<AudioEngineImpl> AudioEngine::s_impl = nullptr;

bool AudioEngine::init() {
    if (s_impl) {
        return s_impl->isInitialized();
    }

    s_impl = std::make_unique<AudioEngineImpl>();
    return s_impl->init();
}

void AudioEngine::shutdown() {
    if (s_impl) {
        s_impl->shutdown();
        s_impl.reset();
    }
}

void AudioEngine::update(float deltaTime) {
    if (s_impl) {
        s_impl->update(deltaTime);
    }
}

bool AudioEngine::isInitialized() {
    return s_impl && s_impl->isInitialized();
}

AudioID AudioEngine::play2d(const std::string& filePath, bool loop, float volume, const AudioProfile* profile) {
    if (!s_impl) return AUDIO_ID_INVALID;
    return s_impl->play2d(filePath, loop, volume, profile);
}

AudioID AudioEngine::play3d(const std::string& filePath, const Audio3DConfig& config,
                            bool loop, float volume, const AudioProfile* profile) {
    if (!s_impl) return AUDIO_ID_INVALID;
    return s_impl->play3d(filePath, config, loop, volume, profile);
}

void AudioEngine::stop(AudioID audioID) {
    if (s_impl) s_impl->stop(audioID);
}

void AudioEngine::stopAll() {
    if (s_impl) s_impl->stopAll();
}

void AudioEngine::pause(AudioID audioID) {
    if (s_impl) s_impl->pause(audioID);
}

void AudioEngine::pauseAll() {
    if (s_impl) s_impl->pauseAll();
}

void AudioEngine::resume(AudioID audioID) {
    if (s_impl) s_impl->resume(audioID);
}

void AudioEngine::resumeAll() {
    if (s_impl) s_impl->resumeAll();
}

void AudioEngine::setVolume(AudioID audioID, float volume) {
    if (s_impl) s_impl->setVolume(audioID, volume);
}

float AudioEngine::getVolume(AudioID audioID) {
    return s_impl ? s_impl->getVolume(audioID) : 0.0f;
}

void AudioEngine::setLoop(AudioID audioID, bool loop) {
    if (s_impl) s_impl->setLoop(audioID, loop);
}

bool AudioEngine::isLoop(AudioID audioID) {
    return s_impl ? s_impl->isLoop(audioID) : false;
}

void AudioEngine::setPitch(AudioID audioID, float pitch) {
    if (s_impl) s_impl->setPitch(audioID, pitch);
}

float AudioEngine::getPitch(AudioID audioID) {
    return s_impl ? s_impl->getPitch(audioID) : 1.0f;
}

float AudioEngine::getCurrentTime(AudioID audioID) {
    return s_impl ? s_impl->getCurrentTime(audioID) : 0.0f;
}

void AudioEngine::setCurrentTime(AudioID audioID, float time) {
    if (s_impl) s_impl->setCurrentTime(audioID, time);
}

float AudioEngine::getDuration(AudioID audioID) {
    return s_impl ? s_impl->getDuration(audioID) : 0.0f;
}

AudioState AudioEngine::getState(AudioID audioID) {
    return s_impl ? s_impl->getState(audioID) : AudioState::Error;
}

void AudioEngine::setPosition3d(AudioID audioID, const glm::vec3& position) {
    if (s_impl) s_impl->setPosition3d(audioID, position);
}

glm::vec3 AudioEngine::getPosition3d(AudioID audioID) {
    return s_impl ? s_impl->getPosition3d(audioID) : glm::vec3(0.0f);
}

void AudioEngine::setVelocity3d(AudioID audioID, const glm::vec3& velocity) {
    if (s_impl) s_impl->setVelocity3d(audioID, velocity);
}

glm::vec3 AudioEngine::getVelocity3d(AudioID audioID) {
    return s_impl ? s_impl->getVelocity3d(audioID) : glm::vec3(0.0f);
}

void AudioEngine::setDirection3d(AudioID audioID, const glm::vec3& direction) {
    if (s_impl) s_impl->setDirection3d(audioID, direction);
}

void AudioEngine::setDistanceParameters(AudioID audioID, float minDistance, float maxDistance, float rolloffFactor) {
    if (s_impl) s_impl->setDistanceParameters(audioID, minDistance, maxDistance, rolloffFactor);
}

void AudioEngine::setDistanceModel(AudioID audioID, DistanceModel model) {
    if (s_impl) s_impl->setDistanceModel(audioID, model);
}

void AudioEngine::setCone(AudioID audioID, float innerAngle, float outerAngle, float outerGain) {
    if (s_impl) s_impl->setCone(audioID, innerAngle, outerAngle, outerGain);
}

void AudioEngine::set3DConfig(AudioID audioID, const Audio3DConfig& config) {
    if (s_impl) s_impl->set3DConfig(audioID, config);
}

void AudioEngine::setListenerPosition(const glm::vec3& position) {
    if (s_impl) s_impl->setListenerPosition(position);
}

glm::vec3 AudioEngine::getListenerPosition() {
    return s_impl ? s_impl->getListenerPosition() : glm::vec3(0.0f);
}

void AudioEngine::setListenerVelocity(const glm::vec3& velocity) {
    if (s_impl) s_impl->setListenerVelocity(velocity);
}

void AudioEngine::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    if (s_impl) s_impl->setListenerOrientation(forward, up);
}

void AudioEngine::setListener(const AudioListener& listener) {
    if (s_impl) s_impl->setListener(listener);
}

const AudioListener& AudioEngine::getListener() {
    static AudioListener defaultListener;
    return s_impl ? s_impl->getListener() : defaultListener;
}

void AudioEngine::setMasterVolume(float volume) {
    if (s_impl) s_impl->setMasterVolume(volume);
}

float AudioEngine::getMasterVolume() {
    return s_impl ? s_impl->getMasterVolume() : 1.0f;
}

void AudioEngine::setGlobalDistanceModel(DistanceModel model) {
    if (s_impl) s_impl->setGlobalDistanceModel(model);
}

void AudioEngine::setSpeedOfSound(float speed) {
    if (s_impl) s_impl->setSpeedOfSound(speed);
}

void AudioEngine::setDopplerFactor(float factor) {
    if (s_impl) s_impl->setDopplerFactor(factor);
}

void AudioEngine::preload(const std::string& filePath, std::function<void(bool)> callback) {
    if (s_impl) s_impl->preload(filePath, std::move(callback));
}

void AudioEngine::uncache(const std::string& filePath) {
    if (s_impl) s_impl->uncache(filePath);
}

void AudioEngine::uncacheAll() {
    if (s_impl) s_impl->uncacheAll();
}

bool AudioEngine::isPreloaded(const std::string& filePath) {
    return s_impl ? s_impl->isPreloaded(filePath) : false;
}

void AudioEngine::setFinishCallback(AudioID audioID, std::function<void(AudioID, const std::string&)> callback) {
    if (s_impl) s_impl->setFinishCallback(audioID, std::move(callback));
}

int AudioEngine::getPlayingAudioCount() {
    return s_impl ? s_impl->getPlayingAudioCount() : 0;
}

std::vector<AudioID> AudioEngine::getPlayingAudioIDs() {
    return s_impl ? s_impl->getPlayingAudioIDs() : std::vector<AudioID>{};
}

int AudioEngine::getMaxAudioInstances() {
    return MAX_AUDIO_INSTANCES;
}

std::string AudioEngine::getFilePath(AudioID audioID) {
    return s_impl ? s_impl->getFilePath(audioID) : "";
}

// ============================================================
// AudioManager Implementation
// ============================================================

AudioManager::AudioManager() = default;

AudioManager::~AudioManager() {
    if (m_initialized) {
        shutdown();
    }
}

bool AudioManager::init() {
    if (m_initialized) {
        return true;
    }

    if (AudioEngine::init()) {
        m_initialized = true;
        return true;
    }

    return false;
}

void AudioManager::shutdown() {
    if (!m_initialized) {
        return;
    }

    AudioEngine::shutdown();
    m_initialized = false;
}

void AudioManager::update(float deltaTime) {
    if (m_initialized) {
        AudioEngine::update(deltaTime);
    }
}

} // namespace Vapor
