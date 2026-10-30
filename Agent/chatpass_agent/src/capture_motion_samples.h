#pragma once
// Cheap low-frequency luminance change detection for GDI fallback.
// GDI reports every successful BitBlt as changed even for a static screen;
// do not let that hold the expensive native-resolution capture at motion FPS.
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hi5 {
class SampledDesktopMotion {
    static constexpr int kCols = 48;
    static constexpr int kRows = 27;
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
            height != lastHeight_ || differences >= 3;
        last_=current;
        lastWidth_=width;
        lastHeight_=height;
        valid_=true;
        return changed;
    }
};
} // namespace hi5
