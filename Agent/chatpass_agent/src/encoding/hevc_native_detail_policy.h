#pragma once
// Isolated hardware-HEVC desktop fidelity policy. Unlike AV1's software
// quality preset, this policy must never silently downscale a native 1080p
// screen when the opt-in flag and the authenticated Viewer request permit it.
// It does not change codec negotiation or the default 720p safety baseline.
#include <algorithm>

namespace hi5::quality {

struct HevcNativeDetailProfile {
    int maxWidth = 1280;
    int maxHeight = 720;
    int bitrateKbps = 4500;
    int encoderFps = 20;
    bool enabled = false;
};

inline HevcNativeDetailProfile ChooseHevcNativeDetailProfile(
    bool optIn, int viewerMaxWidth, int viewerMaxHeight,
    int sourceWidth, int sourceHeight, int senderMaxFps, int viewerManualFps,
    int senderBitrateKbps, int viewerTargetBitrateKbps) {
    HevcNativeDetailProfile p;
    p.bitrateKbps = std::clamp(senderBitrateKbps, 500, 4500);
    p.encoderFps = std::clamp(senderMaxFps, 1, 20);
    if (!optIn || sourceWidth <= 0 || sourceHeight <= 0) return p;
    // 'native' is represented as (0,0), and the uninitialised inherited
    // preference as (-1,-1). Explicit 720p must always remain respected.
    const bool wantsNative = viewerMaxWidth <= 0 && viewerMaxHeight <= 0;
    const bool wants1080p = viewerMaxWidth >= 1920 && viewerMaxHeight >= 1080;
    if (!wantsNative && !wants1080p) return p;
    // Do not raise resolution for an already-small source.
    if (sourceWidth <= 1280 && sourceHeight <= 720) return p;
    p.maxWidth = 1920;
    p.maxHeight = 1080;
    p.enabled = true;
    // Only a manual Viewer FPS selection may exceed the proven 20 FPS
    // automatic ceiling. Even then, never exceed the Agent sender maximum.
    p.encoderFps = viewerManualFps > 0
        ? std::clamp(viewerManualFps, 1, std::max(1, senderMaxFps))
        : std::clamp(senderMaxFps, 1, 20);
    // Honour the authenticated Viewer's adaptive target when provided;
    // avoid forcing a 6 Mbps stream over a genuinely constrained link.
    const int requestedBitrate = viewerTargetBitrateKbps > 0
        ? viewerTargetBitrateKbps : std::max(senderBitrateKbps, 6000);
    p.bitrateKbps = std::clamp(requestedBitrate, 1500, 8000);
    return p;
}
// An already-open hardware MFT retains the frame-rate negotiated at
// initialization. A Viewer change from 20 to 30/45/60 must not leave its
// nominal encoder cadence stuck at 20. Restart ONCE when this explicit
// frame-rate preference changes; bitrate-only feedback never calls for a
// destructive encoder recreation.
inline bool ShouldRestartHevcForFps(bool hevc, bool encoderExists,
                                   int requestedFps, int appliedFps) {
    return hevc && encoderExists && requestedFps > 0 && appliedFps > 0 &&
           requestedFps != appliedFps;
}

} // namespace hi5::quality
