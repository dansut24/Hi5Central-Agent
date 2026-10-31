#pragma once
// Stamp outgoing RTP using real monotonic elapsed time, not the configured
// encoder FPS. A 60 FPS request can yield 30 actual encoded frames/sec,
// especially on shared desktops; frame-index RTP timestamps then advance at
// half wall-clock speed and allow multi-second browser jitter-buffer buildup.
// A clock belongs to ONE negotiated WebRTC sender/SSRC, across mode changes.
#include <chrono>
#include <cstdint>

class RtpVideoWallClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    uint32_t stamp(TimePoint now = Clock::now()) {
        if (!initialized_) {
            first_ = now;
            initialized_ = true;
            lastTicks_ = 0;
            return 0;
        }
        const auto elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - first_).count();
        const uint64_t elapsedTicks = elapsedUs <= 0 ? 0 :
            (static_cast<uint64_t>(elapsedUs) / 100) * 9 +
            (static_cast<uint64_t>(elapsedUs) % 100) * 9 / 100;
        // Do not reuse RTP timestamps when frames are encoded on the same
        // microsecond or when a monotonic clock has unusually coarse ticks.
        const uint64_t next = elapsedTicks > lastTicks_ ? elapsedTicks : lastTicks_ + 1;
        lastTicks_ = next;
        return static_cast<uint32_t>(lastTicks_); // RTP rollover is intentional.
    }

private:
    bool initialized_ = false;
    TimePoint first_{};
    uint64_t lastTicks_ = 0;
};
