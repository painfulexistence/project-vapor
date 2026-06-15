#pragma once

#include "renderer.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Vapor {

// Records rendered frames to an AV1 video file via FFmpeg.
// Usage:
//   VideoRecorder rec;
//   rec.startRecording(renderer, {.outputPath = "output.mkv", .fps = 60});
//   // each frame:
//   rec.captureFrame();
//   // when done:
//   rec.stopRecording();
class VideoRecorder {
public:
    struct Config {
        std::string outputPath = "recording.mp4";
        int fps = 60;
        // AV1 CRF quality (0=lossless, 63=worst). Lower = better quality.
        int crf = 35;
        // FFmpeg encoder name. "libsvtav1" is fastest; fallback to "libaom-av1".
        std::string encoder = "libsvtav1";
    };

    VideoRecorder();
    ~VideoRecorder();

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

private:
    struct RawFrame {
        std::vector<uint8_t> pixels;
        uint32_t width;
        uint32_t height;
    };

    // FFmpegContext hides all FFmpeg types from this header.
    struct FFmpegContext;

    bool initEncoder(uint32_t width, uint32_t height);
    bool encodeFrame(const RawFrame& frame);
    void flushEncoder();
    void cleanup();
    void encoderThreadFunc();

    Renderer* m_renderer = nullptr;
    Config m_config;

    std::unique_ptr<FFmpegContext> m_ffmpeg;

    std::queue<RawFrame> m_frameQueue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_encoderThread;

    std::atomic<bool> m_recording{false};
    std::atomic<bool> m_stop{false};

    static constexpr size_t MAX_QUEUE_FRAMES = 16;
};

} // namespace Vapor
