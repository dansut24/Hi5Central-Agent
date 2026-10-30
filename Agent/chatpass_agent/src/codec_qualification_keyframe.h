#pragma once
// One bounded recovery-keyframe request during an opt-in qualification
// session. Ordinary keyframes, SDP negotiation and capture cadence are
// unchanged. Using monotonic wall time (rather than frame count) means an
// idle 2-FPS desktop can still prove a later decoded keyframe.
#include <chrono>

namespace hi5::universal_codec {
class SingleQualificationKeyframe {
    using Clock = std::chrono::steady_clock;
    Clock::time_point firstEligibleFrame_{};
    bool requested_ = false;
public:
    bool requestIfDue(bool eligible, Clock::time_point now) {
        if (!eligible || requested_) return false;
        if (firstEligibleFrame_.time_since_epoch().count() == 0) {
            firstEligibleFrame_ = now;
            return false;
        }
        if (now <= firstEligibleFrame_ ||
            now - firstEligibleFrame_ < std::chrono::seconds(20))
            return false;
        requested_ = true;
        return true;
    }
};
} // namespace hi5::universal_codec
