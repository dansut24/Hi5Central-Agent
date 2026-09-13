#pragma once

#include <string>
#include <vector>

namespace hi5 {

struct CodecEncoderCapability {
    std::string codec;          // h264, h265, vp8, vp9, av1
    std::string displayName;    // H.264, H.265/HEVC, etc.
    bool viewerLikelySupported = false;
    bool hardwareEncodeAvailable = false;
    bool softwareEncodeAvailable = false;
    std::vector<std::string> hardwareEncoders;
    std::vector<std::string> softwareEncoders;
    std::string recommendation;
};

struct CodecSelectionResult {
    std::string requestedMode;     // auto, vp8, h264_hw, etc.
    std::string selectedCodec;     // currently vp8 until H.264 sender path is implemented
    std::string selectedReason;
    bool hardwareH264Available = false;
    std::vector<CodecEncoderCapability> capabilities;
};

// Probes local Windows encoder availability. This is intentionally detection-only;
// the production stream still uses the current stable VP8 path until each codec
// sender path is implemented and enabled safely.
CodecSelectionResult ProbeCodecCapabilitiesAndSelect(const std::string& requestedMode);

std::string CodecCapabilitiesToLogString(const CodecSelectionResult& result);

} // namespace hi5
