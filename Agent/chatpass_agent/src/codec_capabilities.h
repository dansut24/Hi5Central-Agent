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
    std::string requestedMode;     // auto, av1[_hw/_sw], vp9[_hw/_sw], h265, h264, vp8
    std::string selectedCodec;     // auto or the explicitly requested codec
    std::string selectedReason;
    bool hardwareH264Available = false;
    std::vector<CodecEncoderCapability> capabilities;
};

// Probes local Windows encoder availability. Runtime selection intersects this
// endpoint capability with the Viewer SDP answer and live encode-health telemetry.
CodecSelectionResult ProbeCodecCapabilitiesAndSelect(const std::string& requestedMode);

// Lightweight one-codec hardware probe used by adaptive Auto after Viewer SDP
// negotiation. Unlike the full diagnostics probe, this does not enumerate
// unrelated codecs or software MFTs.
#ifdef _WIN32
bool ProbeHardwareCodecAvailable(const std::string& codec, std::string* encoderName = nullptr);
#else
inline bool ProbeHardwareCodecAvailable(const std::string&, std::string* encoderName = nullptr) {
    if (encoderName) encoderName->clear();
    return false;
}
#endif

std::string CodecCapabilitiesToLogString(const CodecSelectionResult& result);

} // namespace hi5
