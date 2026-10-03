#pragma once

#include "remote_frame.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hi5 {

enum class SoftwareVideoCodec {
    VP8,
    VP9,
    AV1,
};

struct SoftwareEncodedFrame {
    std::vector<std::uint8_t> data;
    std::uint32_t rtpTimestamp = 0;
    bool keyframe = false;
};

class SoftwareVideoEncoder {
public:
    SoftwareVideoEncoder(
        SoftwareVideoCodec codec,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int threads = 0);
    ~SoftwareVideoEncoder();

    SoftwareVideoEncoder(const SoftwareVideoEncoder&) = delete;
    SoftwareVideoEncoder& operator=(const SoftwareVideoEncoder&) = delete;

    SoftwareEncodedFrame encode(const I420Frame& frame, bool forceKeyframe = false);

    SoftwareVideoCodec codec() const;
    std::string codecName() const;
    int width() const;
    int height() const;
    int fps() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hi5
