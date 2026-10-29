#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Telemetry-only bounded rolling window: no allocations or frame copies on the
// encoding hot path. Sorting a small copy happens only at the 5-second log
// interval, never for every frame.
class EncoderLatencyWindow {
public:
    void record(double milliseconds) {
        if (!std::isfinite(milliseconds) || milliseconds < 0.0) return;
        samples_[next_] = milliseconds;
        next_ = (next_ + 1) % samples_.size();
        if (count_ < samples_.size()) ++count_;
    }

    double percentile95() const {
        if (!count_) return 0.0;
        auto values = samples_;
        const auto rank = (count_ * 95 + 99) / 100 - 1; // nearest rank, zero-based
        std::nth_element(values.begin(), values.begin() + rank, values.begin() + count_);
        return values[rank];
    }

    std::size_t count() const { return count_; }
    void clear() { next_ = 0; count_ = 0; }

private:
    std::array<double, 256> samples_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};
