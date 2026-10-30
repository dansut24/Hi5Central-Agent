#pragma once
// Frame-driven idle-to-motion pacing policy for Windows capture helpers.
// Poll rates are request ceilings, not promised delivered FPS. This helper
// intentionally has no platform dependencies so it can be regression tested.
#include <algorithm>
#include <chrono>

namespace hi5 {
struct CaptureFrameRateDecision {
    int targetFps = 2;
    int mode = 0; // 0 idle, 1 changed desktop, 2 recent remote input
};
inline CaptureFrameRateDecision SelectCaptureFrameRate(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point lastInput,
    std::chrono::steady_clock::time_point lastChanged,
    bool changedThisCapture,
    int requestedMaxFps,
    int activeFps,
    int motionFps,
    int idleFps,
    std::chrono::milliseconds inputHold,
    std::chrono::milliseconds motionHold) {
    const int maxFps = std::max(1, requestedMaxFps);
    const int active = std::clamp(activeFps, 1, maxFps);
    const int motion = std::clamp(motionFps, 1, maxFps);
    const int idle = std::clamp(idleFps, 1, maxFps);
    // Idle may poll every 500ms. A shorter motion hold can never survive
    // that interval, permanently trapping an animated desktop at idle FPS.
    const auto idlePoll = std::chrono::milliseconds(1000 / idle);
    const auto effectiveMotionHold = std::max(motionHold, idlePoll * 2);
    const bool input = lastInput.time_since_epoch().count() != 0 &&
        now >= lastInput && now - lastInput <= inputHold;
    const bool changing = changedThisCapture ||
        (lastChanged.time_since_epoch().count() != 0 &&
            now >= lastChanged && now - lastChanged <= effectiveMotionHold);
    if (input) return {motion, 2};
    if (changing) return {active, 1};
    return {idle, 0};
}
} // namespace hi5
