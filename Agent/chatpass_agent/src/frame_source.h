#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct I420Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
};

// Actual provider of this particular poll; not the requested display index.
// DXGI access-denied can leave displayIndex=-1 while WGC streams normally.
enum class DesktopCaptureBackend : uint8_t {
    Unknown = 0, Gdi = 1, Dxgi = 2, Wgc = 3
};
inline const char* DesktopCaptureBackendName(DesktopCaptureBackend backend) {
    switch (backend) {
        case DesktopCaptureBackend::Gdi: return "gdi";
        case DesktopCaptureBackend::Dxgi: return "dxgi";
        case DesktopCaptureBackend::Wgc: return "wgc";
        default: return "unknown";
    }
}
struct FrameCaptureResult {
    DesktopCaptureBackend backend = DesktopCaptureBackend::Unknown;
    // Per-new-WGC-frame timings; zero for empty polls and other backends.
    double wgcReadbackMs = 0.0;
    double wgcConversionMs = 0.0;
    I420Frame frame;
    bool hasFrame = false;
    bool changed = false;
    // True only when the capture provider itself reported a new desktop
    // frame (DXGI dirty frame or WGC compositor event). GDI BitBlt has no
    // change signal, so its successful copies need sparse pixel comparison.
    bool changedIsNative = false;
    bool cursorOnly = false;
    uint64_t frameId = 0;
};

struct DisplayInfo {
    int index = 0;              // -1 = All monitors
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool primary = false;
};

class DesktopFrameSource {
public:
    DesktopFrameSource();
    ~DesktopFrameSource();
    DesktopFrameSource(const DesktopFrameSource&) = delete;
    DesktopFrameSource& operator=(const DesktopFrameSource&) = delete;
    DesktopFrameSource(DesktopFrameSource&& other) noexcept;
    DesktopFrameSource& operator=(DesktopFrameSource&& other) noexcept;

    I420Frame nextFrame();
    FrameCaptureResult nextFrameEx();

    std::vector<DisplayInfo> listDisplays() const;
    DisplayInfo currentDisplayInfo() const;
    int currentDisplayIndex() const;
    bool setDisplayIndex(int index);
    // Only normal desktop helpers are allowed to retry DXGI after fallback.
    // Secure desktop stays on its proven GDI path.
    void setDxgiRecoveryEnabled(bool enabled);

    std::vector<uint8_t> capturePreviewJpeg(int index, int maxWidth, int maxHeight, int quality = 70) const;

private:
    struct Impl;
    Impl* m_impl;
};