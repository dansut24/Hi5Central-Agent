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

struct FrameCaptureResult {
    I420Frame frame;
    bool hasFrame = false;
    bool changed = false;
    bool cursorOnly = false;
    uint64_t frameId = 0;
};

struct SharedGpuFrame {
    int width = 0;
    int height = 0;
    uint32_t dxgiFormat = 0;
    uint32_t adapterLuidLow = 0;
    int32_t adapterLuidHigh = 0;
    uint64_t sharedHandle = 0;
    uint64_t syncKey = 0;
    uint64_t frameId = 0;
};

struct GpuFrameCaptureResult {
    SharedGpuFrame frame;
    bool supported = false;
    bool hasFrame = false;
    bool changed = false;
    bool cursorOnly = false;
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
    FrameCaptureResult nextFrameEx(bool includeUnchangedFrame = true);
    void nextFrameExInto(FrameCaptureResult& result, bool includeUnchangedFrame = true);
    GpuFrameCaptureResult nextSharedGpuFrameEx();
    void nextSharedGpuFrameExInto(GpuFrameCaptureResult& result);

    std::vector<DisplayInfo> listDisplays() const;
    DisplayInfo currentDisplayInfo() const;
    int currentDisplayIndex() const;
    bool setDisplayIndex(int index);

    std::vector<uint8_t> capturePreviewJpeg(int index, int maxWidth, int maxHeight, int quality = 70) const;

private:
    struct Impl;
    Impl* m_impl;
};