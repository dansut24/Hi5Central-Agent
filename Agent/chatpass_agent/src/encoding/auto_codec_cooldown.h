#pragma once

#include <algorithm>
#include <cstdint>

namespace hi5 {

// A genuine sustained VP9 overload takes precedence over a short synthetic
// benchmark on subsequent Agent starts. No codec changes on established RTP
// tracks: this policy affects only future SDP offers.
inline bool Vp9AutoCooldownActive(std::int64_t lastOverloadEpochSeconds,
                                  std::int64_t nowEpochSeconds,
                                  int cooldownHours = 24) {
    if (lastOverloadEpochSeconds <= 0 || nowEpochSeconds < lastOverloadEpochSeconds)
        return false;
    const auto hours = std::max(1, std::min(168, cooldownHours));
    return nowEpochSeconds - lastOverloadEpochSeconds <
           static_cast<std::int64_t>(hours) * 3600;
}

struct CodecBenchmarkSample {
    double vp8AverageMs = 0.0;
    double vp9AverageMs = 0.0;
    double vp9WorstMs = 0.0;
    unsigned int vp8Samples = 0;
    unsigned int vp9Samples = 0;
    bool vp9Failed = false;
};

// Encode time is only part of the end-to-end frame budget. Leave room for
// capture, pixel conversion, RTP delivery and the viewer's decode/render.
// This tighter threshold avoids mistaking a barely-real-time synthetic test
// for sustained performance on an interactive desktop.
inline bool Vp9CompetitiveForAuto(const CodecBenchmarkSample& s) {
    if (s.vp9Failed || s.vp8Samples < 4 || s.vp9Samples < 4 ||
        s.vp8AverageMs <= 0.0 || s.vp9AverageMs <= 0.0) return false;
    const double relativeCeiling = std::max(7.5, s.vp8AverageMs * 1.35);
    return s.vp9AverageMs <= 16.0 &&
           s.vp9AverageMs <= relativeCeiling &&
           s.vp9WorstMs <= 30.0;
}

}  // namespace hi5
