#include "../src/rtp_video_wall_clock.h"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
using namespace std::chrono;
int main() {
 using Clock = RtpVideoWallClock::Clock;
 const auto start = Clock::now();
 RtpVideoWallClock rtp;
 assert(rtp.stamp(start) == 0);
 // Viewer requests 60 FPS but full-HD Dell produces ~32 FPS. RTP media time
 // must still advance one real-world second, not 32/60 seconds.
 for (int i = 1; i <= 32; ++i) {
   const uint32_t t = rtp.stamp(start + microseconds(i * 31250));
   assert(t == static_cast<uint32_t>(i * 2812 + i / 2));
 }
 assert(rtp.stamp(start + seconds(1)) == 90001u);
 // No new frames during idle: the first new frame MUST skip the entire
 // real idle period rather than back up the browser jitter buffer.
 assert(rtp.stamp(start + seconds(6)) == 540000u);
 // Two encoded frames with equal creation times must never reuse a timestamp.
 assert(rtp.stamp(start + seconds(6)) == 540001u);
 // Changing the encoder nominal FPS leaves the one RTP clock undisturbed.
 assert(rtp.stamp(start + seconds(7) + milliseconds(50)) == 634500u);
 // Validate natural 32-bit RTP wrap without resetting sender/SSRC.
 RtpVideoWallClock wrap;
 assert(wrap.stamp(start) == 0);
 const uint32_t before = wrap.stamp(start + hours(13));
 assert(before == static_cast<uint32_t>(46800ULL * 90000ULL));
 const uint32_t after = wrap.stamp(start + hours(14));
 assert(after == static_cast<uint32_t>(50400ULL * 90000ULL));
 assert(after != before);
 std::cout << "RTP_MONOTONIC_WALL_CLOCK_TESTS_PASSED\n";
}
