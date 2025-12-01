#include "Vapor/audio_engine.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <SDL3/SDL_filesystem.h>
#include <fmt/core.h>

namespace Vapor {

// ============================================================
// Helper functions
// ============================================================

static ma_attenuation_model toMiniaudioModel(DistanceModel model) {
    switch (model) {
        case DistanceModel::None:        return ma_attenuation_model_none;
        case DistanceModel::Linear:      return ma_attenuation_model_linear;
        case DistanceModel::Inverse:     return ma_attenuation_model_inverse;
        case DistanceModel::Exponential: return ma_attenuation_model_exponential;
        default:                         return ma_attenuation_model_inverse;
    }
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
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized) {
        return true;
    }

    m_engine = new ma_engine();

    ma_engine_config config = ma_engine_config_init();
    config.channels = 2;
    config.sampleRate = 44100;
    config.listenerCount = 1;

    if (ma_engine_init(&config, m_engine) != MA_SUCCESS) {
        fmt::print("Failed to initialize audio engine\n");
        delete m_engine;
        m_engine = nullptr;
        return false;
    }

    m_initialized = true;
    fmt::print("AudioManager initialized\n");
    return true;
}

void AudioManager::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        return;
    }

    // Stop and cleanup all instances
    for (auto& inst : m_instances) {
        if (inst.sound) {
            ma_sound_stop(inst.sound);
            ma_sound_uninit(inst.sound);
            delete inst.sound;
            inst.sound = nullptr;
            inst.state = AudioState::Stopped;
            inst.finishCallback = nullptr;
        }
    }

    m_pendingCallbacks.clear();

    if (m_engine) {
        ma_engine_uninit(m_engine);
        delete m_engine;
        m_engine = nullptr;
    }

    m_initialized = false;
    fmt::print("AudioManager shutdown\n");
}

void AudioManager::update(float deltaTime) {
    // Collect finished sounds and their callbacks (under lock)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return;

        for (auto& inst : m_instances) {
            if (inst.sound && inst.state == AudioState::Playing) {
                if (ma_sound_at_end(inst.sound) && !ma_sound_is_looping(inst.sound)) {
                    inst.state = AudioState::Stopped;

                    // Queue callback for invocation outside the lock
                    if (inst.finishCallback) {
                        m_pendingCallbacks.push_back({
                            std::move(inst.finishCallback),
                            inst.id,
                            inst.filePath
                        });
                        inst.finishCallback = nullptr;
                    }

                    cleanupInstance(inst);
                }
            }
        }
    }

    // Invoke callbacks outside the lock (safe for user to call AudioManager methods)
    for (auto& pending : m_pendingCallbacks) {
        if (pending.callback) {
            pending.callback(pending.id, pending.filePath);
        }
    }
    m_pendingCallbacks.clear();
}

void AudioManager::cleanupInstance(AudioInstance& inst) {
    // Must be called with lock held
    if (inst.sound) {
        ma_sound_uninit(inst.sound);
        delete inst.sound;
        inst.sound = nullptr;
    }
}

// ============================================================
// Playback
// ============================================================

AudioID AudioManager::play2d(const std::string& filename, bool loop, float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return AUDIO_ID_INVALID;

    AudioID id = allocateInstance();
    if (id == AUDIO_ID_INVALID) return AUDIO_ID_INVALID;

    auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
    inst.sound = new ma_sound();

    std::string filePath = SDL_GetBasePath() + filename;
    if (ma_sound_init_from_file(m_engine, filePath.c_str(), MA_SOUND_FLAG_DECODE,
                                 nullptr, nullptr, inst.sound) != MA_SUCCESS) {
        fmt::print("Failed to load audio: {}\n", filename);
        delete inst.sound;
        inst.sound = nullptr;
        return AUDIO_ID_INVALID;
    }

    inst.id = id;
    inst.filePath = filePath;
    inst.is3D = false;
    inst.volume = volume;
    inst.state = AudioState::Playing;
    inst.finishCallback = nullptr;

    ma_sound_set_volume(inst.sound, volume * m_masterVolume);
    ma_sound_set_looping(inst.sound, loop);
    ma_sound_set_spatialization_enabled(inst.sound, MA_FALSE);
    ma_sound_start(inst.sound);

    return id;
}

AudioID AudioManager::play3d(const std::string& filename, const glm::vec3& position,
                              bool loop, float volume) {
    return play3d(filename, Audio3DConfig(position), loop, volume);
}

AudioID AudioManager::play3d(const std::string& filename, const Audio3DConfig& config,
                              bool loop, float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return AUDIO_ID_INVALID;

    AudioID id = allocateInstance();
    if (id == AUDIO_ID_INVALID) return AUDIO_ID_INVALID;

    auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
    inst.sound = new ma_sound();

    std::string filePath = SDL_GetBasePath() + filename;
    if (ma_sound_init_from_file(m_engine, filePath.c_str(), MA_SOUND_FLAG_DECODE,
                                 nullptr, nullptr, inst.sound) != MA_SUCCESS) {
        fmt::print("Failed to load audio: {}\n", filename);
        delete inst.sound;
        inst.sound = nullptr;
        return AUDIO_ID_INVALID;
    }

    inst.id = id;
    inst.filePath = filePath;
    inst.is3D = true;
    inst.volume = volume;
    inst.config3D = config;
    inst.state = AudioState::Playing;
    inst.finishCallback = nullptr;

    ma_sound_set_volume(inst.sound, volume * m_masterVolume);
    ma_sound_set_looping(inst.sound, loop);
    ma_sound_set_spatialization_enabled(inst.sound, MA_TRUE);

    // 3D settings
    ma_sound_set_position(inst.sound, config.position.x, config.position.y, config.position.z);
    ma_sound_set_velocity(inst.sound, config.velocity.x, config.velocity.y, config.velocity.z);
    ma_sound_set_direction(inst.sound, config.direction.x, config.direction.y, config.direction.z);

    ma_sound_set_attenuation_model(inst.sound, toMiniaudioModel(config.distanceModel));
    ma_sound_set_min_distance(inst.sound, config.minDistance);
    ma_sound_set_max_distance(inst.sound, config.maxDistance);
    ma_sound_set_rolloff(inst.sound, config.rolloffFactor);

    ma_sound_set_cone(inst.sound,
                      config.coneInnerAngle * (MA_PI / 180.0f),
                      config.coneOuterAngle * (MA_PI / 180.0f),
                      config.coneOuterGain);

    ma_sound_start(inst.sound);
    return id;
}

// ============================================================
// Playback Control
// ============================================================

void AudioManager::stop(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst) return;

    ma_sound_stop(inst->sound);
    inst->state = AudioState::Stopped;
    inst->finishCallback = nullptr;
    cleanupInstance(*inst);
}

void AudioManager::stopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& inst : m_instances) {
        if (inst.sound) {
            ma_sound_stop(inst.sound);
            inst.state = AudioState::Stopped;
            inst.finishCallback = nullptr;
            cleanupInstance(inst);
        }
    }
}

void AudioManager::pause(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || inst->state != AudioState::Playing) return;

    ma_sound_stop(inst->sound);
    inst->state = AudioState::Paused;
}

void AudioManager::pauseAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& inst : m_instances) {
        if (inst.sound && inst.state == AudioState::Playing) {
            ma_sound_stop(inst.sound);
            inst.state = AudioState::Paused;
        }
    }
}

void AudioManager::resume(AudioID id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || inst->state != AudioState::Paused) return;

    ma_sound_start(inst->sound);
    inst->state = AudioState::Playing;
}

void AudioManager::resumeAll() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& inst : m_instances) {
        if (inst.sound && inst.state == AudioState::Paused) {
            ma_sound_start(inst.sound);
            inst.state = AudioState::Playing;
        }
    }
}

// ============================================================
// Audio Properties
// ============================================================

void AudioManager::setVolume(AudioID id, float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst) return;

    inst->volume = volume;
    ma_sound_set_volume(inst->sound, volume * m_masterVolume);
}

float AudioManager::getVolume(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    return inst ? inst->volume : 0.0f;
}

void AudioManager::setLoop(AudioID id, bool loop) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (inst) ma_sound_set_looping(inst->sound, loop);
}

bool AudioManager::isLoop(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    return inst ? ma_sound_is_looping(inst->sound) : false;
}

void AudioManager::setPitch(AudioID id, float pitch) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (inst) ma_sound_set_pitch(inst->sound, pitch);
}

float AudioManager::getPitch(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    return inst ? ma_sound_get_pitch(inst->sound) : 1.0f;
}

float AudioManager::getCurrentTime(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst) return 0.0f;

    ma_uint64 cursor;
    ma_sound_get_cursor_in_pcm_frames(inst->sound, &cursor);
    return static_cast<float>(cursor) / ma_engine_get_sample_rate(m_engine);
}

void AudioManager::setCurrentTime(AudioID id, float time) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst) return;

    ma_uint64 frame = static_cast<ma_uint64>(time * ma_engine_get_sample_rate(m_engine));
    ma_sound_seek_to_pcm_frame(inst->sound, frame);
}

float AudioManager::getDuration(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst) return 0.0f;

    float length = 0.0f;
    ma_sound_get_length_in_seconds(inst->sound, &length);
    return length;
}

AudioState AudioManager::getState(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    return inst ? inst->state : AudioState::Error;
}

// ============================================================
// 3D Audio Source Control
// ============================================================

void AudioManager::setPosition3d(AudioID id, const glm::vec3& position) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || !inst->is3D) return;

    inst->config3D.position = position;
    ma_sound_set_position(inst->sound, position.x, position.y, position.z);
}

glm::vec3 AudioManager::getPosition3d(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    return inst ? inst->config3D.position : glm::vec3(0.0f);
}

void AudioManager::setVelocity3d(AudioID id, const glm::vec3& velocity) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || !inst->is3D) return;

    inst->config3D.velocity = velocity;
    ma_sound_set_velocity(inst->sound, velocity.x, velocity.y, velocity.z);
}

void AudioManager::setDirection3d(AudioID id, const glm::vec3& direction) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || !inst->is3D) return;

    inst->config3D.direction = direction;
    ma_sound_set_direction(inst->sound, direction.x, direction.y, direction.z);
}

void AudioManager::setDistanceParameters(AudioID id, float minDist, float maxDist, float rolloff) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || !inst->is3D) return;

    inst->config3D.minDistance = minDist;
    inst->config3D.maxDistance = maxDist;
    inst->config3D.rolloffFactor = rolloff;

    ma_sound_set_min_distance(inst->sound, minDist);
    ma_sound_set_max_distance(inst->sound, maxDist);
    ma_sound_set_rolloff(inst->sound, rolloff);
}

void AudioManager::setDistanceModel(AudioID id, DistanceModel model) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || !inst->is3D) return;

    inst->config3D.distanceModel = model;
    ma_sound_set_attenuation_model(inst->sound, toMiniaudioModel(model));
}

void AudioManager::setCone(AudioID id, float innerAngle, float outerAngle, float outerGain) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || !inst->is3D) return;

    inst->config3D.coneInnerAngle = innerAngle;
    inst->config3D.coneOuterAngle = outerAngle;
    inst->config3D.coneOuterGain = outerGain;

    ma_sound_set_cone(inst->sound,
                      innerAngle * (MA_PI / 180.0f),
                      outerAngle * (MA_PI / 180.0f),
                      outerGain);
}

void AudioManager::set3DConfig(AudioID id, const Audio3DConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (!inst || !inst->is3D) return;

    inst->config3D = config;

    ma_sound_set_position(inst->sound, config.position.x, config.position.y, config.position.z);
    ma_sound_set_velocity(inst->sound, config.velocity.x, config.velocity.y, config.velocity.z);
    ma_sound_set_direction(inst->sound, config.direction.x, config.direction.y, config.direction.z);

    ma_sound_set_attenuation_model(inst->sound, toMiniaudioModel(config.distanceModel));
    ma_sound_set_min_distance(inst->sound, config.minDistance);
    ma_sound_set_max_distance(inst->sound, config.maxDistance);
    ma_sound_set_rolloff(inst->sound, config.rolloffFactor);

    ma_sound_set_cone(inst->sound,
                      config.coneInnerAngle * (MA_PI / 180.0f),
                      config.coneOuterAngle * (MA_PI / 180.0f),
                      config.coneOuterGain);
}

// ============================================================
// Listener Control
// ============================================================

void AudioManager::setListenerPosition(const glm::vec3& position) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;
    m_listener.position = position;
    ma_engine_listener_set_position(m_engine, 0, position.x, position.y, position.z);
}

void AudioManager::setListenerVelocity(const glm::vec3& velocity) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;
    m_listener.velocity = velocity;
    ma_engine_listener_set_velocity(m_engine, 0, velocity.x, velocity.y, velocity.z);
}

void AudioManager::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;
    m_listener.forward = forward;
    m_listener.up = up;
    ma_engine_listener_set_direction(m_engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine, 0, up.x, up.y, up.z);
}

void AudioManager::setListener(const AudioListener& listener) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;
    m_listener = listener;
    ma_engine_listener_set_position(m_engine, 0, listener.position.x, listener.position.y, listener.position.z);
    ma_engine_listener_set_velocity(m_engine, 0, listener.velocity.x, listener.velocity.y, listener.velocity.z);
    ma_engine_listener_set_direction(m_engine, 0, listener.forward.x, listener.forward.y, listener.forward.z);
    ma_engine_listener_set_world_up(m_engine, 0, listener.up.x, listener.up.y, listener.up.z);
}

// ============================================================
// Global Settings
// ============================================================

void AudioManager::setMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_masterVolume = volume;
    if (m_initialized) {
        ma_engine_set_volume(m_engine, volume);
    }
}

void AudioManager::setDopplerFactor(float factor) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) return;
    for (auto& inst : m_instances) {
        if (inst.sound && inst.is3D) {
            ma_sound_set_doppler_factor(inst.sound, factor);
        }
    }
}

// ============================================================
// Callbacks & Utility
// ============================================================

void AudioManager::setFinishCallback(AudioID id, std::function<void(AudioID, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    if (inst) inst->finishCallback = std::move(callback);
}

int AudioManager::getPlayingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    int count = 0;
    for (const auto& inst : m_instances) {
        if (inst.sound && inst.state == AudioState::Playing) count++;
    }
    return count;
}

std::string AudioManager::getFilePath(AudioID id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* inst = getInstance(id);
    return inst ? inst->filePath : "";
}

// ============================================================
// Private helpers (must be called with lock held)
// ============================================================

AudioManager::AudioInstance* AudioManager::getInstance(AudioID id) {
    if (id == AUDIO_ID_INVALID) return nullptr;
    auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
    return (inst.sound && inst.id == id) ? &inst : nullptr;
}

const AudioManager::AudioInstance* AudioManager::getInstance(AudioID id) const {
    if (id == AUDIO_ID_INVALID) return nullptr;
    const auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
    return (inst.sound && inst.id == id) ? &inst : nullptr;
}

AudioID AudioManager::allocateInstance() {
    // Must be called with lock held
    for (int i = 0; i < MAX_AUDIO_INSTANCES; i++) {
        int idx = (m_nextID + i) % MAX_AUDIO_INSTANCES;
        if (!m_instances[idx].sound) {
            return m_nextID++;
        }
    }
    fmt::print("No available audio slots\n");
    return AUDIO_ID_INVALID;
}

} // namespace Vapor
