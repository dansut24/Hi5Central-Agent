#pragma once

#include "frame_source.h"

#include <cstdint>
#include <string>
#include <vector>

struct H264EncodedFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint32_t timestamp90k = 0;
};

class H264MfEncoder {
public:
    H264MfEncoder();
    ~H264MfEncoder();

    bool init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error = nullptr);
    void shutdown();

    bool encode(const I420Frame& frame, bool forceKeyframe, H264EncodedFrame& out, std::string* error = nullptr);

    bool isOpen() const { return m_open; }
    std::string encoderName() const { return m_encoderName; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_open = false;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 20;
    int m_bitrateKbps = 6000;
    uint64_t m_frameIndex = 0;
    std::string m_encoderName;
};
