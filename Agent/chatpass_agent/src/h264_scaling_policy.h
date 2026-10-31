#pragma once
// H.264 filtered 720p is independently opt-in and keeps the proven
// Level 3.1 negotiation. Native 1080p sharp-text remains a separate Level 4
// trial and must never be enabled merely to choose the bilinear scaler.
namespace hi5::quality {
inline bool UseFilteredH264Scaling(bool sharp1080, bool sharpFilter, bool filtered720) {
    return sharp1080 ? sharpFilter : filtered720;
}
} // namespace hi5::quality
