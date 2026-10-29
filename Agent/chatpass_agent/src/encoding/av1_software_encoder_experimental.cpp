#include "av1_software_encoder_experimental.h"
#include <aom/aom_encoder.h>
#include <aom/aom_image.h>
#include <aom/aomcx.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace hi5 {
struct SoftwareAv1EncoderExperiment::Impl {
    aom_codec_ctx_t codec{};
    aom_codec_enc_cfg_t cfg{};
    aom_image_t image{};
    bool initialized = false;
    bool imageReady = false;
    uint64_t frameNumber = 0;
    int width = 0, height = 0, fps = 20;
};
SoftwareAv1EncoderExperiment::SoftwareAv1EncoderExperiment() = default;
SoftwareAv1EncoderExperiment::~SoftwareAv1EncoderExperiment() { shutdown(); }
void SoftwareAv1EncoderExperiment::shutdown() {
    if (!impl_) return;
    if (impl_->initialized) aom_codec_destroy(&impl_->codec);
    if (impl_->imageReady) aom_img_free(&impl_->image);
    impl_.reset();
}
bool SoftwareAv1EncoderExperiment::initialize(int width, int height, int fps,
                                               int bitrateKbps, int threads,
                                               std::string* error) {
    shutdown();
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        shutdown();
        return false;
    };
    if (width <= 0 || height <= 0 || width > 3840 || height > 2160 ||
        (width % 2) || (height % 2) || fps < 1 || fps > 60 ||
        bitrateKbps < 100 || bitrateKbps > 20000 || threads < 1 || threads > 16)
        return fail("unsupported experimental AV1 frame or bitrate configuration");
    impl_ = std::make_unique<Impl>();
    impl_->width = width;
    impl_->height = height;
    impl_->fps = fps;
    if (aom_codec_enc_config_default(aom_codec_av1_cx(), &impl_->cfg, AOM_USAGE_REALTIME) != AOM_CODEC_OK)
        return fail("libaom real-time configuration unavailable");
    auto& c = impl_->cfg;
    c.g_w = static_cast<unsigned int>(width);
    c.g_h = static_cast<unsigned int>(height);
    c.g_threads = static_cast<unsigned int>(threads);
    c.g_profile = 0; // I420 8-bit; upgrade to I444 only after decode tests.
    c.g_input_bit_depth = 8;
    c.g_timebase.num = 1;
    c.g_timebase.den = 1000;
    c.g_lag_in_frames = 0;
    c.g_pass = AOM_RC_ONE_PASS;
    c.rc_end_usage = AOM_CBR;
    c.rc_target_bitrate = static_cast<unsigned int>(bitrateKbps);
    c.rc_min_quantizer = 10;
    c.rc_max_quantizer = 52;
    c.kf_mode = AOM_KF_DISABLED; // keyframes requested by caller.
    if (aom_codec_enc_init(&impl_->codec, aom_codec_av1_cx(), &c, 0) != AOM_CODEC_OK)
        return fail("libaom encoder initialization failed");
    impl_->initialized = true;
    // Chrome Remote Desktop deliberately uses fast realtime AV1 with
    // screen-content optimisations. Never block production if optional
    // libaom controls are absent on a particular version.
    aom_codec_control(&impl_->codec, AOME_SET_CPUUSED, 11);
    aom_codec_control(&impl_->codec, AV1E_SET_AQ_MODE, 3);
    aom_codec_control(&impl_->codec, AV1E_SET_TUNE_CONTENT, AOM_CONTENT_SCREEN);
    aom_codec_control(&impl_->codec, AV1E_SET_ROW_MT, threads > 1 ? 1 : 0);
    aom_codec_control(&impl_->codec, AV1E_SET_ENABLE_TPL_MODEL, 0);
    aom_codec_control(&impl_->codec, AV1E_SET_DELTAQ_MODE, 0);
    aom_codec_control(&impl_->codec, AV1E_SET_ENABLE_ORDER_HINT, 0);
    if (threads >= 4) aom_codec_control(&impl_->codec, AV1E_SET_TILE_COLUMNS, 1);
    if (threads >= 8) aom_codec_control(&impl_->codec, AV1E_SET_TILE_ROWS, 1);
    if (!aom_img_alloc(&impl_->image, AOM_IMG_FMT_I420,
                       static_cast<unsigned int>(width),
                       static_cast<unsigned int>(height), 1))
        return fail("libaom I420 image allocation failed");
    impl_->imageReady = true;
    return true;
}
bool SoftwareAv1EncoderExperiment::encode(const SoftwareAv1Frame& frame, bool keyframe,
                                          std::vector<SoftwareAv1Packet>& packets,
                                          std::string* error) {
    packets.clear();
    if (!impl_ || !impl_->initialized || !impl_->imageReady ||
        frame.width != impl_->width || frame.height != impl_->height) {
        if (error) *error = "AV1 encoder not initialized or frame dimensions changed";
        return false;
    }
    const auto w = static_cast<size_t>(frame.width);
    const auto h = static_cast<size_t>(frame.height);
    if (frame.y.size() != w*h || frame.u.size() != w*h/4 || frame.v.size() != w*h/4) {
        if (error) *error = "invalid I420 frame plane sizes";
        return false;
    }
    const auto copyPlane = [](const std::vector<uint8_t>& source, uint8_t* dest,
                              int stride, size_t rowWidth, size_t rows) {
        for (size_t i=0; i<rows; ++i)
            std::memcpy(dest + i*static_cast<size_t>(stride),
                        source.data() + i*rowWidth, rowWidth);
    };
    copyPlane(frame.y, impl_->image.planes[0], impl_->image.stride[0], w, h);
    copyPlane(frame.u, impl_->image.planes[1], impl_->image.stride[1], w/2, h/2);
    copyPlane(frame.v, impl_->image.planes[2], impl_->image.stride[2], w/2, h/2);
    const uint64_t timeMs = (1000ull*impl_->frameNumber) / static_cast<uint64_t>(impl_->fps);
    const unsigned long duration = static_cast<unsigned long>(
        std::max<uint64_t>(1, 1000ull / static_cast<uint64_t>(impl_->fps)));
    const aom_enc_frame_flags_t flags = keyframe ? AOM_EFLAG_FORCE_KF : 0;
    const aom_codec_err_t result = aom_codec_encode(
        &impl_->codec, &impl_->image, static_cast<aom_codec_pts_t>(timeMs),
        duration, flags);
    if (result != AOM_CODEC_OK) {
        if (error) *error = std::string("libaom encode failed: ") +
            aom_codec_error(&impl_->codec);
        return false;
    }
    ++impl_->frameNumber;
    aom_codec_iter_t it = nullptr;
    const aom_codec_cx_pkt_t* packet = nullptr;
    while ((packet = aom_codec_get_cx_data(&impl_->codec, &it)) != nullptr) {
        if (packet->kind != AOM_CODEC_CX_FRAME_PKT) continue;
        const auto* bytes = static_cast<const uint8_t*>(packet->data.frame.buf);
        const auto size = static_cast<size_t>(packet->data.frame.sz);
        if (!bytes || size == 0 || size > 4u*1024u*1024u) {
            if (error) *error = "invalid AV1 encoded packet length";
            packets.clear();
            return false;
        }
        SoftwareAv1Packet p;
        p.bytes.assign(bytes, bytes + size);
        p.keyframe = (packet->data.frame.flags & AOM_FRAME_IS_KEY) != 0;
        packets.push_back(std::move(p));
    }
    return true;
}
} // namespace hi5
