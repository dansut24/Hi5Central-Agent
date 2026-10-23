#include "vp9_vpx_encoder.h"
#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#include <algorithm>
#include <stdexcept>

struct Vp9VpxEncoder::Impl {
    vpx_codec_ctx_t codec{};
    vpx_codec_enc_cfg_t cfg{};
    bool initialized = false;
    bool lossless = false;
    int width, height, fps, bitrateKbps;
    uint64_t pts = 0;

    Impl(int w, int h, int f, int br)
      : width(w), height(h), fps(std::max(1, f)), bitrateKbps(std::max(500,br)) {
        if (w <= 0 || h <= 0 || (w % 2) || (h % 2))
            throw std::runtime_error("libvpx VP9 needs even I420 frame dimensions");
        if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK)
            throw std::runtime_error("libvpx VP9 default configuration failed");
        cfg.g_w = unsigned(w);
        cfg.g_h = unsigned(h);
        cfg.g_timebase.num = 1;
        cfg.g_timebase.den = 90000;
        cfg.g_threads = 2;
        cfg.g_lag_in_frames = 0;
        cfg.g_pass = VPX_RC_ONE_PASS;
        cfg.rc_end_usage = VPX_CBR;
        cfg.rc_target_bitrate = bitrateKbps;
        cfg.rc_resize_allowed = 0;
        cfg.rc_dropframe_thresh = 0;
        cfg.rc_min_quantizer = 0;
        cfg.rc_max_quantizer = 45;
        cfg.kf_mode = VPX_KF_AUTO;
        cfg.kf_max_dist = std::max(3, fps*10);
        #ifdef VPX_ERROR_RESILIENT_DEFAULT
        cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
        #else
        cfg.g_error_resilient = 1;
        #endif
        if (vpx_codec_enc_init(&codec, vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK)
            throw std::runtime_error("libvpx VP9 codec init failed");
        initialized = true;
        if (vpx_codec_control(&codec, VP8E_SET_CPUUSED, 7) != VPX_CODEC_OK)
            throw std::runtime_error("libvpx VP9 realtime control failed");
    }
    ~Impl() { if (initialized) vpx_codec_destroy(&codec); }
};

Vp9VpxEncoder::Vp9VpxEncoder(int w, int h, int fps, int bitrateKbps)
  : m_impl(std::make_unique<Impl>(w, h, fps, bitrateKbps)) {}
Vp9VpxEncoder::~Vp9VpxEncoder() = default;

bool Vp9VpxEncoder::reconfigure(int fps, int bitrateKbps) {
    if (!m_impl || !m_impl->initialized) return false;
    fps = std::max(1,fps);
    bitrateKbps = std::max(500,bitrateKbps);
    auto next = m_impl->cfg;
    next.rc_target_bitrate = bitrateKbps;
    next.kf_max_dist = std::max(3,fps*10);
    if (vpx_codec_enc_config_set(&m_impl->codec, &next) != VPX_CODEC_OK) return false;
    m_impl->cfg = next;
    m_impl->fps = fps;
    m_impl->bitrateKbps = bitrateKbps;
    return true;
}

Vp9EncodedFrame Vp9VpxEncoder::encode(const I420Frame& frame, bool keyframe, bool lossless) {
    if (!m_impl || !m_impl->initialized || frame.width != m_impl->width || frame.height != m_impl->height)
        throw std::runtime_error("libvpx VP9 input dimensions mismatch");
    const size_t y = size_t(frame.width)*size_t(frame.height);
    const size_t uv = size_t((frame.width+1)/2)*size_t((frame.height+1)/2);
    if (frame.y.size()<y || frame.u.size()<uv || frame.v.size()<uv)
        throw std::runtime_error("libvpx VP9 input buffer too small");

    const bool changed = lossless != m_impl->lossless;
    if (changed) {
        if (vpx_codec_control(&m_impl->codec, VP9E_SET_LOSSLESS, lossless ? 1 : 0) != VPX_CODEC_OK)
            throw std::runtime_error("libvpx VP9 lossless control rejected");
        m_impl->lossless = lossless;
    }

    vpx_image_t image{};
    if (!vpx_img_wrap(&image, VPX_IMG_FMT_I420, unsigned(frame.width), unsigned(frame.height), 1, nullptr))
        throw std::runtime_error("libvpx VP9 I420 image wrapping failed");
    image.planes[0] = const_cast<uint8_t*>(frame.y.data());
    image.planes[1] = const_cast<uint8_t*>(frame.u.data());
    image.planes[2] = const_cast<uint8_t*>(frame.v.data());
    image.stride[0] = frame.width;
    image.stride[1] = (frame.width+1)/2;
    image.stride[2] = (frame.width+1)/2;

    const uint64_t duration = 90000u / uint64_t(m_impl->fps);
    const uint64_t pts = m_impl->pts;
    if (vpx_codec_encode(&m_impl->codec, &image, pts, duration,
          keyframe || changed ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_REALTIME) != VPX_CODEC_OK)
        throw std::runtime_error("libvpx VP9 frame encoding failed");

    Vp9EncodedFrame output{};
    output.timestamp90k = uint32_t(pts);
    vpx_codec_iter_t it = nullptr;
    const vpx_codec_cx_pkt_t* packet = nullptr;
    while ((packet = vpx_codec_get_cx_data(&m_impl->codec, &it)) != nullptr) {
        if (packet->kind == VPX_CODEC_CX_FRAME_PKT) {
            const auto* bytes = static_cast<const uint8_t*>(packet->data.frame.buf);
            output.data.assign(bytes, bytes + packet->data.frame.sz);
            output.keyframe = (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
            break;
        }
    }
    m_impl->pts += duration;
    return output;
}
bool Vp9VpxEncoder::losslessActive() const {
    return m_impl && m_impl->initialized && m_impl->lossless;
}
