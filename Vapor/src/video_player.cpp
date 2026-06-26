#include "video_player.hpp"
#include <fmt/core.h>

#ifdef VAPOR_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include "miniaudio.h"
#endif

#include <cstring>
#include <deque>

namespace Vapor {

// ─── FFmpegDecodeContext ──────────────────────────────────────────────────────

struct VideoPlayer::FFmpegDecodeContext {
#ifdef VAPOR_HAS_FFMPEG
    // Video
    AVFormatContext* fmtCtx         = nullptr;
    AVCodecContext*  codecCtx       = nullptr;
    SwsContext*      swsCtx         = nullptr;
    AVFrame*         frame          = nullptr;
    AVFrame*         hwFrame        = nullptr; // CPU staging frame for HW transfer
    AVPacket*        packet         = nullptr;
    int              videoStreamIdx = -1;
    double           timeBase       = 0.0;

    // Hardware acceleration
    AVBufferRef*  hwDeviceCtx = nullptr;
    AVPixelFormat hwPixFmt    = AV_PIX_FMT_NONE; // the HW pixel format this session uses

    // Audio decode
    AVCodecContext*  audioCodecCtx   = nullptr;
    SwrContext*      audioSwrCtx     = nullptr;
    AVFrame*         audioFrame      = nullptr;
    int              audioStreamIdx  = -1;
    int              audioSampleRate = 0;
    int              audioChannels   = 0;

    // Audio playback (miniaudio low-level device)
    ma_device        audioDevice             = {};
    bool             audioDeviceInitialized  = false;
    bool             audioReady              = false;

    // PCM queue shared between decode thread and audio callback
    std::deque<int16_t> audioPCM;
    std::mutex          audioMutex;
#endif
};

// ─── miniaudio data callback ──────────────────────────────────────────────────

#ifdef VAPOR_HAS_FFMPEG
static void maAudioDataCallback(ma_device* pDevice, void* pOutput,
                                const void* /*pInput*/, ma_uint32 frameCount)
{
    auto* ff  = static_cast<VideoPlayer::FFmpegDecodeContext*>(pDevice->pUserData);
    auto* out = static_cast<int16_t*>(pOutput);
    int total = static_cast<int>(frameCount) * ff->audioChannels;

    std::lock_guard<std::mutex> lk(ff->audioMutex);
    int avail  = static_cast<int>(ff->audioPCM.size());
    int toCopy = std::min(total, avail);
    for (int i = 0; i < toCopy; ++i) {
        out[i] = ff->audioPCM.front();
        ff->audioPCM.pop_front();
    }
    if (toCopy < total) {
        std::memset(out + toCopy, 0,
                    static_cast<size_t>((total - toCopy) * sizeof(int16_t)));
    }
}
#endif

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

    m_currentTime     = 0.0;
    m_hasCurrentFrame = false;
    m_stop            = false;
    m_finished        = false;
    m_open            = true;

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

#ifdef VAPOR_HAS_FFMPEG
    if (m_ffmpeg && m_ffmpeg->audioDeviceInitialized) {
        ma_device_uninit(&m_ffmpeg->audioDevice);
        m_ffmpeg->audioDeviceInitialized = false;
        m_ffmpeg->audioReady = false;
    }
    if (m_ffmpeg) {
        std::lock_guard<std::mutex> al(m_ffmpeg->audioMutex);
        m_ffmpeg->audioPCM.clear();
    }
#endif

    cleanup();

    m_open            = false;
    m_playing         = false;
    m_finished        = false;
    m_currentTime     = 0.0;
    m_duration        = 0.0;
    m_hasCurrentFrame = false;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameQueue.clear();
}

void VideoPlayer::play() {
    m_playing = true;
#ifdef VAPOR_HAS_FFMPEG
    if (m_ffmpeg && m_ffmpeg->audioReady) {
        ma_device_start(&m_ffmpeg->audioDevice);
    }
#endif
}

void VideoPlayer::pause() {
    m_playing = false;
#ifdef VAPOR_HAS_FFMPEG
    if (m_ffmpeg && m_ffmpeg->audioReady) {
        ma_device_stop(&m_ffmpeg->audioDevice);
    }
#endif
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

        while (!m_frameQueue.empty() && m_frameQueue.front().pts <= m_currentTime) {
            m_currentFrame    = std::move(m_frameQueue.front());
            m_frameQueue.pop_front();
            m_hasCurrentFrame = true;
            frameChanged      = true;
        }
    }

    m_cv.notify_one();

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

    auto enqueueAudio = [&](AVFrame* af) {
        int outSamples = swr_get_out_samples(ff.audioSwrCtx, af->nb_samples);
        if (outSamples <= 0) {
            return;
        }

        std::vector<int16_t> pcm(static_cast<size_t>(outSamples) * ff.audioChannels);
        uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(pcm.data())};
        int converted = swr_convert(ff.audioSwrCtx,
                                    outPlanes, outSamples,
                                    const_cast<const uint8_t**>(af->data),
                                    af->nb_samples);
        if (converted <= 0) {
            return;
        }
        pcm.resize(static_cast<size_t>(converted) * ff.audioChannels);

        std::lock_guard<std::mutex> al(ff.audioMutex);
        static constexpr size_t MAX_PCM = 48000 * 2 * 5; // ~5 seconds
        if (ff.audioPCM.size() + pcm.size() <= MAX_PCM) {
            ff.audioPCM.insert(ff.audioPCM.end(), pcm.begin(), pcm.end());
        }
    };

    while (!m_stop) {
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
            // Flush video decoder
            avcodec_send_packet(ff.codecCtx, nullptr);
            while (avcodec_receive_frame(ff.codecCtx, ff.frame) >= 0) {
                convertAndEnqueue(ff.frame);
            }
            // Flush audio decoder
            if (ff.audioStreamIdx >= 0 && ff.audioCodecCtx) {
                avcodec_send_packet(ff.audioCodecCtx, nullptr);
                while (avcodec_receive_frame(ff.audioCodecCtx, ff.audioFrame) >= 0) {
                    enqueueAudio(ff.audioFrame);
                }
            }
            m_finished = true;
            break;
        }
        if (ret < 0) {
            break;
        }

        if (ff.packet->stream_index == ff.videoStreamIdx) {
            avcodec_send_packet(ff.codecCtx, ff.packet);
            av_packet_unref(ff.packet);
            while (avcodec_receive_frame(ff.codecCtx, ff.frame) >= 0) {
                if (!convertAndEnqueue(ff.frame)) {
                    break;
                }
            }
        } else if (ff.audioStreamIdx >= 0 &&
                   ff.packet->stream_index == ff.audioStreamIdx &&
                   ff.audioCodecCtx) {
            avcodec_send_packet(ff.audioCodecCtx, ff.packet);
            av_packet_unref(ff.packet);
            while (avcodec_receive_frame(ff.audioCodecCtx, ff.audioFrame) >= 0) {
                enqueueAudio(ff.audioFrame);
            }
        } else {
            av_packet_unref(ff.packet);
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

    // ── Video stream ────────────────────────────────────────────────────────
    const AVCodec* codec = nullptr;
    ff.videoStreamIdx = av_find_best_stream(ff.fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ff.videoStreamIdx < 0 || !codec) {
        fmt::print(stderr, "[VideoPlayer] No video stream found in '{}'\n", path);
        return false;
    }

    AVStream* vstream = ff.fmtCtx->streams[ff.videoStreamIdx];
    ff.timeBase       = av_q2d(vstream->time_base);

    if (ff.fmtCtx->duration != AV_NOPTS_VALUE) {
        m_duration = static_cast<double>(ff.fmtCtx->duration) / AV_TIME_BASE;
    }

    ff.codecCtx = avcodec_alloc_context3(codec);
    if (!ff.codecCtx) {
        return false;
    }
    avcodec_parameters_to_context(ff.codecCtx, vstream->codecpar);

    // ── Hardware acceleration probe ─────────────────────────────────────────
    // Try each HW type in priority order; first one that succeeds is used.
    // All fall through gracefully to pure software if nothing works.
    static constexpr AVHWDeviceType kHwPriority[] = {
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX, // macOS
        AV_HWDEVICE_TYPE_CUDA,         // NVIDIA
        AV_HWDEVICE_TYPE_VAAPI,        // Linux / Intel
        AV_HWDEVICE_TYPE_D3D11VA,      // Windows
        AV_HWDEVICE_TYPE_NONE,
    };
    for (const AVHWDeviceType hwType : kHwPriority) {
        if (hwType == AV_HWDEVICE_TYPE_NONE) {
            break;
        }

        // Does the selected software codec advertise support for this HW type?
        AVPixelFormat hwPix = AV_PIX_FMT_NONE;
        for (int i = 0; ; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) { break; }
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && cfg->device_type == hwType) {
                hwPix = cfg->pix_fmt;
                break;
            }
        }
        if (hwPix == AV_PIX_FMT_NONE) { continue; }

        if (av_hwdevice_ctx_create(&ff.hwDeviceCtx, hwType,
                                   nullptr, nullptr, 0) < 0) {
            continue; // this HW isn't available on this machine
        }

        ff.hwPixFmt = hwPix;
        ff.codecCtx->hw_device_ctx = av_buffer_ref(ff.hwDeviceCtx);

        // Captureless lambda — convertible to C function pointer.
        ff.codecCtx->opaque = &ff.hwPixFmt;
        ff.codecCtx->get_format = [](AVCodecContext* ctx,
                                     const AVPixelFormat* fmts) -> AVPixelFormat {
            const auto target = *static_cast<AVPixelFormat*>(ctx->opaque);
            for (const AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; ++f) {
                if (*f == target) { return *f; }
            }
            return fmts[0]; // codec didn't offer HW fmt — accept software fallback
        };

        fmt::print("[VideoPlayer] HW decode: {} ({})\n",
                   av_hwdevice_get_type_name(hwType),
                   av_get_pix_fmt_name(hwPix));
        break;
    }

    if (avcodec_open2(ff.codecCtx, codec, nullptr) < 0) {
        fmt::print(stderr, "[VideoPlayer] Failed to open video decoder for '{}'\n", path);
        return false;
    }

    ff.frame   = av_frame_alloc();
    ff.hwFrame = av_frame_alloc(); // CPU staging frame for HW→CPU transfer
    ff.packet  = av_packet_alloc();

    // swsCtx is created lazily in convertAndEnqueue() once we know the actual
    // pixel format — software decoders report it at open time, but hardware
    // decoders give us the real CPU format only after the first transfer.

    // ── Audio stream (optional) ─────────────────────────────────────────────
    const AVCodec* acodec = nullptr;
    int aIdx = av_find_best_stream(ff.fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (aIdx >= 0 && acodec) {
        AVStream* astream = ff.fmtCtx->streams[aIdx];

        ff.audioCodecCtx = avcodec_alloc_context3(acodec);
        if (ff.audioCodecCtx) {
            avcodec_parameters_to_context(ff.audioCodecCtx, astream->codecpar);
            if (avcodec_open2(ff.audioCodecCtx, acodec, nullptr) >= 0) {
                ff.audioStreamIdx  = aIdx;
                ff.audioSampleRate = ff.audioCodecCtx->sample_rate;
                ff.audioChannels   = std::min(ff.audioCodecCtx->ch_layout.nb_channels, 2);
                ff.audioFrame      = av_frame_alloc();

                // Convert any input format → S16 interleaved (1 or 2 channels)
                AVChannelLayout outLayout{};
                av_channel_layout_default(&outLayout, ff.audioChannels);

                int swrRet = swr_alloc_set_opts2(
                    &ff.audioSwrCtx,
                    &outLayout,
                    AV_SAMPLE_FMT_S16,
                    ff.audioSampleRate,
                    &ff.audioCodecCtx->ch_layout,
                    ff.audioCodecCtx->sample_fmt,
                    ff.audioCodecCtx->sample_rate,
                    0, nullptr);

                av_channel_layout_uninit(&outLayout);

                if (swrRet >= 0 && swr_init(ff.audioSwrCtx) >= 0) {
                    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
                    cfg.playback.format   = ma_format_s16;
                    cfg.playback.channels = static_cast<ma_uint32>(ff.audioChannels);
                    cfg.sampleRate        = static_cast<ma_uint32>(ff.audioSampleRate);
                    cfg.dataCallback      = maAudioDataCallback;
                    cfg.pUserData         = &ff;

                    if (ma_device_init(nullptr, &cfg, &ff.audioDevice) == MA_SUCCESS) {
                        ff.audioDeviceInitialized = true;
                        ff.audioReady = true;
                        fmt::print("[VideoPlayer] Audio: {} Hz, {} ch ({})\n",
                                   ff.audioSampleRate, ff.audioChannels, acodec->name);
                    } else {
                        fmt::print(stderr, "[VideoPlayer] ma_device_init failed; audio disabled\n");
                        swr_free(&ff.audioSwrCtx);
                        avcodec_free_context(&ff.audioCodecCtx);
                        av_frame_free(&ff.audioFrame);
                        ff.audioStreamIdx = -1;
                    }
                } else {
                    fmt::print(stderr, "[VideoPlayer] swr init failed; audio disabled\n");
                    if (ff.audioSwrCtx) { swr_free(&ff.audioSwrCtx); }
                    avcodec_free_context(&ff.audioCodecCtx);
                    av_frame_free(&ff.audioFrame);
                    ff.audioStreamIdx = -1;
                }
            } else {
                avcodec_free_context(&ff.audioCodecCtx);
            }
        }
    }

    fmt::print("[VideoPlayer] Opened '{}' — {}x{} {:.1f}s ({})\n",
               path, ff.codecCtx->width, ff.codecCtx->height, m_duration, codec->name);
    return true;
#endif
}

bool VideoPlayer::convertAndEnqueue(void* avframe_ptr) {
#ifndef VAPOR_HAS_FFMPEG
    return false;
#else
    auto& ff      = *m_ffmpeg;
    auto* avframe = static_cast<AVFrame*>(avframe_ptr);

    // If the frame is in hardware memory, transfer it to a CPU frame first.
    AVFrame* swFrame = avframe;
    if (ff.hwPixFmt != AV_PIX_FMT_NONE
        && avframe->format == static_cast<int>(ff.hwPixFmt)) {
        av_frame_unref(ff.hwFrame);
        if (av_hwframe_transfer_data(ff.hwFrame, avframe, 0) < 0) {
            return true; // skip this frame rather than hard-failing
        }
        ff.hwFrame->pts = avframe->pts;
        swFrame = ff.hwFrame;
    }

    int w = swFrame->width;
    int h = swFrame->height;

    // Lazy swsCtx init: format is unknown until after the first HW transfer.
    if (!ff.swsCtx) {
        ff.swsCtx = sws_getContext(
            w, h, static_cast<AVPixelFormat>(swFrame->format),
            w, h, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!ff.swsCtx) {
            fmt::print(stderr, "[VideoPlayer] Failed to create colour converter\n");
            return false;
        }
    }

    Frame decoded;
    decoded.pixels.resize(static_cast<size_t>(w) * h * 4);
    decoded.width  = static_cast<uint32_t>(w);
    decoded.height = static_cast<uint32_t>(h);
    decoded.pts    = (swFrame->pts != AV_NOPTS_VALUE)
                         ? static_cast<double>(swFrame->pts) * ff.timeBase
                         : 0.0;

    uint8_t* dstPlanes[1]  = {decoded.pixels.data()};
    int      dstStrides[1] = {w * 4};
    sws_scale(ff.swsCtx, swFrame->data, swFrame->linesize, 0, h, dstPlanes, dstStrides);

    {
        std::unique_lock<std::mutex> lock(m_mutex);
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

    // Audio
    if (ff.audioSwrCtx) {
        swr_free(&ff.audioSwrCtx);
    }
    if (ff.audioFrame) {
        av_frame_free(&ff.audioFrame);
    }
    if (ff.audioCodecCtx) {
        avcodec_free_context(&ff.audioCodecCtx);
    }

    // Video
    if (ff.swsCtx) {
        sws_freeContext(ff.swsCtx);
        ff.swsCtx = nullptr;
    }
    if (ff.hwFrame) {
        av_frame_free(&ff.hwFrame);
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
    if (ff.hwDeviceCtx) {
        av_buffer_unref(&ff.hwDeviceCtx);
    }
    if (ff.fmtCtx) {
        avformat_close_input(&ff.fmtCtx);
    }

    m_ffmpeg.reset();
#endif
}

} // namespace Vapor
