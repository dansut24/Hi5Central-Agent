#pragma once
// In-session adaptive BGRA -> I420 for normal WGC frames only.
// Both halves use the SAME libyuv conversion as the proven serial path;
// even row boundaries preserve 4:2:0 pixel-for-pixel output. Never queue
// frames or make encoder/network latency dependent on background workers.
#include "frame_source.h"
#include <libyuv/convert.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace hi5 {

inline int ConvertBgraToI420Serial(const uint8_t* src, int stride, int width,
    int height, I420Frame& dst) {
    return libyuv::ARGBToI420(src, stride, dst.y.data(), width,
        dst.u.data(), (width + 1) / 2,
        dst.v.data(), (width + 1) / 2, width, height);
}

// Splits on an even row so no chroma sample straddles the two jobs.
// Joining in the same call preserves existing frame order and latency.
inline int ConvertBgraToI420TwoStrips(const uint8_t* src, int stride,
    int width, int height, I420Frame& dst) {
    if (height < 4 || (height & 1) || width < 2)
        return ConvertBgraToI420Serial(src, stride, width, height, dst);
    const int upper = (height / 4) * 2;
    if (upper <= 0 || upper >= height)
        return ConvertBgraToI420Serial(src, stride, width, height, dst);
    const int uvStride = (width + 1) / 2;
    int upperStatus = -1;
    std::thread worker;
    try {
        worker = std::thread([&]() {
            upperStatus = libyuv::ARGBToI420(src, stride,
                dst.y.data(), width,
                dst.u.data(), uvStride,
                dst.v.data(), uvStride, width, upper);
        });
    } catch (...) {
        // No thread available: return the exact serial algorithm.
        return ConvertBgraToI420Serial(src, stride, width, height, dst);
    }
    const int lowerStatus = libyuv::ARGBToI420(
        src + static_cast<size_t>(stride) * upper, stride,
        dst.y.data() + static_cast<size_t>(width) * upper, width,
        dst.u.data() + static_cast<size_t>(uvStride) * (upper / 2), uvStride,
        dst.v.data() + static_cast<size_t>(uvStride) * (upper / 2), uvStride,
        width, height - upper);
    worker.join();
    return (upperStatus == 0 && lowerStatus == 0) ? 0 : -1;
}

class WgcAdaptiveI420Converter {
public:
    // The single-threaded path is always available. Adaptive trials are
    // WGC-only; other capture providers retain the existing function.
    bool Convert(const uint8_t* src, int stride, int width, int height,
        I420Frame& dst, bool enableParallel = true) {
        dst.width = width;
        dst.height = height;
        const int uvWidth = (width + 1) / 2;
        const int uvHeight = (height + 1) / 2;
        dst.y.resize(static_cast<size_t>(width) * height);
        dst.u.resize(static_cast<size_t>(uvWidth) * uvHeight);
        dst.v.resize(static_cast<size_t>(uvWidth) * uvHeight);
        const bool qualified = enableParallel && width >= 1920 &&
            height >= 1080 && !(height & 1) &&
            std::thread::hardware_concurrency() >= 4;
        if (!qualified) {
            parallelLast_ = false;
            return ConvertBgraToI420Serial(src,stride,width,height,dst) == 0;
        }
        // Four serial and four parallel samples, then keep the faster path.
        // Sampling is bounded, with no extra frame copies or warm-up frames.
        const bool parallel = samples_ >= 4 && (samples_ < 8 || useParallel_);
        parallelLast_ = parallel;
        const auto start = std::chrono::steady_clock::now();
        const int result = parallel
            ? ConvertBgraToI420TwoStrips(src,stride,width,height,dst)
            : ConvertBgraToI420Serial(src,stride,width,height,dst);
        const double ms = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (samples_ < 8) {
            if (parallel) parallelTotalMs_ += ms;
            else serialTotalMs_ += ms;
            ++samples_;
            if (samples_ == 8) {
                // Demand at least 10% faster before adding a second thread.
                useParallel_ = parallelTotalMs_ < serialTotalMs_ * 0.90;
            }
        }
        return result == 0;
    }
    bool ParallelLast() const { return parallelLast_; }
    bool Qualified() const { return samples_ >= 8; }
    bool UsingParallel() const { return useParallel_; }
    double SerialTrialAvgMs() const { return serialTotalMs_ / 4.0; }
    double ParallelTrialAvgMs() const { return parallelTotalMs_ / 4.0; }
private:
    int samples_ = 0;
    bool useParallel_ = false;
    bool parallelLast_ = false;
    double serialTotalMs_ = 0;
    double parallelTotalMs_ = 0;
};
} // namespace hi5
