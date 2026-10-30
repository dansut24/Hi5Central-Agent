#pragma once
// Future-proof, fail-closed frame-rate selection for universal codecs.
// FPS is a capability to be *qualified*, never inferred solely from GPU
// enumeration, synthetic encode benchmarks or a user-selected dropdown.
// No proof => preserve the existing 20 FPS safe ceiling.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace hi5::universal_codec {
struct FpsQualification {
    std::string codec;
    int testedFps = 0;
    int width = 0;
    int height = 0;
    int encodedFrames = 0;
    int decodedFrames = 0;
    int capturedFrames = 0;
    int recoveredKeyframes = 0;
    double measuredEncodeP95Ms = 0.0;
    double measuredEncodeWorstMs = 0.0;
    double decodedFps = 0.0;
    double droppedFraction = 1.0;
    double lossFraction = 1.0;
    bool actualViewer = false;
    bool rtpPacketizationVerified = false;
};
struct FrameRateChoice {
    int targetFps = 20;
    bool qualified = false;
    std::string reason = "safe 20 FPS until real frame rate proof";
};
inline bool fpsProven(const FpsQualification& q) {
    if (!q.actualViewer || !q.rtpPacketizationVerified ||
        q.testedFps < 15 || q.testedFps > 120 ||
        q.width < 320 || q.height < 240 ||
        q.recoveredKeyframes < 1 ||
        q.encodedFrames < q.testedFps * 2 ||
        q.capturedFrames < q.testedFps * 2 ||
        q.decodedFrames < q.testedFps * 2 ||
        q.decodedFps < q.testedFps * 0.90 ||
        !std::isfinite(q.droppedFraction) || q.droppedFraction > 0.02 ||
        !std::isfinite(q.lossFraction) || q.lossFraction > 0.015)
        return false;
    const double frameIntervalMs = 1000.0 / q.testedFps;
    return std::isfinite(q.measuredEncodeP95Ms) &&
        std::isfinite(q.measuredEncodeWorstMs) &&
        q.measuredEncodeP95Ms > 0.0 &&
        q.measuredEncodeP95Ms < frameIntervalMs * 0.75 &&
        q.measuredEncodeWorstMs > 0.0 &&
        q.measuredEncodeWorstMs < frameIntervalMs * 1.5;
}
inline FrameRateChoice selectAutomaticFps(
    const std::string& negotiatedCodec,
    const std::vector<FpsQualification>& qualifications,
    int viewerDisplayHz = 0,
    int agentHardwareLimit = 60,
    int viewerRequestedMaximum = 120) {
    FrameRateChoice result;
    result.targetFps = std::min({20, std::max(1, agentHardwareLimit),
                                  std::max(1, viewerRequestedMaximum)});
    // Do not advertise 120 on hardware or a Viewer still limited to 60.
    for (int step : {30, 45, 60, 90, 120}) {
        if (step > agentHardwareLimit || step > viewerRequestedMaximum)
            continue;
        if (step > 60 && (viewerDisplayHz <= 0 || step > viewerDisplayHz))
            continue;
        const bool passed = std::any_of(qualifications.begin(), qualifications.end(),
            [&](const FpsQualification& proof) {
                return proof.codec == negotiatedCodec && proof.testedFps >= step &&
                    fpsProven(proof);
            });
        if (passed) {
            result.targetFps = step;
            result.qualified = true;
            result.reason = "real capture+encoder+RTP+viewer measured frame rate qualified";
        }
    }
    return result;
}
} // namespace hi5::universal_codec
