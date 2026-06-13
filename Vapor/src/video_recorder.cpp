#include "video_recorder.hpp"
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

// ─── FFmpegContext ────────────────────────────────────────────────────────────

struct VideoRecorder::FFmpegContext {
#ifdef VAPOR_HAS_FFMPEG
    AVCodecContext* codecCtx = nullptr;
    AVFormatContext* fmtCtx = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* swsCtx = nullptr;
    int64_t pts = 0;
#endif
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder() {
    if (m_recording) {
        stopRecording();
    }
}

bool VideoRecorder::startRecording(Renderer* renderer, const Config& config) {
#ifndef VAPOR_HAS_FFMPEG
    fmt::print(stderr, "[VideoRecorder] Built without FFmpeg support — recording unavailable\n");
    return false;
#else
    if (m_recording) {
        return false;
    }

    m_renderer = renderer;
    m_config = config;
    m_ffmpeg = std::make_unique<FFmpegContext>();
    m_stop = false;
    m_recording = true;

    m_encoderThread = std::thread(&VideoRecorder::encoderThreadFunc, this);
    return true;
#endif
}

void VideoRecorder::stopRecording() {
    if (!m_recording) {
        return;
    }

    m_stop = true;
    m_cv.notify_all();

    if (m_encoderThread.joinable()) {
        m_encoderThread.join();
    }

    m_recording = false;
    m_renderer = nullptr;
}

void VideoRecorder::captureFrame() {
    if (!m_recording) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frameQueue.size() >= MAX_QUEUE_FRAMES) {
            return; // encoder is lagging; drop this frame
        }
    }

    m_renderer->readPixelsAsync([this](const GpuImageData& data) {
        if (!m_recording) {
            return;
        }

        RawFrame frame;
        frame.pixels = data.data;
        frame.width = data.width;
        frame.height = data.height;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_frameQueue.size() < MAX_QUEUE_FRAMES) {
                m_frameQueue.push(std::move(frame));
            }
        }
        m_cv.notify_one();
    });
}

// ─── Encoder thread ───────────────────────────────────────────────────────────

void VideoRecorder::encoderThreadFunc() {
    bool encoderInitialized = false;

    while (true) {
        RawFrame frame;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // Wake on new frame OR stop; keep running until the queue is drained.
            m_cv.wait(lock, [this] { return !m_frameQueue.empty() || m_stop.load(); });

            if (m_frameQueue.empty()) {
                break; // stop signalled and queue is empty
            }

            frame = std::move(m_frameQueue.front());
            m_frameQueue.pop();
        }

        if (!encoderInitialized) {
            if (!initEncoder(frame.width, frame.height)) {
                fmt::print(stderr, "[VideoRecorder] Encoder initialization failed\n");
                m_recording = false;
                return;
            }
            encoderInitialized = true;
        }

        encodeFrame(frame);
    }

    if (encoderInitialized) {
        flushEncoder();
    }
    cleanup();
}

// ─── FFmpeg implementation ────────────────────────────────────────────────────

bool VideoRecorder::initEncoder(uint32_t width, uint32_t height) {
#ifndef VAPOR_HAS_FFMPEG
    return false;
#else
    auto& ff = *m_ffmpeg;

    // Try configured encoder, then fallbacks
    const AVCodec* codec = avcodec_find_encoder_by_name(m_config.encoder.c_str());
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libaom-av1");
    }
    if (!codec) {
        fmt::print(stderr, "[VideoRecorder] No AV1 encoder found (tried '{}', 'libaom-av1')\n",
                   m_config.encoder);
        return false;
    }

    if (avformat_alloc_output_context2(&ff.fmtCtx, nullptr, nullptr,
                                       m_config.outputPath.c_str()) < 0) {
        fmt::print(stderr, "[VideoRecorder] Failed to create output context for '{}'\n",
                   m_config.outputPath);
        return false;
    }

    ff.codecCtx = avcodec_alloc_context3(codec);
    if (!ff.codecCtx) {
        return false;
    }

    ff.codecCtx->width = static_cast<int>(width);
    ff.codecCtx->height = static_cast<int>(height);
    ff.codecCtx->time_base = AVRational{1, m_config.fps};
    ff.codecCtx->framerate = AVRational{m_config.fps, 1};
    ff.codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    ff.codecCtx->gop_size = m_config.fps; // one keyframe per second

    av_opt_set_int(ff.codecCtx->priv_data, "crf", m_config.crf, 0);

    // Speed presets: libsvtav1 uses "preset", libaom-av1 uses "cpu-used"
    const std::string enc = m_config.encoder;
    if (enc == "libsvtav1") {
        av_opt_set_int(ff.codecCtx->priv_data, "preset", 8, 0);
    } else if (enc == "libaom-av1") {
        av_opt_set_int(ff.codecCtx->priv_data, "cpu-used", 6, 0);
        av_opt_set(ff.codecCtx->priv_data, "usage", "realtime", 0);
    } else if (enc == "librav1e") {
        av_opt_set_int(ff.codecCtx->priv_data, "speed", 8, 0);
    }

    if (ff.fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        ff.codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(ff.codecCtx, codec, nullptr) < 0) {
        fmt::print(stderr, "[VideoRecorder] Failed to open AV1 codec\n");
        return false;
    }

    ff.stream = avformat_new_stream(ff.fmtCtx, nullptr);
    if (!ff.stream) {
        return false;
    }
    ff.stream->time_base = ff.codecCtx->time_base;
    avcodec_parameters_from_context(ff.stream->codecpar, ff.codecCtx);

    if (!(ff.fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ff.fmtCtx->pb, m_config.outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            fmt::print(stderr, "[VideoRecorder] Cannot open output file '{}'\n",
                       m_config.outputPath);
            return false;
        }
    }

    if (avformat_write_header(ff.fmtCtx, nullptr) < 0) {
        fmt::print(stderr, "[VideoRecorder] Failed to write container header\n");
        return false;
    }

    ff.frame = av_frame_alloc();
    ff.frame->format = AV_PIX_FMT_YUV420P;
    ff.frame->width = ff.codecCtx->width;
    ff.frame->height = ff.codecCtx->height;
    if (av_frame_get_buffer(ff.frame, 0) < 0) {
        return false;
    }

    ff.packet = av_packet_alloc();

    ff.swsCtx = sws_getContext(
        static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGBA,
        ff.codecCtx->width, ff.codecCtx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!ff.swsCtx) {
        fmt::print(stderr, "[VideoRecorder] Failed to create color space converter\n");
        return false;
    }

    fmt::print("[VideoRecorder] Started: {} ({}x{} @ {} fps, encoder: {})\n",
               m_config.outputPath, width, height, m_config.fps, enc);
    return true;
#endif
}

bool VideoRecorder::encodeFrame(const RawFrame& rawFrame) {
#ifndef VAPOR_HAS_FFMPEG
    return false;
#else
    auto& ff = *m_ffmpeg;

    if (av_frame_make_writable(ff.frame) < 0) {
        return false;
    }

    // Convert RGBA input to YUV420P
    const uint8_t* srcPlanes[1] = {rawFrame.pixels.data()};
    int srcStrides[1] = {static_cast<int>(rawFrame.width) * 4};
    sws_scale(ff.swsCtx, srcPlanes, srcStrides, 0, static_cast<int>(rawFrame.height),
              ff.frame->data, ff.frame->linesize);

    ff.frame->pts = ff.pts++;

    if (avcodec_send_frame(ff.codecCtx, ff.frame) < 0) {
        return false;
    }

    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(ff.codecCtx, ff.packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return false;
        }
        av_packet_rescale_ts(ff.packet, ff.codecCtx->time_base, ff.stream->time_base);
        ff.packet->stream_index = ff.stream->index;
        av_interleaved_write_frame(ff.fmtCtx, ff.packet);
        av_packet_unref(ff.packet);
    }

    return true;
#endif
}

void VideoRecorder::flushEncoder() {
#ifdef VAPOR_HAS_FFMPEG
    auto& ff = *m_ffmpeg;
    if (!ff.codecCtx) {
        return;
    }

    avcodec_send_frame(ff.codecCtx, nullptr);

    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(ff.codecCtx, ff.packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        av_packet_rescale_ts(ff.packet, ff.codecCtx->time_base, ff.stream->time_base);
        ff.packet->stream_index = ff.stream->index;
        av_interleaved_write_frame(ff.fmtCtx, ff.packet);
        av_packet_unref(ff.packet);
    }
#endif
}

void VideoRecorder::cleanup() {
#ifdef VAPOR_HAS_FFMPEG
    if (!m_ffmpeg) {
        return;
    }
    auto& ff = *m_ffmpeg;

    if (ff.fmtCtx) {
        if (ff.fmtCtx->pb) {
            av_write_trailer(ff.fmtCtx);
            avio_closep(&ff.fmtCtx->pb);
        }
        avformat_free_context(ff.fmtCtx);
        ff.fmtCtx = nullptr;
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
    if (ff.swsCtx) {
        sws_freeContext(ff.swsCtx);
        ff.swsCtx = nullptr;
    }

    m_ffmpeg.reset();
    fmt::print("[VideoRecorder] Finished: {}\n", m_config.outputPath);
#endif
}

} // namespace Vapor
