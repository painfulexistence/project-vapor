#include "Vapor/audio_engine.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "Vapor/file_system.hpp"
#include <fmt/core.h>

namespace Vapor {

    // Helper functions

    static auto toMiniaudioModel(DistanceModel model) -> ma_attenuation_model {
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

    // AudioEngine Implementation

    AudioEngine::AudioEngine() = default;

    AudioEngine::~AudioEngine() {
        if (m_initialized) {
            shutdown();
        }
    }

    auto AudioEngine::init() -> bool {
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
        fmt::print("AudioEngine initialized\n");
        return true;
    }

    void AudioEngine::shutdown() {
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
        fmt::print("AudioEngine shutdown\n");
    }

    void AudioEngine::update(float deltaTime) {
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
                            m_pendingCallbacks.push_back({ std::move(inst.finishCallback), inst.id, inst.filePath });
                            inst.finishCallback = nullptr;
                        }

                        cleanupInstance(inst);
                    }
                }
            }
        }

        // Invoke callbacks outside the lock (safe for user to call AudioEngine methods)
        for (auto& pending : m_pendingCallbacks) {
            if (pending.callback) {
                pending.callback(pending.id, pending.filePath);
            }
        }
        m_pendingCallbacks.clear();
    }

    void AudioEngine::cleanupInstance(AudioInstance& inst) {
        // Must be called with lock held
        if (inst.sound) {
            ma_sound_uninit(inst.sound);
            delete inst.sound;
            inst.sound = nullptr;
        }
    }

    // Playback

    auto AudioEngine::play2d(const std::string& filename, bool loop, float volume) -> AudioID {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return AUDIO_ID_INVALID;

        AudioID id = allocateInstance();
        if (id == AUDIO_ID_INVALID) return AUDIO_ID_INVALID;

        auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
        inst.sound = new ma_sound();

        auto resolvedAudio = FileSystem::instance().resolvePath(filename);
        if (!resolvedAudio) {
            fmt::print("Audio file not found in any search path: {}\n", filename);
            return AUDIO_ID_INVALID;
        }
        std::string filePath = *resolvedAudio;
        if (ma_sound_init_from_file(m_engine, filePath.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, inst.sound)
            != MA_SUCCESS) {
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

    auto AudioEngine::play3d(const std::string& filename, const glm::vec3& position, bool loop, float volume)
        -> AudioID {
        return play3d(filename, Audio3DConfig(position), loop, volume);
    }

    auto AudioEngine::play3d(const std::string& filename, const Audio3DConfig& config, bool loop, float volume)
        -> AudioID {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return AUDIO_ID_INVALID;

        AudioID id = allocateInstance();
        if (id == AUDIO_ID_INVALID) return AUDIO_ID_INVALID;

        auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
        inst.sound = new ma_sound();

        auto resolvedAudio = FileSystem::instance().resolvePath(filename);
        if (!resolvedAudio) {
            fmt::print("Audio file not found in any search path: {}\n", filename);
            return AUDIO_ID_INVALID;
        }
        std::string filePath = *resolvedAudio;
        if (ma_sound_init_from_file(m_engine, filePath.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, inst.sound)
            != MA_SUCCESS) {
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

        ma_sound_set_cone(
            inst.sound,
            config.coneInnerAngle * (MA_PI / 180.0f),
            config.coneOuterAngle * (MA_PI / 180.0f),
            config.coneOuterGain
        );

        ma_sound_start(inst.sound);
        return id;
    }

    // Playback Control

    void AudioEngine::stop(AudioID id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst) return;

        ma_sound_stop(inst->sound);
        inst->state = AudioState::Stopped;
        inst->finishCallback = nullptr;
        cleanupInstance(*inst);
    }

    void AudioEngine::stopAll() {
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

    void AudioEngine::pause(AudioID id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst || inst->state != AudioState::Playing) return;

        ma_sound_stop(inst->sound);
        inst->state = AudioState::Paused;
    }

    void AudioEngine::pauseAll() {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& inst : m_instances) {
            if (inst.sound && inst.state == AudioState::Playing) {
                ma_sound_stop(inst.sound);
                inst.state = AudioState::Paused;
            }
        }
    }

    void AudioEngine::resume(AudioID id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst || inst->state != AudioState::Paused) return;

        ma_sound_start(inst->sound);
        inst->state = AudioState::Playing;
    }

    void AudioEngine::resumeAll() {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& inst : m_instances) {
            if (inst.sound && inst.state == AudioState::Paused) {
                ma_sound_start(inst.sound);
                inst.state = AudioState::Playing;
            }
        }
    }

    // Audio Properties

    void AudioEngine::setVolume(AudioID id, float volume) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst) return;

        inst->volume = volume;
        ma_sound_set_volume(inst->sound, volume * m_masterVolume);
    }

    auto AudioEngine::getVolume(AudioID id) const -> float {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        return inst ? inst->volume : 0.0f;
    }

    void AudioEngine::setLoop(AudioID id, bool loop) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (inst) ma_sound_set_looping(inst->sound, loop);
    }

    auto AudioEngine::isLoop(AudioID id) const -> bool {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        return inst ? ma_sound_is_looping(inst->sound) : false;
    }

    void AudioEngine::setPitch(AudioID id, float pitch) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (inst) ma_sound_set_pitch(inst->sound, pitch);
    }

    auto AudioEngine::getPitch(AudioID id) const -> float {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        return inst ? ma_sound_get_pitch(inst->sound) : 1.0f;
    }

    auto AudioEngine::getCurrentTime(AudioID id) const -> float {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst) return 0.0f;

        ma_uint64 cursor;
        ma_sound_get_cursor_in_pcm_frames(inst->sound, &cursor);
        return static_cast<float>(cursor) / ma_engine_get_sample_rate(m_engine);
    }

    void AudioEngine::setCurrentTime(AudioID id, float time) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst) return;

        auto frame = static_cast<ma_uint64>(time * ma_engine_get_sample_rate(m_engine));
        ma_sound_seek_to_pcm_frame(inst->sound, frame);
    }

    auto AudioEngine::getDuration(AudioID id) const -> float {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst) return 0.0f;

        float length = 0.0f;
        ma_sound_get_length_in_seconds(inst->sound, &length);
        return length;
    }

    auto AudioEngine::getState(AudioID id) const -> AudioState {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        return inst ? inst->state : AudioState::Error;
    }

    // 3D Audio Source Control

    void AudioEngine::setPosition3d(AudioID id, const glm::vec3& position) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst || !inst->is3D) return;

        inst->config3D.position = position;
        ma_sound_set_position(inst->sound, position.x, position.y, position.z);
    }

    auto AudioEngine::getPosition3d(AudioID id) const -> glm::vec3 {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        return inst ? inst->config3D.position : glm::vec3(0.0f);
    }

    void AudioEngine::setVelocity3d(AudioID id, const glm::vec3& velocity) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst || !inst->is3D) return;

        inst->config3D.velocity = velocity;
        ma_sound_set_velocity(inst->sound, velocity.x, velocity.y, velocity.z);
    }

    void AudioEngine::setDirection3d(AudioID id, const glm::vec3& direction) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst || !inst->is3D) return;

        inst->config3D.direction = direction;
        ma_sound_set_direction(inst->sound, direction.x, direction.y, direction.z);
    }

    void AudioEngine::setDistanceParameters(AudioID id, float minDist, float maxDist, float rolloff) {
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

    void AudioEngine::setDistanceModel(AudioID id, DistanceModel model) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst || !inst->is3D) return;

        inst->config3D.distanceModel = model;
        ma_sound_set_attenuation_model(inst->sound, toMiniaudioModel(model));
    }

    void AudioEngine::setCone(AudioID id, float innerAngle, float outerAngle, float outerGain) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (!inst || !inst->is3D) return;

        inst->config3D.coneInnerAngle = innerAngle;
        inst->config3D.coneOuterAngle = outerAngle;
        inst->config3D.coneOuterGain = outerGain;

        ma_sound_set_cone(inst->sound, innerAngle * (MA_PI / 180.0f), outerAngle * (MA_PI / 180.0f), outerGain);
    }

    void AudioEngine::set3DConfig(AudioID id, const Audio3DConfig& config) {
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

        ma_sound_set_cone(
            inst->sound,
            config.coneInnerAngle * (MA_PI / 180.0f),
            config.coneOuterAngle * (MA_PI / 180.0f),
            config.coneOuterGain
        );
    }

    // Listener Control

    void AudioEngine::setListenerPosition(const glm::vec3& position) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return;
        m_listener.position = position;
        ma_engine_listener_set_position(m_engine, 0, position.x, position.y, position.z);
    }

    void AudioEngine::setListenerVelocity(const glm::vec3& velocity) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return;
        m_listener.velocity = velocity;
        ma_engine_listener_set_velocity(m_engine, 0, velocity.x, velocity.y, velocity.z);
    }

    void AudioEngine::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return;
        m_listener.forward = forward;
        m_listener.up = up;
        ma_engine_listener_set_direction(m_engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(m_engine, 0, up.x, up.y, up.z);
    }

    void AudioEngine::setListener(const AudioListener& listener) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return;
        m_listener = listener;
        ma_engine_listener_set_position(m_engine, 0, listener.position.x, listener.position.y, listener.position.z);
        ma_engine_listener_set_velocity(m_engine, 0, listener.velocity.x, listener.velocity.y, listener.velocity.z);
        ma_engine_listener_set_direction(m_engine, 0, listener.forward.x, listener.forward.y, listener.forward.z);
        ma_engine_listener_set_world_up(m_engine, 0, listener.up.x, listener.up.y, listener.up.z);
    }

    // Global Settings

    void AudioEngine::setMasterVolume(float volume) {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_masterVolume = volume;
        if (m_initialized) {
            ma_engine_set_volume(m_engine, volume);
        }
    }

    void AudioEngine::setDopplerFactor(float factor) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) return;
        for (auto& inst : m_instances) {
            if (inst.sound && inst.is3D) {
                ma_sound_set_doppler_factor(inst.sound, factor);
            }
        }
    }

    // Callbacks & Utility

    void AudioEngine::setFinishCallback(AudioID id, std::function<void(AudioID, const std::string&)> callback) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        if (inst) inst->finishCallback = std::move(callback);
    }

    auto AudioEngine::getPlayingCount() const -> int {
        std::lock_guard<std::mutex> lock(m_mutex);

        int count = 0;
        for (const auto& inst : m_instances) {
            if (inst.sound && inst.state == AudioState::Playing) count++;
        }
        return count;
    }

    auto AudioEngine::getFilePath(AudioID id) const -> std::string {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto* inst = getInstance(id);
        return inst ? inst->filePath : "";
    }

    // Private helpers (must be called with lock held)

    auto AudioEngine::getInstance(AudioID id) -> AudioEngine::AudioInstance* {
        if (id == AUDIO_ID_INVALID) return nullptr;
        auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
        return (inst.sound && inst.id == id) ? &inst : nullptr;
    }

    auto AudioEngine::getInstance(AudioID id) const -> const AudioEngine::AudioInstance* {
        if (id == AUDIO_ID_INVALID) return nullptr;
        const auto& inst = m_instances[id % MAX_AUDIO_INSTANCES];
        return (inst.sound && inst.id == id) ? &inst : nullptr;
    }

    auto AudioEngine::allocateInstance() -> AudioID {
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

}// namespace Vapor
