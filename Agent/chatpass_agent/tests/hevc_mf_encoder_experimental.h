#pragma once

#include "vp8_encoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct HevcEncodedFrame {
    std::vector<uint8_t> data;
    uint32_t timestamp90k = 0;
    bool keyframe = false;
};

class HevcMfEncoder {
public:
    HevcMfEncoder();
    ~HevcMfEncoder();

    bool init(int width, int height, int fps, int bitrateKbps, bool preferHardware, std::string* error = nullptr);
    bool encode(const I420Frame& frame, bool forceKeyframe, HevcEncodedFrame& out, std::string* error = nullptr);
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
