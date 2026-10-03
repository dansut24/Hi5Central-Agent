#include "remote_software_video_encoder.h"

#include <aom/aom_encoder.h>
#include <aom/aom_image.h>
#include <aom/aomcx.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace hi5 {
namespace {

int resolvedThreads(int requested) {
    if (requested > 0) return std::clamp(requested, 1, 8);
    const auto hw = static_cast<int>(std::thread::hardware_concurrency());
    return std::clamp(hw > 0 ? hw : 2, 1, 8);
}

std::uint64_t frameDuration90k(int fps) {
    return 90000ULL / static_cast<std::uint64_t>(std::max(1, fps));
}

} // namespace

struct SoftwareVideoEncoder::Impl {
    SoftwareVideoCodec codec = SoftwareVideoCodec::VP8;
    int width = 0;
    int height = 0;
    int fps = 30;
    int bitrateKbps = 6000;
    int threads = 2;
    std::uint64_t nextPts = 0;

    vpx_codec_ctx_t vpx{};
    vpx_codec_enc_cfg_t vpxCfg{};
    bool vpxInitialized = false;

    aom_codec_ctx_t aom{};
    aom_codec_enc_cfg_t aomCfg{};
    bool aomInitialized = false;

    Impl(
        SoftwareVideoCodec codecIn,
        int widthIn,
        int heightIn,
        int fpsIn,
        int bitrateIn,
        int threadsIn)
        : codec(codecIn),
          width(widthIn & ~1),
          height(heightIn & ~1),
          fps(std::max(1, fpsIn)),
          bitrateKbps(std::max(250, bitrateIn)),
          threads(resolvedThreads(threadsIn)) {

        if (width < 2 || height < 2) {
            throw std::runtime_error("Invalid software video encoder dimensions.");
        }

        if (codec == SoftwareVideoCodec::AV1) configureAv1();
        else configureVpx();
    }

    ~Impl() {
        if (vpxInitialized) vpx_codec_destroy(&vpx);
        if (aomInitialized) aom_codec_destroy(&aom);
    }

    void configureVpx() {
        vpx_codec_iface_t* iface =
            codec == SoftwareVideoCodec::VP9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();

        if (vpx_codec_enc_config_default(iface, &vpxCfg, 0) != VPX_CODEC_OK) {
            throw std::runtime_error("libvpx default encoder configuration failed.");
        }

        vpxCfg.g_w = static_cast<unsigned int>(width);
        vpxCfg.g_h = static_cast<unsigned int>(height);
        vpxCfg.g_timebase.num = 1;
        vpxCfg.g_timebase.den = 90000;
        vpxCfg.g_threads = static_cast<unsigned int>(threads);
        vpxCfg.g_pass = VPX_RC_ONE_PASS;
        vpxCfg.g_lag_in_frames = 0;
        vpxCfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
        vpxCfg.rc_end_usage = VPX_CBR;
        vpxCfg.rc_target_bitrate = static_cast<unsigned int>(bitrateKbps);
        vpxCfg.rc_resize_allowed = 0;
        vpxCfg.rc_dropframe_thresh = 0;
        vpxCfg.rc_min_quantizer = codec == SoftwareVideoCodec::VP9 ? 8 : 4;
        vpxCfg.rc_max_quantizer = codec == SoftwareVideoCodec::VP9 ? 45 : 36;
        vpxCfg.rc_buf_initial_sz = 400;
        vpxCfg.rc_buf_optimal_sz = 600;
        vpxCfg.rc_buf_sz = 1000;
        vpxCfg.kf_mode = VPX_KF_AUTO;
        vpxCfg.kf_min_dist = 0;
        vpxCfg.kf_max_dist = static_cast<unsigned int>(std::max(3, fps * 3));

        if (vpx_codec_enc_init(&vpx, iface, &vpxCfg, 0) != VPX_CODEC_OK) {
            throw std::runtime_error("libvpx encoder initialization failed.");
        }
        vpxInitialized = true;

        // This control applies to both VP8 and VP9 encoders in libvpx.
        const int cpuUsed = codec == SoftwareVideoCodec::VP9 ? 8 : 7;
        vpx_codec_control(&vpx, VP8E_SET_CPUUSED, cpuUsed);
        if (codec == SoftwareVideoCodec::VP8) {
            vpx_codec_control(&vpx, VP8E_SET_NOISE_SENSITIVITY, 0);
            vpx_codec_control(&vpx, VP8E_SET_STATIC_THRESHOLD, 0);
        }
    }

    void configureAv1() {
        if (aom_codec_enc_config_default(aom_codec_av1_cx(), &aomCfg, 0) != AOM_CODEC_OK) {
            throw std::runtime_error("libaom default encoder configuration failed.");
        }

        aomCfg.g_w = static_cast<unsigned int>(width);
        aomCfg.g_h = static_cast<unsigned int>(height);
        aomCfg.g_timebase.num = 1;
        aomCfg.g_timebase.den = 90000;
        aomCfg.g_threads = static_cast<unsigned int>(threads);
        aomCfg.g_pass = AOM_RC_ONE_PASS;
        aomCfg.g_lag_in_frames = 0;
        aomCfg.g_error_resilient = AOM_ERROR_RESILIENT_DEFAULT;
        aomCfg.rc_end_usage = AOM_CBR;
        aomCfg.rc_target_bitrate = static_cast<unsigned int>(bitrateKbps);
        aomCfg.rc_dropframe_thresh = 0;
        aomCfg.rc_min_quantizer = 12;
        aomCfg.rc_max_quantizer = 48;
        aomCfg.rc_buf_initial_sz = 400;
        aomCfg.rc_buf_optimal_sz = 600;
        aomCfg.rc_buf_sz = 1000;
        aomCfg.kf_mode = AOM_KF_AUTO;
        aomCfg.kf_min_dist = 0;
        aomCfg.kf_max_dist = static_cast<unsigned int>(std::max(3, fps * 3));

        if (aom_codec_enc_init(&aom, aom_codec_av1_cx(), &aomCfg, 0) != AOM_CODEC_OK) {
            throw std::runtime_error("libaom AV1 encoder initialization failed.");
        }
        aomInitialized = true;

        aom_codec_control(&aom, AOME_SET_CPUUSED, 8);
#ifdef AV1E_SET_ROW_MT
        aom_codec_control(&aom, AV1E_SET_ROW_MT, 1);
#endif
    }

    SoftwareEncodedFrame encodeVpx(const I420Frame& frame, bool forceKeyframe) {
        vpx_image_t image{};
        if (!vpx_img_wrap(
                &image,
                VPX_IMG_FMT_I420,
                static_cast<unsigned int>(width),
                static_cast<unsigned int>(height),
                1,
                nullptr)) {
            throw std::runtime_error("libvpx image wrapper failed.");
        }

        image.planes[0] = const_cast<unsigned char*>(frame.y.data());
        image.planes[1] = const_cast<unsigned char*>(frame.u.data());
        image.planes[2] = const_cast<unsigned char*>(frame.v.data());
        image.stride[0] = width;
        image.stride[1] = width / 2;
        image.stride[2] = width / 2;

        const auto duration = frameDuration90k(fps);
        const long flags = forceKeyframe ? VPX_EFLAG_FORCE_KF : 0;
        if (vpx_codec_encode(
                &vpx,
                &image,
                nextPts,
                duration,
                flags,
                VPX_DL_REALTIME) != VPX_CODEC_OK) {
            throw std::runtime_error("libvpx frame encode failed.");
        }

        SoftwareEncodedFrame out;
        out.rtpTimestamp = static_cast<std::uint32_t>(nextPts);

        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_cx_pkt_t* packet = nullptr;
        while ((packet = vpx_codec_get_cx_data(&vpx, &iter)) != nullptr) {
            if (packet->kind != VPX_CODEC_CX_FRAME_PKT) continue;
            const auto* bytes = static_cast<const std::uint8_t*>(packet->data.frame.buf);
            out.data.assign(bytes, bytes + packet->data.frame.sz);
            out.keyframe = (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
            break;
        }

        nextPts += duration;
        return out;
    }

    SoftwareEncodedFrame encodeAv1(const I420Frame& frame, bool forceKeyframe) {
        aom_image_t image{};
        if (!aom_img_wrap(
                &image,
                AOM_IMG_FMT_I420,
                static_cast<unsigned int>(width),
                static_cast<unsigned int>(height),
                1,
                nullptr)) {
            throw std::runtime_error("libaom image wrapper failed.");
        }

        image.planes[AOM_PLANE_Y] = const_cast<unsigned char*>(frame.y.data());
        image.planes[AOM_PLANE_U] = const_cast<unsigned char*>(frame.u.data());
        image.planes[AOM_PLANE_V] = const_cast<unsigned char*>(frame.v.data());
        image.stride[AOM_PLANE_Y] = width;
        image.stride[AOM_PLANE_U] = width / 2;
        image.stride[AOM_PLANE_V] = width / 2;

        const auto duration = frameDuration90k(fps);
        const aom_enc_frame_flags_t flags = forceKeyframe ? AOM_EFLAG_FORCE_KF : 0;
        if (aom_codec_encode(
                &aom,
                &image,
                static_cast<aom_codec_pts_t>(nextPts),
                static_cast<unsigned long>(duration),
                flags) != AOM_CODEC_OK) {
            throw std::runtime_error("libaom AV1 frame encode failed.");
        }

        SoftwareEncodedFrame out;
        out.rtpTimestamp = static_cast<std::uint32_t>(nextPts);

        aom_codec_iter_t iter = nullptr;
        const aom_codec_cx_pkt_t* packet = nullptr;
        while ((packet = aom_codec_get_cx_data(&aom, &iter)) != nullptr) {
            if (packet->kind != AOM_CODEC_CX_FRAME_PKT) continue;
            const auto* bytes = static_cast<const std::uint8_t*>(packet->data.frame.buf);
            out.data.assign(bytes, bytes + packet->data.frame.sz);
            out.keyframe = (packet->data.frame.flags & AOM_FRAME_IS_KEY) != 0;
            break;
        }

        nextPts += duration;
        return out;
    }
};

SoftwareVideoEncoder::SoftwareVideoEncoder(
    SoftwareVideoCodec codec,
    int width,
    int height,
    int fps,
    int bitrateKbps,
    int threads)
    : impl_(std::make_unique<Impl>(
          codec,
          width,
          height,
          fps,
          bitrateKbps,
          threads)) {}

SoftwareVideoEncoder::~SoftwareVideoEncoder() = default;

SoftwareEncodedFrame SoftwareVideoEncoder::encode(
    const I420Frame& frame,
    bool forceKeyframe) {

    if (!impl_ ||
        frame.width != impl_->width ||
        frame.height != impl_->height ||
        frame.y.size() < static_cast<std::size_t>(frame.width) * frame.height ||
        frame.u.size() < static_cast<std::size_t>(frame.width / 2) * (frame.height / 2) ||
        frame.v.size() < static_cast<std::size_t>(frame.width / 2) * (frame.height / 2)) {
        throw std::runtime_error("Invalid I420 frame supplied to software video encoder.");
    }

    return impl_->codec == SoftwareVideoCodec::AV1
        ? impl_->encodeAv1(frame, forceKeyframe)
        : impl_->encodeVpx(frame, forceKeyframe);
}

SoftwareVideoCodec SoftwareVideoEncoder::codec() const {
    return impl_->codec;
}

std::string SoftwareVideoEncoder::codecName() const {
    switch (impl_->codec) {
        case SoftwareVideoCodec::VP8: return "vp8";
        case SoftwareVideoCodec::VP9: return "vp9";
        case SoftwareVideoCodec::AV1: return "av1";
    }
    return "unknown";
}

int SoftwareVideoEncoder::width() const { return impl_->width; }
int SoftwareVideoEncoder::height() const { return impl_->height; }
int SoftwareVideoEncoder::fps() const { return impl_->fps; }

} // namespace hi5
