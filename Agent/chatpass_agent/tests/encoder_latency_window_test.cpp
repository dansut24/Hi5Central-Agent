#include "../src/encoding/encoder_latency_window.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
    EncoderLatencyWindow w;
    assert(w.percentile95() == 0.0 && w.count() == 0);
    for (int i = 1; i <= 100; ++i) w.record(static_cast<double>(i));
    assert(w.count() == 100);
    assert(w.percentile95() == 95.0);
    w.record(-1.0);
    w.record(NAN);
    assert(w.count() == 100);
    // When full, the rolling window contains only the newest 256 samples.
    for (int i = 101; i <= 400; ++i) w.record(static_cast<double>(i));
    assert(w.count() == 256);
    assert(w.percentile95() == 388.0);
    w.clear();
    assert(w.count() == 0 && w.percentile95() == 0.0);
    w.record(13.5);
    assert(w.percentile95() == 13.5);
    std::cout << "bounded encoder latency p95: passed\n";
}
