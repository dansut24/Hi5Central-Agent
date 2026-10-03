#include "frame_source.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint8_t clampByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

int maskShift(unsigned long mask) {
    if (!mask) return 0;
    int shift = 0;
    while ((mask & 1UL) == 0) { mask >>= 1; ++shift; }
    return shift;
}

unsigned long maskMax(unsigned long mask) {
    if (!mask) return 0;
    while ((mask & 1UL) == 0) mask >>= 1;
    return mask;
}

uint8_t component(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    const int shift = maskShift(mask);
    const unsigned long maximum = maskMax(mask);
    const unsigned long value = (pixel & mask) >> shift;
    return maximum ? static_cast<uint8_t>((value * 255UL + maximum / 2UL) / maximum) : 0;
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb rgbAt(const XImage* image, int x, int y) {
    unsigned long pixel = 0;
    if (image->bits_per_pixel == 32 && image->byte_order == LSBFirst) {
        std::uint32_t raw = 0;
        std::memcpy(
            &raw,
            image->data + static_cast<std::size_t>(y) * image->bytes_per_line + static_cast<std::size_t>(x) * 4,
            sizeof(raw));
        pixel = raw;
    } else {
        pixel = XGetPixel(const_cast<XImage*>(image), x, y);
    }
    return {
        component(pixel, image->red_mask),
        component(pixel, image->green_mask),
        component(pixel, image->blue_mask)
    };
}

I420Frame toI420(XImage* image) {
    I420Frame out;
    if (!image || image->width <= 0 || image->height <= 0) return out;
    const int width = image->width;
    const int height = image->height;
    const int uvWidth = (width + 1) / 2;
    const int uvHeight = (height + 1) / 2;

    out.width = width;
    out.height = height;
    out.y.resize(static_cast<std::size_t>(width) * height);
    out.u.resize(static_cast<std::size_t>(uvWidth) * uvHeight);
    out.v.resize(static_cast<std::size_t>(uvWidth) * uvHeight);

    for (int by = 0; by < height; by += 2) {
        for (int bx = 0; bx < width; bx += 2) {
            int sumR = 0, sumG = 0, sumB = 0, samples = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int y = by + dy;
                if (y >= height) continue;
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = bx + dx;
                    if (x >= width) continue;
                    const auto rgb = rgbAt(image, x, y);
                    const int yy = ((66 * rgb.r + 129 * rgb.g + 25 * rgb.b + 128) >> 8) + 16;
                    out.y[static_cast<std::size_t>(y) * width + x] = clampByte(yy);
                    sumR += rgb.r; sumG += rgb.g; sumB += rgb.b; ++samples;
                }
            }
            if (!samples) continue;
            const int r = sumR / samples;
            const int g = sumG / samples;
            const int b = sumB / samples;
            const int uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            const int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            const std::size_t uvIndex = static_cast<std::size_t>(by / 2) * uvWidth + (bx / 2);
            out.u[uvIndex] = clampByte(uu);
            out.v[uvIndex] = clampByte(vv);
        }
    }
    return out;
}

} // namespace

struct DesktopFrameSource::Impl {
    Display* display = nullptr;
    Window root = 0;
    std::vector<DisplayInfo> displays;
    int currentIndex = 0;
    std::uint64_t frameId = 0;

    Impl() {
        XInitThreads();
        display = XOpenDisplay(nullptr);
        if (!display) throw std::runtime_error("Could not open X11 display for screen capture.");
        root = DefaultRootWindow(display);
        refreshDisplays();
        if (displays.empty()) throw std::runtime_error("X11 reported no captureable displays.");
        currentIndex = displays.front().index;
    }

    ~Impl() {
        if (display) XCloseDisplay(display);
    }

    void refreshDisplays() {
        displays.clear();
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(display, root, &attrs)) {
            throw std::runtime_error("Could not query X11 root window geometry.");
        }

        int count = 0;
        XineramaScreenInfo* screens = nullptr;
        if (XineramaIsActive(display)) screens = XineramaQueryScreens(display, &count);

        if (screens && count > 1) {
            displays.push_back({-1, "All monitors", 0, 0, attrs.width, attrs.height, false});
            for (int i = 0; i < count; ++i) {
                displays.push_back({
                    i,
                    "Monitor " + std::to_string(i + 1),
                    screens[i].x_org,
                    screens[i].y_org,
                    screens[i].width,
                    screens[i].height,
                    i == 0
                });
            }
        } else if (screens && count == 1) {
            displays.push_back({
                0, "Monitor 1", screens[0].x_org, screens[0].y_org,
                screens[0].width, screens[0].height, true
            });
        } else {
            displays.push_back({0, "Display :0", 0, 0, attrs.width, attrs.height, true});
        }

        if (screens) XFree(screens);
    }

    DisplayInfo current() const {
        for (const auto& item : displays) if (item.index == currentIndex) return item;
        return displays.empty() ? DisplayInfo{} : displays.front();
    }
};

DesktopFrameSource::DesktopFrameSource() : m_impl(new Impl()) {}
DesktopFrameSource::~DesktopFrameSource() { delete m_impl; }

I420Frame DesktopFrameSource::nextFrame() {
    const auto d = m_impl->current();
    if (d.width <= 0 || d.height <= 0) throw std::runtime_error("Invalid X11 capture geometry.");
    XImage* image = XGetImage(
        m_impl->display,
        m_impl->root,
        d.x,
        d.y,
        static_cast<unsigned int>(d.width),
        static_cast<unsigned int>(d.height),
        AllPlanes,
        ZPixmap);
    if (!image) throw std::runtime_error("XGetImage failed for X11 desktop capture.");
    auto frame = toI420(image);
    XDestroyImage(image);
    if (frame.width <= 0 || frame.height <= 0) throw std::runtime_error("X11 capture produced an empty frame.");
    return frame;
}

FrameCaptureResult DesktopFrameSource::nextFrameEx() {
    FrameCaptureResult result;
    result.frame = nextFrame();
    result.hasFrame = true;
    result.changed = true;
    result.cursorOnly = false;
    result.frameId = ++m_impl->frameId;
    return result;
}

std::vector<DisplayInfo> DesktopFrameSource::listDisplays() const {
    return m_impl->displays;
}

DisplayInfo DesktopFrameSource::currentDisplayInfo() const {
    return m_impl->current();
}

int DesktopFrameSource::currentDisplayIndex() const {
    return m_impl->currentIndex;
}

bool DesktopFrameSource::setDisplayIndex(int index) {
    for (const auto& item : m_impl->displays) {
        if (item.index == index) {
            m_impl->currentIndex = index;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> DesktopFrameSource::capturePreviewJpeg(int, int, int, int) const {
    return {};
}
