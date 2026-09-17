#pragma once

#include "frame_source.h"

#include <cstdint>
#include <string>
#include <vector>

struct H265EncodedFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint32_t timestamp90k = 0;
};

class H265MfEncoder {
public:
    H265MfEncoder();
    ~H265MfEncoder();

    bool init(int width, int height, int fps, int bitrateKbps,
        bool preferHardware, std::string* error = nullptr);
    bool encode(const I420Frame& frame, bool forceKeyframe,
        H265EncodedFrame& out, std::string* error = nullptr);
    void shutdown();

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
