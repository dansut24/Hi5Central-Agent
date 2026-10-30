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
// Explicit FPS selection is a target for an active changing desktop, not
// a forced rate on a static screen and not a guarantee of delivered frames.
inline CaptureFrameRateDecision ApplyManualCaptureFps(
    CaptureFrameRateDecision decision, int manualFps, int maxFps) {
    if (decision.mode != 0 &&
        (manualFps == 15 || manualFps == 30 ||
         manualFps == 45 || manualFps == 60))
        decision.targetFps = std::min(manualFps, std::max(1, maxFps));
    return decision;
}
// WGC exposes compositor frame availability independently of screen motion.
// An explicit Viewer FPS selection is a polling ceiling even when the
// compositor has temporarily reported no new frame. Otherwise a 2-FPS idle
// downshift can delay the *next* fresh WGC frame by up to 500 ms.
// This path is NORMAL desktop WGC only: do not force GDI, DXGI, secure/UAC,
// Automatic sessions, or a static desktop to encode duplicate video frames.
inline CaptureFrameRateDecision ApplyWgcManualPollingFloor(
    CaptureFrameRateDecision decision,
    int manualFps,
    int maxFps,
    bool wgcActive,
    bool secureDesktop) {
    if (wgcActive && !secureDesktop &&
        (manualFps == 15 || manualFps == 30 ||
         manualFps == 45 || manualFps == 60)) {
        decision.targetFps = std::min(manualFps, std::max(1, maxFps));
    }
    return decision;
}
// Explicit session ceiling. Older endpoint installers wrote
// HI5_STREAM_FPS=20 into machine environment; that legacy Automatic default
// cannot constrain newer manual Viewer selections. Round an administrator's
// optional ceiling DOWN to a supported manual rate.
inline int SelectManualSessionCeilingFps(int requested) {
    if (requested >= 60) return 60;
    if (requested >= 45) return 45;
    if (requested >= 30) return 30;
    return 15;
}
} // namespace hi5
