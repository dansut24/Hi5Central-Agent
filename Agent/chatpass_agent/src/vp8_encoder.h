#pragma once

#include "frame_source.h"

#include <cstdint>
#include <vector>

struct EncodedFrame {
    std::vector<uint8_t> data;
    uint32_t rtpTimestamp = 0;
    bool keyframe = false;
};

class Vp8Encoder {
public:
    // Extra tuning parameters are optional so existing call sites remain valid.
    // cpuUsed: VP8 realtime speed setting. Higher is faster/lower CPU, with some quality trade-off.
    // min/max quantizer: keeps the desktop readable while allowing low-CPU profiles to relax quality slightly.
    Vp8Encoder(int width,
        int height,
        int fps,
        int bitrateKbps,
        int cpuUsed = 6,
        int minQuantizer = 4,
        int maxQuantizer = 34,
        int threads = 2);
    ~Vp8Encoder();

    EncodedFrame encode(const I420Frame& frame, bool forceKeyframe = false);

    // Reconfigure bitrate/FPS/speed without destroying the codec. This avoids
    // a large wake-from-idle spike caused by recreating libvpx when the stream
    // changes from idle to active/motion. Width/height changes still require a
    // new encoder instance.
    bool reconfigure(int fps,
        int bitrateKbps,
        int cpuUsed = 6,
        int minQuantizer = 4,
        int maxQuantizer = 34);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
