#pragma once

#include "remote_frame.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace hi5 {

struct H264EncodedFrame {
    std::vector<std::uint8_t> data;
    std::uint32_t rtpTimestamp = 0;
    bool keyframe = false;
};

class H264VideoToolboxEncoder {
public:
    H264VideoToolboxEncoder(int width, int height, int fps, int bitrateKbps);
    ~H264VideoToolboxEncoder();

    H264VideoToolboxEncoder(const H264VideoToolboxEncoder&) = delete;
    H264VideoToolboxEncoder& operator=(const H264VideoToolboxEncoder&) = delete;

    H264EncodedFrame encode(const I420Frame& frame, bool forceKeyframe);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hi5