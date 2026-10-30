#pragma once
// Windows Graphics Capture alternative for normal-desktop DXGI access denials.
// Experimental, explicitly opted-in only. Never capture Winlogon/UAC desktops.
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Valid only when TryReadBgra returned a compositor frame successfully.
struct WgcReadbackTimings {
    double gpuCopyMapMs = 0.0; // GPU CopyResource and blocking CPU Map
    double cpuBufferCopyMs = 0.0; // mapped BGRA to reusable CPU buffer
};
class WgcMonitorCapture final {
public:
    WgcMonitorCapture();
    ~WgcMonitorCapture();
    WgcMonitorCapture(const WgcMonitorCapture&) = delete;
    WgcMonitorCapture& operator=(const WgcMonitorCapture&) = delete;

    bool Start(int monitorX, int monitorY, int monitorWidth, int monitorHeight,
               std::string& failure);
    // Returns false without failure when the compositor has not produced a
    // new frame; caller can reuse its last good frame or use GDI initially.
    bool TryReadBgra(std::vector<uint8_t>& bgra, int& width, int& height,
                     int& stride, std::string& failure,
                     WgcReadbackTimings* timings = nullptr);
    void Stop();
    bool IsRunning() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
