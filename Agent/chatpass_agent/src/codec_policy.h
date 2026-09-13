#pragma once

#include <string>

namespace hi5 {

struct CodecDecision {
    std::string requestedMode;
    std::string selectedCodec;
    std::string encoder;
    int fps = 30;
    int bitrateKbps = 16000;
    bool hardware = false;
    bool stable = true;
    std::string reason;
};

CodecDecision SelectCodecPolicy();

} // namespace hi5