#pragma once
// Sparse luminance change detection for GDI fallback. The old 48x27 grid
// missed narrow text/scroll movement and forwarded just 1-2 FPS despite
// 5-9 successful captures. A 96x54 grid is still only 5,184 luma samples
// per native-resolution capture, but is sensitive to normal UI motion.
// Suppress single-sample cursor flickers and unchanged desktops.
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hi5 {
class SampledDesktopMotion {
    static constexpr int kCols = 96;
    static constexpr int kRows = 54;
    std::array<uint8_t, kCols*kRows> last_{};
    int lastWidth_ = 0;
    int lastHeight_ = 0;
    bool valid_ = false;
public:
    bool observe(const std::vector<uint8_t>& luma, int width, int height) {
        if (width < 1 || height < 1 ||
            luma.size() < static_cast<size_t>(width) * static_cast<size_t>(height))
            return false;
        std::array<uint8_t, kCols*kRows> current{};
        int differences = 0;
        for (int row=0;row<kRows;++row) {
            const int yy=std::min(height-1,(2*row+1)*height/(2*kRows));
            for (int col=0;col<kCols;++col) {
                const int xx=std::min(width-1,(2*col+1)*width/(2*kCols));
                const auto sample=luma[static_cast<size_t>(yy)*width+xx];
                const size_t index=static_cast<size_t>(row)*kCols+col;
                current[index]=sample;
                if (valid_ && (static_cast<int>(sample)-static_cast<int>(last_[index])>12 ||
                               static_cast<int>(last_[index])-static_cast<int>(sample)>12))
                    ++differences;
            }
        }
        const bool changed = !valid_ || width != lastWidth_ ||
            height != lastHeight_ || differences >= 2;
        last_=current;
        lastWidth_=width;
        lastHeight_=height;
        valid_=true;
        return changed;
    }
};
} // namespace hi5
