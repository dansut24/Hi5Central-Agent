#include "../src/encoding/auto_codec_cooldown.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    constexpr std::int64_t now = 1'800'000'000;
    assert(!hi5::Vp9AutoCooldownActive(0, now));
    assert(!hi5::Vp9AutoCooldownActive(now + 10, now));
    assert(hi5::Vp9AutoCooldownActive(now - 2 * 3600, now));
    assert(hi5::Vp9AutoCooldownActive(now - 23 * 3600, now));
    assert(!hi5::Vp9AutoCooldownActive(now - 24 * 3600, now));
    assert(hi5::Vp9AutoCooldownActive(now - 2 * 3600, now, 3));
    assert(!hi5::Vp9AutoCooldownActive(now - 4 * 3600, now, 3));

    hi5::CodecBenchmarkSample fast{};
    fast.vp8Samples = 6;
    fast.vp9Samples = 6;
    fast.vp8AverageMs = 14.0;
    fast.vp9AverageMs = 8.0;
    fast.vp9WorstMs = 13.0;
    assert(hi5::Vp9CompetitiveForAuto(fast));

    auto marginal = fast;
    marginal.vp9AverageMs = 19.0;
    marginal.vp9WorstMs = 23.0;
    assert(!hi5::Vp9CompetitiveForAuto(marginal));

    auto spikes = fast;
    spikes.vp9WorstMs = 50.0;
    assert(!hi5::Vp9CompetitiveForAuto(spikes));

    auto startupOnly = fast;
    startupOnly.vp9Samples = 2;
    assert(!hi5::Vp9CompetitiveForAuto(startupOnly));

    auto failed = fast;
    failed.vp9Failed = true;
    assert(!hi5::Vp9CompetitiveForAuto(failed));

    std::cout << "VP9 auto selection and persistent overload cooldown: 12 checks passed\n";
    return 0;
}
