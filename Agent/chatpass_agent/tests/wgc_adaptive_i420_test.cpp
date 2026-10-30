#include "../src/wgc_adaptive_i420.h"
#include <libyuv/cpu_id.h>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

void Check(int width, int height, int rowPadding) {
    const int stride = width * 4 + rowPadding;
    std::vector<uint8_t> bgra(static_cast<size_t>(stride) * height);
    for (size_t i = 0; i < bgra.size(); ++i)
        bgra[i] = static_cast<uint8_t>((i * 31u + (i / 7u) * 13u + 123u) & 255u);
    I420Frame baseline;
    I420Frame parallel;
    hi5::WgcAdaptiveI420Converter baselineConverter;
    hi5::WgcAdaptiveI420Converter parallelConverter;
    assert(baselineConverter.Convert(bgra.data(), stride, width, height, baseline, false));
    assert(parallelConverter.Convert(bgra.data(), stride, width, height, parallel, false));
    assert(hi5::ConvertBgraToI420TwoStrips(bgra.data(), stride, width, height, parallel) == 0);
    assert(baseline.y == parallel.y);
    assert(baseline.u == parallel.u);
    assert(baseline.v == parallel.v);
    // Production adaptive path must remain byte-identical on all 12
    // subsequent frames, including calibration and post-decision phases.
    for (int i = 0; i < 12; ++i) {
        I420Frame out;
        assert(parallelConverter.Convert(bgra.data(), stride, width, height, out, true));
        assert(baseline.y == out.y);
        assert(baseline.u == out.u);
        assert(baseline.v == out.v);
    }
    std::cout << "WGC_I420_PIXEL_EQUAL width=" << width << " height=" << height
              << " stride=" << stride
              << " selected=" << (parallelConverter.UsingParallel() ? "two_strip" : "serial")
              << " serial_ms=" << parallelConverter.SerialTrialAvgMs()
              << " parallel_ms=" << parallelConverter.ParallelTrialAvgMs() << "\n";
}
int main() {
    const int flags = libyuv::InitCpuFlags();
    std::cout << "WGC_LIBYUV_CPU_FLAGS=" << flags
              << " AVX2=" << (libyuv::TestCpuFlag(libyuv::kCpuHasAVX2) != 0)
              << " SSSE3=" << (libyuv::TestCpuFlag(libyuv::kCpuHasSSSE3) != 0) << "\n";
    Check(2560, 1600, 0);
    Check(1920, 1080, 64);
    Check(1280, 720, 0);
    Check(1023, 721, 16); // odd dimensions: known-good serial fallback
    std::cout << "WGC_I420_ADAPTIVE_PIXEL_TESTS_PASSED\n";
}
