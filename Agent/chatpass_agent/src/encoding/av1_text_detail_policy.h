#pragma once
// Isolated AV1 zoomed-text fidelity experiment. Explicit viewer 1080p
// request AND operator feature flag required; AV1 default remains 720p.
// Pure function so production/CI can verify fail-closed decisions.
#include <algorithm>

namespace hi5::quality {
struct Av1TextDetailProfile {
    int maxWidth = 1280;
    int maxHeight = 720;
    int bitrateKbps = 4500;
    bool enabled = false;
};
inline Av1TextDetailProfile ChooseAv1TextDetailProfile(
    bool isSoftwareAv1, bool operatorOptIn,
    int requestedWidth, int requestedHeight,
    int encoderBitrateKbps, int viewerRequestedBitrateKbps) {
    Av1TextDetailProfile profile;
    profile.bitrateKbps = std::clamp(encoderBitrateKbps, 500, 4500);
    // Do not infer 1080p from "auto" or "native": upgrading without an
    // explicit request could more than double software encoding work.
    if (!isSoftwareAv1 || !operatorOptIn ||
        requestedWidth != 1920 || requestedHeight != 1080)
        return profile;
    profile.maxWidth = 1920;
    profile.maxHeight = 1080;
    profile.bitrateKbps = std::clamp(
        std::max(encoderBitrateKbps, viewerRequestedBitrateKbps),
        4500, 6500);
    profile.enabled = true;
    return profile;
}
} // namespace hi5::quality
