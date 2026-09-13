#pragma once

#include "encoding/video_encoder.h"
#include "vp8_encoder.h"

#include <memory>

namespace hi5 {

class Vp8EncoderAdapter final : public IVideoEncoder {
public:
    explicit Vp8EncoderAdapter(const VideoEncoderConfig& cfg);
    ~Vp8EncoderAdapter() override = default;

    VideoEncodedFrame Encode(const I420Frame& frame, bool forceKeyframe) override;
    void ForceKeyframe() override { m_forceKeyframe = true; }
    VideoCodecKind Codec() const override { return VideoCodecKind::VP8; }
    const char* Name() const override { return "libvpx-vp8"; }

private:
    std::unique_ptr<Vp8Encoder> m_encoder;
    bool m_forceKeyframe = true;
};

} // namespace hi5
