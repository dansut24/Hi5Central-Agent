#include "encoding/vp8_encoder_adapter.h"

namespace hi5 {

Vp8EncoderAdapter::Vp8EncoderAdapter(const VideoEncoderConfig& cfg)
    : m_encoder(std::make_unique<Vp8Encoder>(cfg.width, cfg.height, cfg.fps, cfg.bitrateKbps)) {
}

VideoEncodedFrame Vp8EncoderAdapter::Encode(const I420Frame& frame, bool forceKeyframe) {
    const bool keyframe = forceKeyframe || m_forceKeyframe;
    m_forceKeyframe = false;

    EncodedFrame vp8 = m_encoder->encode(frame, keyframe);

    VideoEncodedFrame out;
    out.data = std::move(vp8.data);
    out.rtpTimestamp = vp8.rtpTimestamp;
    out.keyframe = vp8.keyframe;
    out.codec = VideoCodecKind::VP8;
    return out;
}

} // namespace hi5


std::unique_ptr<IVideoEncoder> CreateVp8VideoEncoder(const VideoEncoderConfig& cfg,
                                                     std::string* selectedName) {
    auto enc = std::make_unique<Vp8EncoderAdapter>(cfg);
    if (selectedName) *selectedName = enc->Name();
    return enc;
}
