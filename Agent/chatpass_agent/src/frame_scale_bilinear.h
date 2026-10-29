#pragma once
// Edge-preserving bilinear scaler for experimental remote H.264 sharp-text mode.
// Existing VP8, VP9 and non-experimental H.264 paths keep their proven scaler.
// Samples pixel centres; unlike nearest neighbour, fractional downscaling
// does not simply discard most source pixel values (jagged small text).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hi5 {
namespace quality {

inline void ScalePlaneBilinear(
    const std::vector<std::uint8_t>& src, int srcW, int srcH,
    std::vector<std::uint8_t>& dst, int dstW, int dstH) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 ||
        static_cast<std::size_t>(srcW) > src.size() / static_cast<std::size_t>(srcH) ||
        static_cast<std::size_t>(dstW) > static_cast<std::size_t>(32000000) /
            static_cast<std::size_t>(dstH)) {
        dst.clear();
        return;
    }
    dst.resize(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH));
    if (srcW == dstW && srcH == dstH) {
        std::copy_n(src.begin(), dst.size(), dst.begin());
        return;
    }
    struct XSample { int x0; int x1; int fraction; };
    std::vector<XSample> xmap(static_cast<std::size_t>(dstW));
    for (int x = 0; x < dstW; ++x) {
        const double sx = (static_cast<double>(x) + 0.5) * srcW / dstW - 0.5;
        const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, srcW - 1);
        const int x1 = std::min(srcW - 1, x0 + 1);
        const int weight = sx <= 0.0 || sx >= srcW - 1
            ? 0 : std::clamp(static_cast<int>((sx - x0) * 256.0 + 0.5), 0, 256);
        xmap[static_cast<std::size_t>(x)] = { x0, x1, weight };
    }
    for (int y = 0; y < dstH; ++y) {
        const double sy = (static_cast<double>(y) + 0.5) * srcH / dstH - 0.5;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, srcH - 1);
        const int y1 = std::min(srcH - 1, y0 + 1);
        const int wy = sy <= 0.0 || sy >= srcH - 1
            ? 0 : std::clamp(static_cast<int>((sy - y0) * 256.0 + 0.5), 0, 256);
        const std::size_t row0 = static_cast<std::size_t>(y0) * srcW;
        const std::size_t row1 = static_cast<std::size_t>(y1) * srcW;
        const std::size_t out = static_cast<std::size_t>(y) * dstW;
        for (int x = 0; x < dstW; ++x) {
            const XSample& p = xmap[static_cast<std::size_t>(x)];
            const int wx = p.fraction;
            const int a = (256 - wx) * src[row0 + p.x0] + wx * src[row0 + p.x1];
            const int b = (256 - wx) * src[row1 + p.x0] + wx * src[row1 + p.x1];
            dst[out + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(
                ((256 - wy) * a + wy * b + 32768) >> 16);
        }
    }
}

} // namespace quality
} // namespace hi5
