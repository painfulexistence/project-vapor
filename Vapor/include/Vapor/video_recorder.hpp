#pragma once

#include "audio_engine.hpp"
#include "renderer.hpp"
#include "imgui.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fmt/core.h>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Vapor {

// Records rendered frames to an H.264/AV1 video file via FFmpeg, optionally
// muxing the engine's mixed audio output into an AAC track.
//
// Usage:
//   VideoRecorder rec;
//   rec.setAudioEngine(&engineCore.getAudioEngine()); // optional, for audio
//   rec.startRecording(renderer, {.outputPath = "output.mp4", .fps = 30});
//   // each frame:
//   rec.captureFrame();
//   // when done:
//   rec.stopRecording();
//
// Audio is captured by registering as an AudioCaptureSink on the AudioEngine,
// which delivers the final mixed PCM on the audio thread. The encoder thread
// resamples it to the AAC encoder format and interleaves it with the video.
class VideoRecorder : public AudioCaptureSink {
public:
    struct Config {
        std::string outputPath = "recording.mp4";
        int fps = 30;
        // CRF quality for SW AV1 encoders (0=lossless, 63=worst). Lower = better.
        // Ignored for HW encoders (VideoToolbox, NVENC), which use fixed quality.
        int crf = 35;
        // Encoder probe order: h264_videotoolbox → h264_nvenc → libsvtav1 →
        // libaom-av1 → libx264. Set to empty string to use the probe order directly.
        std::string encoder = "h264_videotoolbox";
        // Capture the engine's mixed audio into an AAC track. Requires an
        // AudioEngine to be set via setAudioEngine(); silently video-only
        // otherwise.
        bool captureAudio = true;
        // AAC bitrate in bits/sec.
        int audioBitrate = 128000;
    };

    VideoRecorder();
    ~VideoRecorder() override;

    // Provide the AudioEngine whose mixed output should be recorded. Required
    // for audio capture — without this, captureAudio in Config has no effect.
    // Call once after construction, before startRecording().
    void setAudioEngine(AudioEngine* audioEngine) { m_audioEngine = audioEngine; }

    // Start recording. Returns false if already recording or FFmpeg unavailable.
    // (Config is a nested type with default member initializers, so we use an
    // overload rather than a `= {}` default argument, which is ill-formed inside
    // the enclosing class definition.)
    bool startRecording(Renderer* renderer, const Config& config);
    bool startRecording(Renderer* renderer) {
        return startRecording(renderer, Config{});
    }

    // Stop recording and flush remaining frames to disk.
    void stopRecording();

    bool isRecording() const { return m_recording.load(); }

    // Schedule capture of the current rendered frame. Call once per frame.
    // Drops frames silently if the encoder queue is full.
    void captureFrame();

    // AudioCaptureSink — called on the audio thread with the engine's mixed
    // output while recording. Buffers PCM for the encoder thread to consume.
    void writeAudio(const float* frames, uint32_t frameCount) override;

    // Set the directory where timestamped recordings are saved.
    void setBaseOutputDir(const std::string& dir) {
        m_baseOutputDir = dir;
        refreshOutputPath();
    }

    // Draw the recording section inside whatever ImGui window is currently open.
    void drawImGui(Renderer& renderer) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##recpath", m_outputBuf, sizeof(m_outputBuf));

        if (!isRecording()) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
            if (ImGui::Button("Start##rec", ImVec2(-1.0f, 0.0f))) {
                std::error_code ec;
                std::filesystem::create_directories(
                    std::filesystem::path(m_outputBuf).parent_path(), ec);
                Config cfg;
                cfg.outputPath = m_outputBuf;
                if (startRecording(&renderer, cfg))
                    m_status.clear();
                else
                    m_status = "Failed to start (FFmpeg unavailable?)";
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Stop##rec", ImVec2(-1.0f, 0.0f))) {
                stopRecording();
                m_status = fmt::format("Saved: {}", m_outputBuf);
                refreshOutputPath();
            }
            ImGui::PopStyleColor();
            auto elapsed = std::chrono::steady_clock::now() - m_recordingStart;
            auto secs    = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "REC  %02d:%02d", (int)(secs / 60), (int)(secs % 60));
        }
        if (!m_status.empty())
            ImGui::TextDisabled("%s", m_status.c_str());
    }

private:
    struct RawFrame {
        std::vector<uint8_t> pixels;
        uint32_t width;
        uint32_t height;
        double timestamp; // seconds since recording started (wall-clock)
    };

    // FFmpegContext hides all FFmpeg types from this header.
    struct FFmpegContext;

    bool initEncoder(uint32_t width, uint32_t height);
    bool encodeFrame(const RawFrame& frame);
    void flushEncoder();
    void cleanup();
    void encoderThreadFunc();

    // Audio pipeline (no-ops when audio capture is inactive).
    bool initAudioEncoder();     // called from initEncoder, before write_header
    void drainAudio(bool flush); // resample queued PCM → AAC → mux
    void encodeAudioFrame(int nbSamples);
    void flushAudioEncoder();

    // UI state (only used when drawImGui() is called)
    std::string m_baseOutputDir = "output";
    char        m_outputBuf[256] = {};
    std::string m_status;

    void refreshOutputPath() {
        auto now  = std::chrono::system_clock::now();
        auto t    = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char filename[64];
        std::strftime(filename, sizeof(filename), "recording_%Y%m%d_%H%M%S.mp4", &tm);
        auto path = (std::filesystem::path(m_baseOutputDir) / filename).string();
        strncpy(m_outputBuf, path.c_str(), sizeof(m_outputBuf) - 1);
        m_outputBuf[sizeof(m_outputBuf) - 1] = '\0';
    }

    Renderer* m_renderer = nullptr;
    Config m_config;
    std::chrono::steady_clock::time_point m_recordingStart;

    std::unique_ptr<FFmpegContext> m_ffmpeg;

    std::queue<RawFrame> m_frameQueue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_encoderThread;

    std::atomic<bool> m_recording{false};
    std::atomic<bool> m_stop{false};

    static constexpr size_t MAX_QUEUE_FRAMES = 16;

    // ── Audio capture state ──────────────────────────────────────────────
    AudioEngine* m_audioEngine = nullptr;
    bool m_audioActive = false; // audio track is being recorded this session
    int  m_audioSampleRate = 44100;
    int  m_audioChannels = 2;

    // Interleaved float PCM produced by the audio thread, consumed by the
    // encoder thread. Guarded by m_audioMutex; bounded to a few seconds.
    std::deque<float> m_audioQueue;
    std::mutex        m_audioMutex;
};

} // namespace Vapor
