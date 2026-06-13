#include "video_player.hpp"
#include <fmt/core.h>

#ifdef VAPOR_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace Vapor {

// ─── FFmpegDecodeContext ──────────────────────────────────────────────────────

struct VideoPlayer::FFmpegDecodeContext {
#ifdef VAPOR_HAS_FFMPEG
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int videoStreamIdx = -1;
    double timeBase = 0.0; // stream time_base as seconds per tick
#endif
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() {
    close();
}

bool VideoPlayer::open(const std::string& path) {
#ifndef VAPOR_HAS_FFMPEG
    fmt::print(stderr, "[VideoPlayer] Built without FFmpeg support\n");
    return false;
#else
    close();

    m_ffmpeg = std::make_unique<FFmpegDecodeContext>();
    if (!initDecoder(path)) {
        cleanup();
        return false;
    }

    m_currentTime = 0.0;
    m_hasCurrentFrame = false;
    m_stop = false;
    m_finished = false;
    m_open = true;

    m_decodeThread = std::thread(&VideoPlayer::decodeThreadFunc, this);
    return true;
#endif
}

void VideoPlayer::close() {
    if (!m_open) {
        return;
    }

    m_stop = true;
    m_cv.notify_all();

    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }

    cleanup();

    m_open = false;
    m_playing = false;
    m_finished = false;
    m_currentTime = 0.0;
    m_duration = 0.0;
    m_hasCurrentFrame = false;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameQueue.clear();
}

void VideoPlayer::play() {
    m_playing = true;
}

void VideoPlayer::pause() {
    m_playing = false;
}

// ─── Main-thread update ───────────────────────────────────────────────────────

bool VideoPlayer::update(double deltaTime) {
    if (!m_open || !m_playing) {
        return false;
    }

    m_currentTime += deltaTime;

    bool frameChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Consume every queued frame whose PTS has passed; keep the latest.
        while (!m_frameQueue.empty() && m_frameQueue.front().pts <= m_currentTime) {
            m_currentFrame = std::move(m_frameQueue.front());
            m_frameQueue.pop_front();
            m_hasCurrentFrame = true;
            frameChanged = true;
        }
    }

    // Wake the decode thread now that queue space may have freed up.
    m_cv.notify_one();

    // Stop the clock at the end of the video.
    if (m_finished && frameChanged) {
        bool queueEmpty;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            queueEmpty = m_frameQueue.empty();
        }
        if (queueEmpty) {
            m_playing = false;
        }
    }

    return frameChanged;
}

const VideoPlayer::Frame* VideoPlayer::getCurrentFrame() const {
    return m_hasCurrentFrame ? &m_currentFrame : nullptr;
}

// ─── Decode thread ────────────────────────────────────────────────────────────

void VideoPlayer::decodeThreadFunc() {
#ifdef VAPOR_HAS_FFMPEG
    auto& ff = *m_ffmpeg;

    while (!m_stop) {
        // Block when the frame queue is full.
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_frameQueue.size() < MAX_BUFFERED_FRAMES || m_stop.load();
            });
        }
        if (m_stop) {
            break;
        }

        int ret = av_read_frame(ff.fmtCtx, ff.packet);
        if (ret == AVERROR_EOF) {
            // Flush decoder
            avcodec_send_packet(ff.codecCtx, nullptr);
            while (avcodec_receive_frame(ff.codecCtx, ff.frame) >= 0) {
                convertAndEnqueue(ff.frame);
            }
            m_finished = true;
            break;
        }
        if (ret < 0) {
            break;
        }

        if (ff.packet->stream_index != ff.videoStreamIdx) {
            av_packet_unref(ff.packet);
            continue;
        }

        avcodec_send_packet(ff.codecCtx, ff.packet);
        av_packet_unref(ff.packet);

        while (avcodec_receive_frame(ff.codecCtx, ff.frame) >= 0) {
            if (!convertAndEnqueue(ff.frame)) {
                break;
            }
        }
    }
#endif
}

// ─── FFmpeg helpers ───────────────────────────────────────────────────────────

bool VideoPlayer::initDecoder(const std::string& path) {
#ifndef VAPOR_HAS_FFMPEG
    return false;
#else
    auto& ff = *m_ffmpeg;

    if (avformat_open_input(&ff.fmtCtx, path.c_str(), nullptr, nullptr) < 0) {
        fmt::print(stderr, "[VideoPlayer] Cannot open '{}'\n", path);
        return false;
    }
    if (avformat_find_stream_info(ff.fmtCtx, nullptr) < 0) {
        fmt::print(stderr, "[VideoPlayer] Cannot read stream info from '{}'\n", path);
        return false;
    }

    // Find the best video stream (prefers AV1 but accepts any)
    const AVCodec* codec = nullptr;
    ff.videoStreamIdx = av_find_best_stream(ff.fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ff.videoStreamIdx < 0 || !codec) {
        fmt::print(stderr, "[VideoPlayer] No video stream found in '{}'\n", path);
        return false;
    }

    AVStream* stream = ff.fmtCtx->streams[ff.videoStreamIdx];
    ff.timeBase = av_q2d(stream->time_base);

    if (ff.fmtCtx->duration != AV_NOPTS_VALUE) {
        m_duration = static_cast<double>(ff.fmtCtx->duration) / AV_TIME_BASE;
    }

    ff.codecCtx = avcodec_alloc_context3(codec);
    if (!ff.codecCtx) {
        return false;
    }
    avcodec_parameters_to_context(ff.codecCtx, stream->codecpar);

    if (avcodec_open2(ff.codecCtx, codec, nullptr) < 0) {
        fmt::print(stderr, "[VideoPlayer] Failed to open decoder for '{}'\n", path);
        return false;
    }

    ff.frame = av_frame_alloc();
    ff.packet = av_packet_alloc();

    int w = ff.codecCtx->width;
    int h = ff.codecCtx->height;
    ff.swsCtx = sws_getContext(w, h, ff.codecCtx->pix_fmt,
                               w, h, AV_PIX_FMT_RGBA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ff.swsCtx) {
        fmt::print(stderr, "[VideoPlayer] Failed to create colour converter\n");
        return false;
    }

    fmt::print("[VideoPlayer] Opened '{}' — {}x{} {:.1f}s ({})\n",
               path, w, h, m_duration, codec->name);
    return true;
#endif
}

bool VideoPlayer::convertAndEnqueue(void* avframe_ptr) {
#ifndef VAPOR_HAS_FFMPEG
    return false;
#else
    auto& ff = *m_ffmpeg;
    auto* avframe = static_cast<AVFrame*>(avframe_ptr);

    int w = avframe->width;
    int h = avframe->height;

    Frame decoded;
    decoded.pixels.resize(static_cast<size_t>(w) * h * 4);
    decoded.width = static_cast<uint32_t>(w);
    decoded.height = static_cast<uint32_t>(h);
    decoded.pts = (avframe->pts != AV_NOPTS_VALUE)
                      ? static_cast<double>(avframe->pts) * ff.timeBase
                      : 0.0;

    uint8_t* dstPlanes[1] = {decoded.pixels.data()};
    int dstStrides[1] = {w * 4};
    sws_scale(ff.swsCtx, avframe->data, avframe->linesize, 0, h, dstPlanes, dstStrides);

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // Wait if full (main thread already holds at MAX_BUFFERED_FRAMES - 1 or fewer).
        m_cv.wait(lock, [this] {
            return m_frameQueue.size() < MAX_BUFFERED_FRAMES || m_stop.load();
        });
        if (m_stop) {
            return false;
        }
        m_frameQueue.push_back(std::move(decoded));
    }
    return true;
#endif
}

void VideoPlayer::cleanup() {
#ifdef VAPOR_HAS_FFMPEG
    if (!m_ffmpeg) {
        return;
    }
    auto& ff = *m_ffmpeg;

    if (ff.swsCtx) {
        sws_freeContext(ff.swsCtx);
        ff.swsCtx = nullptr;
    }
    if (ff.frame) {
        av_frame_free(&ff.frame);
    }
    if (ff.packet) {
        av_packet_free(&ff.packet);
    }
    if (ff.codecCtx) {
        avcodec_free_context(&ff.codecCtx);
    }
    if (ff.fmtCtx) {
        avformat_close_input(&ff.fmtCtx);
    }

    m_ffmpeg.reset();
#endif
}

} // namespace Vapor
