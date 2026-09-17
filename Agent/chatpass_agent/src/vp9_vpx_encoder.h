#pragma once

#include "vp9_mf_encoder.h"

#include <memory>
#include <string>

class Vp9VpxEncoder {
public:
    Vp9VpxEncoder(int width,
        int height,
        int fps,
        int bitrateKbps,
        int cpuUsed = 8,
        int minQuantizer = 4,
        int maxQuantizer = 38,
        int threads = 4);
    ~Vp9VpxEncoder();

    Vp9EncodedFrame encode(const I420Frame& frame, bool forceKeyframe);

    bool reconfigure(int fps,
        int bitrateKbps,
        int cpuUsed,
        int minQuantizer,
        int maxQuantizer);

    std::string encoderName() const { return "libvpx VP9 software"; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
