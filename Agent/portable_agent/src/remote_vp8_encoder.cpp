#include "remote_vp8_encoder.h"

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

struct Vp8Encoder::Impl {
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrateKbps = 0;
    int cpuUsed = 6;
    int minQuantizer = 4;
    int maxQuantizer = 34;
    int threads = 2;

    vpx_codec_ctx_t codec{};
    vpx_codec_enc_cfg_t cfg{};
    vpx_image_t image{};
    bool initialized = false;
    bool imageAllocated = false;
    uint64_t frameCount = 0;
    uint64_t nextPts = 0;

    Impl(int w,
        int h,
        int fpsIn,
        int bitrateIn,
        int cpuUsedIn,
        int minQuantizerIn,
        int maxQuantizerIn,
        int threadsIn)
        : width(w),
        height(h),
        fps(std::max(1, fpsIn)),
        bitrateKbps(std::max(100, bitrateIn)),
        cpuUsed(std::max(0, std::min(16, cpuUsedIn))),
        minQuantizer(std::max(0, std::min(63, minQuantizerIn))),
        maxQuantizer(std::max(0, std::min(63, maxQuantizerIn))),
        threads(std::max(1, std::min(8, threadsIn))) {

        if (minQuantizer > maxQuantizer) {
            std::swap(minQuantizer, maxQuantizer);
        }

        if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) {
            throw std::runtime_error("vpx_codec_enc_config_default failed");
        }

        cfg.g_w = width;
        cfg.g_h = height;
        cfg.g_timebase.num = 1;
        cfg.g_timebase.den = 90000;
        cfg.g_threads = threads;
        cfg.g_pass = VPX_RC_ONE_PASS;
        cfg.g_lag_in_frames = 0;
        cfg.rc_end_usage = VPX_CBR;
        cfg.rc_target_bitrate = bitrateKbps;

        // Saved anti-fuzz baseline, but relaxed enough for the low-CPU profiles:
        // no adaptive resize/drop, error resilient stream, tight-ish quantizers,
        // and event-based keyframes driven by the caller.
        cfg.rc_resize_allowed = 0;
        cfg.rc_dropframe_thresh = 0;
        cfg.rc_min_quantizer = minQuantizer;
        cfg.rc_max_quantizer = maxQuantizer;
        cfg.rc_buf_initial_sz = 400;
        cfg.rc_buf_optimal_sz = 600;
        cfg.rc_buf_sz = 1000;

        cfg.kf_mode = VPX_KF_AUTO;
        cfg.kf_min_dist = 0;
        cfg.kf_max_dist = std::max(3, fps * 10);

#ifdef VPX_ERROR_RESILIENT_DEFAULT
        cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
#else
        cfg.g_error_resilient = 1;
#endif

        const auto initResult =
            vpx_codec_enc_init(&codec, vpx_codec_vp8_cx(), &cfg, 0);
        if (initResult != VPX_CODEC_OK) {
            throw std::runtime_error(
                std::string("vpx_codec_enc_init failed: ") +
                vpx_codec_err_to_string(initResult));
        }

        if (!vpx_img_alloc(
                &image,
                VPX_IMG_FMT_I420,
                static_cast<unsigned int>(width),
                static_cast<unsigned int>(height),
                32)) {
            vpx_codec_destroy(&codec);
            throw std::runtime_error("vpx_img_alloc failed");
        }
        imageAllocated = true;

        vpx_codec_control(&codec, VP8E_SET_CPUUSED, cpuUsed);
        vpx_codec_control(&codec, VP8E_SET_NOISE_SENSITIVITY, 0);
        vpx_codec_control(&codec, VP8E_SET_STATIC_THRESHOLD, 0);
        vpx_codec_control(&codec, VP8E_SET_SCREEN_CONTENT_MODE, 1);
        initialized = true;
    }

    ~Impl() {
        if (imageAllocated) {
            vpx_img_free(&image);
        }
        if (initialized) {
            vpx_codec_destroy(&codec);
        }
    }
};

Vp8Encoder::Vp8Encoder(int width,
    int height,
    int fps,
    int bitrateKbps,
    int cpuUsed,
    int minQuantizer,
    int maxQuantizer,
    int threads)
    : m_impl(new Impl(width, height, fps, bitrateKbps, cpuUsed, minQuantizer, maxQuantizer, threads)) {
}

Vp8Encoder::~Vp8Encoder() {
    delete m_impl;
}

EncodedFrame Vp8Encoder::encode(const I420Frame& frame, bool forceKeyframe) {
    if (!m_impl || frame.width != m_impl->width || frame.height != m_impl->height) {
        throw std::runtime_error("frame size mismatch");
    }
    if (!m_impl->imageAllocated) {
        throw std::runtime_error("VP8 image buffer is not initialized");
    }

    const int chromaWidth = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    const auto requiredY =
        static_cast<std::size_t>(frame.width) * frame.height;
    const auto requiredChroma =
        static_cast<std::size_t>(chromaWidth) * chromaHeight;
    if (frame.y.size() < requiredY ||
        frame.u.size() < requiredChroma ||
        frame.v.size() < requiredChroma) {
        throw std::runtime_error("I420 frame plane size mismatch");
    }

    auto& img = m_impl->image;
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(
            img.planes[0] + static_cast<std::size_t>(row) * img.stride[0],
            frame.y.data() + static_cast<std::size_t>(row) * frame.width,
            static_cast<std::size_t>(frame.width));
    }
    for (int row = 0; row < chromaHeight; ++row) {
        std::memcpy(
            img.planes[1] + static_cast<std::size_t>(row) * img.stride[1],
            frame.u.data() + static_cast<std::size_t>(row) * chromaWidth,
            static_cast<std::size_t>(chromaWidth));
        std::memcpy(
            img.planes[2] + static_cast<std::size_t>(row) * img.stride[2],
            frame.v.data() + static_cast<std::size_t>(row) * chromaWidth,
            static_cast<std::size_t>(chromaWidth));
    }

    const uint64_t frameDuration = 90000 / static_cast<uint64_t>(std::max(1, m_impl->fps));
    const uint64_t pts = m_impl->nextPts;
    const long flags = forceKeyframe ? VPX_EFLAG_FORCE_KF : 0;

    const auto encodeResult =
        vpx_codec_encode(&m_impl->codec, &img, pts, frameDuration, flags, VPX_DL_REALTIME);
    if (encodeResult != VPX_CODEC_OK) {
        std::string message =
            std::string("vpx_codec_encode failed: ") +
            vpx_codec_err_to_string(encodeResult);
        if (const char* detail = vpx_codec_error_detail(&m_impl->codec);
            detail && *detail) {
            message += " (";
            message += detail;
            message += ")";
        }
        throw std::runtime_error(message);
    }

    EncodedFrame out;
    out.rtpTimestamp = static_cast<uint32_t>(pts);

    vpx_codec_iter_t iter = nullptr;
    const vpx_codec_cx_pkt_t* pkt = nullptr;
    while ((pkt = vpx_codec_get_cx_data(&m_impl->codec, &iter)) != nullptr) {
        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
            const auto* buf = static_cast<const uint8_t*>(pkt->data.frame.buf);
            out.data.assign(buf, buf + pkt->data.frame.sz);
            out.keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
            break;
        }
    }

    ++m_impl->frameCount;
    m_impl->nextPts += frameDuration;
    return out;
}

bool Vp8Encoder::reconfigure(int fps,
    int bitrateKbps,
    int cpuUsed,
    int minQuantizer,
    int maxQuantizer) {
    if (!m_impl || !m_impl->initialized) {
        return false;
    }

    fps = std::max(1, fps);
    bitrateKbps = std::max(100, bitrateKbps);
    cpuUsed = std::max(0, std::min(16, cpuUsed));
    minQuantizer = std::max(0, std::min(63, minQuantizer));
    maxQuantizer = std::max(0, std::min(63, maxQuantizer));
    if (minQuantizer > maxQuantizer) {
        std::swap(minQuantizer, maxQuantizer);
    }

    vpx_codec_enc_cfg_t newCfg = m_impl->cfg;
    newCfg.rc_target_bitrate = bitrateKbps;
    newCfg.rc_min_quantizer = minQuantizer;
    newCfg.rc_max_quantizer = maxQuantizer;
    newCfg.kf_max_dist = std::max(3, fps * 10);

    if (vpx_codec_enc_config_set(&m_impl->codec, &newCfg) != VPX_CODEC_OK) {
        return false;
    }

    m_impl->cfg = newCfg;
    m_impl->fps = fps;
    m_impl->bitrateKbps = bitrateKbps;
    m_impl->cpuUsed = cpuUsed;
    m_impl->minQuantizer = minQuantizer;
    m_impl->maxQuantizer = maxQuantizer;

    vpx_codec_control(&m_impl->codec, VP8E_SET_CPUUSED, cpuUsed);
    vpx_codec_control(&m_impl->codec, VP8E_SET_NOISE_SENSITIVITY, 0);
    vpx_codec_control(&m_impl->codec, VP8E_SET_STATIC_THRESHOLD, 0);
    vpx_codec_control(&m_impl->codec, VP8E_SET_SCREEN_CONTENT_MODE, 1);
    return true;
}