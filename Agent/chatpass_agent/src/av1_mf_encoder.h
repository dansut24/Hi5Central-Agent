#pragma once

#include "vp8_encoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Av1EncodedFrame {
    std::vector<uint8_t> data;
    uint32_t timestamp90k = 0;
    bool keyframe = false;
};

class Av1MfEncoder {
public:
    Av1MfEncoder();
    ~Av1MfEncoder();

    bool init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error = nullptr);
    bool encode(const I420Frame& frame, bool forceKeyframe, Av1EncodedFrame& out, std::string* error = nullptr);
    void shutdown();

    std::string encoderName() const { return m_encoderName; }

private:
    struct Impl;
    Impl* m_impl = nullptr;

    bool m_open = false;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    int m_bitrateKbps = 4000;
    uint64_t m_frameIndex = 0;
    std::string m_encoderName;
};
