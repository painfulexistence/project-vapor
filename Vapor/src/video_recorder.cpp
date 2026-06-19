#include "video_recorder.hpp"
#include <fmt/core.h>

#ifdef VAPOR_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace Vapor {

// ─── FFmpegContext ────────────────────────────────────────────────────────────

struct VideoRecorder::FFmpegContext {
#ifdef VAPOR_HAS_FFMPEG
    // Video
    AVCodecContext* codecCtx = nullptr;
    AVFormatContext* fmtCtx = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* swsCtx = nullptr;
    // Audio
    AVCodecContext* audioCodecCtx = nullptr;
    AVStream* audioStream = nullptr;
    AVPacket* audioPacket = nullptr;
    AVFrame* audioFrame = nullptr;
    SwrContext* swrCtx = nullptr;
    AVAudioFifo* audioFifo = nullptr;
    int64_t audioNextPts = 0; // cumulative samples (per channel) handed to encoder
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
    m_recordingStart = std::chrono::steady_clock::now();
    m_stop = false;

    // Decide up front whether this session records audio. The audio encoder is
    // initialised lazily (in initEncoder, once the first video frame fixes the
    // dimensions), but the capture sink is attached now so PCM buffers from the
    // very start of recording.
    m_audioActive = false;
    if (m_config.captureAudio && m_audioManager && m_audioManager->isInitialized()) {
        m_audioSampleRate = static_cast<int>(m_audioManager->getSampleRate());
        m_audioChannels   = static_cast<int>(m_audioManager->getChannels());
        m_audioActive     = true;
        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            m_audioQueue.clear();
        }
    }

    m_recording = true;

    // Attach the sink only after m_recording is set so writeAudio() accepts data.
    if (m_audioActive) {
        m_audioManager->setCaptureSink(this);
    }

    m_encoderThread = std::thread(&VideoRecorder::encoderThreadFunc, this);
    return true;
#endif
}

void VideoRecorder::stopRecording() {
    if (!m_recording) {
        return;
    }

    // Stop feeding new audio first; whatever is already queued will be drained
    // and flushed by the encoder thread before it exits.
    if (m_audioActive && m_audioManager) {
        m_audioManager->clearCaptureSink();
    }

    m_stop = true;
    m_cv.notify_all();

    if (m_encoderThread.joinable()) {
        m_encoderThread.join();
    }

    m_recording = false;
    m_audioActive = false;
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

    // Capture wall-clock time before the async readback so it reflects the
    // moment this frame was presented, not when the callback fires.
    auto captureTime = std::chrono::steady_clock::now();

    m_renderer->readPixelsAsync([this, captureTime](const GpuImageData& data) {
        if (!m_recording) {
            return;
        }

        double timestamp = std::chrono::duration<double>(captureTime - m_recordingStart).count();

        RawFrame frame;
        frame.pixels = data.data;
        frame.width = data.width;
        frame.height = data.height;
        frame.timestamp = timestamp;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_frameQueue.size() < MAX_QUEUE_FRAMES) {
                m_frameQueue.push(std::move(frame));
            }
        }
        m_cv.notify_one();
    });
}

// ─── Audio capture (audio thread) ─────────────────────────────────────────────

void VideoRecorder::writeAudio(const float* frames, uint32_t frameCount) {
    if (!m_audioActive || !m_recording.load() || frameCount == 0) {
        return;
    }

    const size_t incoming = static_cast<size_t>(frameCount) * m_audioChannels;
    // Cap the backlog at ~5 seconds so a lagging encoder can't grow this without
    // bound. Dropping the oldest samples keeps the most recent audio.
    const size_t cap = static_cast<size_t>(m_audioSampleRate) * m_audioChannels * 5;

    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (m_audioQueue.size() + incoming > cap) {
        size_t overflow = std::min(m_audioQueue.size() + incoming - cap, m_audioQueue.size());
        m_audioQueue.erase(m_audioQueue.begin(), m_audioQueue.begin() + overflow);
    }
    m_audioQueue.insert(m_audioQueue.end(), frames, frames + incoming);
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
        // Interleave audio: drain whatever PCM has accumulated since last frame.
        drainAudio(/*flush=*/false);
    }

    if (encoderInitialized) {
        drainAudio(/*flush=*/true); // resample + encode remaining audio
        flushAudioEncoder();
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

    // AV1 requires even-numbered dimensions; crop one pixel if needed.
    width  &= ~1u;
    height &= ~1u;
    if (width == 0 || height == 0) {
        fmt::print(stderr, "[VideoRecorder] Frame dimensions too small after alignment\n");
        return false;
    }

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
    // Microsecond time base → pts values are real elapsed microseconds.
    // Framerate hint is still useful for the encoder's GOP and rate control.
    ff.codecCtx->time_base = AVRational{1, 1'000'000};
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

    // Set up the audio stream before writing the header so both tracks are
    // declared in the container. Failure here downgrades to video-only.
    if (m_audioActive) {
        if (!initAudioEncoder()) {
            fmt::print(stderr, "[VideoRecorder] Audio encoder init failed — recording video only\n");
            m_audioActive = false;
            if (m_audioManager) {
                m_audioManager->clearCaptureSink();
            }
        }
    }

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

    // Convert RGBA input to YUV420P (clamp src height to encoder height in case
    // a frame arrived before the even-dimension crop was applied to frame.height)
    const uint8_t* srcPlanes[1] = {rawFrame.pixels.data()};
    int srcStrides[1] = {static_cast<int>(rawFrame.width) * 4};
    int srcHeight = std::min(static_cast<int>(rawFrame.height), ff.codecCtx->height);
    sws_scale(ff.swsCtx, srcPlanes, srcStrides, 0, srcHeight,
              ff.frame->data, ff.frame->linesize);

    // Wall-clock pts in microseconds so playback speed matches actual game speed.
    ff.frame->pts = static_cast<int64_t>(rawFrame.timestamp * 1'000'000.0);

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

// ─── Audio encoding ───────────────────────────────────────────────────────────

bool VideoRecorder::initAudioEncoder() {
#ifndef VAPOR_HAS_FFMPEG
    return false;
#else
    auto& ff = *m_ffmpeg;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fmt::print(stderr, "[VideoRecorder] AAC encoder not available\n");
        return false;
    }

    ff.audioCodecCtx = avcodec_alloc_context3(codec);
    if (!ff.audioCodecCtx) {
        return false;
    }

    ff.audioCodecCtx->sample_fmt  = AV_SAMPLE_FMT_FLTP; // native AAC encoder format
    ff.audioCodecCtx->sample_rate = m_audioSampleRate;
    ff.audioCodecCtx->bit_rate    = m_config.audioBitrate;
    av_channel_layout_default(&ff.audioCodecCtx->ch_layout, m_audioChannels);
    ff.audioCodecCtx->time_base   = AVRational{1, m_audioSampleRate};

    if (ff.fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        ff.audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(ff.audioCodecCtx, codec, nullptr) < 0) {
        fmt::print(stderr, "[VideoRecorder] Failed to open AAC encoder\n");
        return false;
    }

    ff.audioStream = avformat_new_stream(ff.fmtCtx, nullptr);
    if (!ff.audioStream) {
        return false;
    }
    ff.audioStream->time_base = ff.audioCodecCtx->time_base;
    avcodec_parameters_from_context(ff.audioStream->codecpar, ff.audioCodecCtx);

    // Resampler: engine output (interleaved float) → encoder format (FLTP).
    // Sample rate and channel count are unchanged; this is a layout conversion.
    AVChannelLayout inLayout, outLayout;
    av_channel_layout_default(&inLayout, m_audioChannels);
    av_channel_layout_default(&outLayout, m_audioChannels);
    int swrErr = swr_alloc_set_opts2(&ff.swrCtx,
                                     &outLayout, AV_SAMPLE_FMT_FLTP, m_audioSampleRate,
                                     &inLayout, AV_SAMPLE_FMT_FLT, m_audioSampleRate,
                                     0, nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    if (swrErr < 0 || !ff.swrCtx || swr_init(ff.swrCtx) < 0) {
        fmt::print(stderr, "[VideoRecorder] Failed to init audio resampler\n");
        return false;
    }

    ff.audioFifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, m_audioChannels, 1);
    ff.audioFrame  = av_frame_alloc();
    ff.audioPacket = av_packet_alloc();
    if (!ff.audioFifo || !ff.audioFrame || !ff.audioPacket) {
        return false;
    }

    fmt::print("[VideoRecorder] Audio track: AAC {} Hz, {} ch, {} kbps\n",
               m_audioSampleRate, m_audioChannels, m_config.audioBitrate / 1000);
    return true;
#endif
}

void VideoRecorder::drainAudio(bool flush) {
#ifdef VAPOR_HAS_FFMPEG
    if (!m_audioActive || !m_ffmpeg || !m_ffmpeg->audioCodecCtx) {
        return;
    }
    auto& ff = *m_ffmpeg;

    // Move queued interleaved PCM out under the lock; resample outside it.
    std::vector<float> input;
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (!m_audioQueue.empty()) {
            input.assign(m_audioQueue.begin(), m_audioQueue.end());
            m_audioQueue.clear();
        }
    }

    if (!input.empty()) {
        const int inSamples = static_cast<int>(input.size()) / m_audioChannels;
        const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(input.data()) };

        int outCount = swr_get_out_samples(ff.swrCtx, inSamples);
        if (outCount > 0) {
            uint8_t** outData = nullptr;
            int outLinesize = 0;
            if (av_samples_alloc_array_and_samples(&outData, &outLinesize, m_audioChannels,
                                                   outCount, AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                int converted = swr_convert(ff.swrCtx, outData, outCount, inData, inSamples);
                if (converted > 0) {
                    av_audio_fifo_write(ff.audioFifo, reinterpret_cast<void**>(outData), converted);
                }
                if (outData) {
                    av_freep(&outData[0]);
                }
                av_freep(&outData);
            }
        }
    }

    const int frameSize = ff.audioCodecCtx->frame_size > 0 ? ff.audioCodecCtx->frame_size : 1024;

    while (av_audio_fifo_size(ff.audioFifo) >= frameSize) {
        encodeAudioFrame(frameSize);
    }

    if (flush) {
        // Flush the resampler's internal delay buffer.
        int outCount = swr_get_out_samples(ff.swrCtx, 0);
        if (outCount > 0) {
            uint8_t** outData = nullptr;
            int outLinesize = 0;
            if (av_samples_alloc_array_and_samples(&outData, &outLinesize, m_audioChannels,
                                                   outCount, AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                int converted = swr_convert(ff.swrCtx, outData, outCount, nullptr, 0);
                if (converted > 0) {
                    av_audio_fifo_write(ff.audioFifo, reinterpret_cast<void**>(outData), converted);
                }
                if (outData) {
                    av_freep(&outData[0]);
                }
                av_freep(&outData);
            }
        }
        while (av_audio_fifo_size(ff.audioFifo) >= frameSize) {
            encodeAudioFrame(frameSize);
        }
        int remaining = av_audio_fifo_size(ff.audioFifo);
        if (remaining > 0) {
            encodeAudioFrame(remaining); // final, possibly short, frame
        }
    }
#else
    (void)flush;
#endif
}

void VideoRecorder::encodeAudioFrame(int nbSamples) {
#ifdef VAPOR_HAS_FFMPEG
    auto& ff = *m_ffmpeg;

    av_frame_unref(ff.audioFrame);
    ff.audioFrame->nb_samples  = nbSamples;
    ff.audioFrame->format      = ff.audioCodecCtx->sample_fmt;
    ff.audioFrame->sample_rate = ff.audioCodecCtx->sample_rate;
    av_channel_layout_copy(&ff.audioFrame->ch_layout, &ff.audioCodecCtx->ch_layout);
    if (av_frame_get_buffer(ff.audioFrame, 0) < 0) {
        return;
    }

    av_audio_fifo_read(ff.audioFifo, reinterpret_cast<void**>(ff.audioFrame->data), nbSamples);

    // Sample-accurate pts keeps audio gap-free and in sync with the video.
    ff.audioFrame->pts = ff.audioNextPts;
    ff.audioNextPts += nbSamples;

    if (avcodec_send_frame(ff.audioCodecCtx, ff.audioFrame) < 0) {
        return;
    }

    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(ff.audioCodecCtx, ff.audioPacket);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return;
        }
        av_packet_rescale_ts(ff.audioPacket, ff.audioCodecCtx->time_base, ff.audioStream->time_base);
        ff.audioPacket->stream_index = ff.audioStream->index;
        av_interleaved_write_frame(ff.fmtCtx, ff.audioPacket);
        av_packet_unref(ff.audioPacket);
    }
#else
    (void)nbSamples;
#endif
}

void VideoRecorder::flushAudioEncoder() {
#ifdef VAPOR_HAS_FFMPEG
    if (!m_audioActive || !m_ffmpeg || !m_ffmpeg->audioCodecCtx) {
        return;
    }
    auto& ff = *m_ffmpeg;

    avcodec_send_frame(ff.audioCodecCtx, nullptr);

    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(ff.audioCodecCtx, ff.audioPacket);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        av_packet_rescale_ts(ff.audioPacket, ff.audioCodecCtx->time_base, ff.audioStream->time_base);
        ff.audioPacket->stream_index = ff.audioStream->index;
        av_interleaved_write_frame(ff.fmtCtx, ff.audioPacket);
        av_packet_unref(ff.audioPacket);
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
        avformat_free_context(ff.fmtCtx); // also frees the stream objects
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

    // Audio resources
    if (ff.audioFrame) {
        av_frame_free(&ff.audioFrame);
    }
    if (ff.audioPacket) {
        av_packet_free(&ff.audioPacket);
    }
    if (ff.audioCodecCtx) {
        avcodec_free_context(&ff.audioCodecCtx);
    }
    if (ff.swrCtx) {
        swr_free(&ff.swrCtx);
    }
    if (ff.audioFifo) {
        av_audio_fifo_free(ff.audioFifo);
        ff.audioFifo = nullptr;
    }

    m_ffmpeg.reset();
    fmt::print("[VideoRecorder] Finished: {}\n", m_config.outputPath);
#endif
}

} // namespace Vapor
