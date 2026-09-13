#pragma once

#include "frame_source.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hi5 {

enum class VideoCodecKind {
    VP8
};

struct VideoEncoderConfig {
    int width = 0;
    int height = 0;
    int fps = 30;
    int bitrateKbps = 6000;
    VideoCodecKind codec = VideoCodecKind::VP8;
};

// VP8-only encoded frame for the supported libvpx streaming baseline.
struct VideoEncodedFrame {
    std::vector<uint8_t> data;
    uint32_t rtpTimestamp = 0;
    bool keyframe = false;
    VideoCodecKind codec = VideoCodecKind::VP8;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual VideoEncodedFrame Encode(const I420Frame& frame, bool forceKeyframe) = 0;
    virtual void ForceKeyframe() = 0;
    virtual VideoCodecKind Codec() const = 0;
    virtual const char* Name() const = 0;
};

std::unique_ptr<IVideoEncoder> CreateVp8VideoEncoder(const VideoEncoderConfig& cfg,
                                                     std::string* selectedName = nullptr);

} // namespace hi5
