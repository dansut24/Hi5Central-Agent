#pragma once
// Experimental libaom software encoder based on public Chromium/CRD design
// principles: realtime, speed 11, screen-content tuning and zero lookahead.
// Not linked to the production Agent or its WebRTC sender.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hi5 {
struct SoftwareAv1Frame {
    int width = 0, height = 0;
    std::vector<uint8_t> y, u, v;
};
struct SoftwareAv1Packet {
    std::vector<uint8_t> bytes;
    bool keyframe = false;
};
class SoftwareAv1EncoderExperiment {
public:
    SoftwareAv1EncoderExperiment();
    ~SoftwareAv1EncoderExperiment();
    SoftwareAv1EncoderExperiment(const SoftwareAv1EncoderExperiment&) = delete;
    SoftwareAv1EncoderExperiment& operator=(const SoftwareAv1EncoderExperiment&) = delete;
    bool initialize(int width, int height, int fps, int bitrateKbps, int threads,
                    std::string* error);
    bool encode(const SoftwareAv1Frame& frame, bool keyframe,
                std::vector<SoftwareAv1Packet>& packets, std::string* error);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hi5
