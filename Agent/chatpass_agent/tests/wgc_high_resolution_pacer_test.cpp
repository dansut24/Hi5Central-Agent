#include "../src/wgc_high_resolution_pacer_win.h"
#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using Clock = std::chrono::steady_clock;
    hi5::WgcHighResolutionPacer pacer;
    // No system-wide timeBeginPeriod and no spin-loop. Each selected manual
    // rate can request one bounded high-resolution timer wait.
    for (int fps : {15, 30, 45, 60}) {
        const auto requested = std::chrono::nanoseconds(1000000000ll / fps);
        auto start = Clock::now();
        pacer.WaitUntil(start + requested);
        const auto elapsed = Clock::now() - start;
        assert(elapsed >= std::chrono::milliseconds(5));
        assert(elapsed < std::chrono::seconds(2));
    }
    // A passed deadline must be a no-op, not a blocking wait.
    const auto before = Clock::now();
    const bool used = pacer.WaitUntil(before - std::chrono::milliseconds(5));
    assert(!used);
    assert(Clock::now() - before < std::chrono::seconds(1));
    std::cout << "WGC_HIGH_RES_TIMER_TEST_PASSED available=" <<
        (pacer.Available() ? 1 : 0) <<
        " fallback_error=" << pacer.CreateError() << "\n";
}
