#include "vp8_encoder.h"

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
    bool initialized = false;
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

        if (vpx_codec_enc_init(&codec, vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) {
            throw std::runtime_error("vpx_codec_enc_init failed");
        }

        vpx_codec_control(&codec, VP8E_SET_CPUUSED, cpuUsed);
        vpx_codec_control(&codec, VP8E_SET_NOISE_SENSITIVITY, 0);
        vpx_codec_control(&codec, VP8E_SET_STATIC_THRESHOLD, 0);
        initialized = true;
    }

    ~Impl() {
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

    vpx_image_t img{};
    if (!vpx_img_wrap(&img, VPX_IMG_FMT_I420,
        static_cast<unsigned int>(frame.width),
        static_cast<unsigned int>(frame.height),
        1,
        nullptr)) {
        throw std::runtime_error("vpx_img_wrap failed");
    }

    img.planes[0] = const_cast<unsigned char*>(frame.y.data());
    img.planes[1] = const_cast<unsigned char*>(frame.u.data());
    img.planes[2] = const_cast<unsigned char*>(frame.v.data());
    img.stride[0] = frame.width;
    img.stride[1] = (frame.width + 1) / 2;
    img.stride[2] = (frame.width + 1) / 2;

    const uint64_t frameDuration = 90000 / static_cast<uint64_t>(std::max(1, m_impl->fps));
    const uint64_t pts = m_impl->nextPts;
    const long flags = forceKeyframe ? VPX_EFLAG_FORCE_KF : 0;

    if (vpx_codec_encode(&m_impl->codec, &img, pts, frameDuration, flags, VPX_DL_REALTIME) != VPX_CODEC_OK) {
        throw std::runtime_error("vpx_codec_encode failed");
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
    return true;
}
