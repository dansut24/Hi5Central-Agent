#pragma once
// Windows Graphics Capture alternative for normal-desktop DXGI access denials.
// Experimental, explicitly opted-in only. Never capture Winlogon/UAC desktops.
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Valid only when TryReadBgra returned a compositor frame successfully.
// Monotonic session counters from the WinRT FrameArrived event. Callback
// only touches atomics; no frame copies, WinRT calls, or queueing in the event.
struct WgcFrameArrivalDiagnostics {
    uint64_t eventsTotal = 0;
    uint64_t arrivalGapCount = 0;
    uint64_t arrivalGapTotalNs = 0;
    uint64_t arrivalGapMaxNs = 0;
    uint64_t framesConsumedTotal = 0;
    uint64_t eventsSincePreviousFrame = 0;
    // Windows compositor's SystemRelativeTime is 100ns units.
    int64_t sourceTimestamp100ns = 0;
    int64_t sourceTimestampGap100ns = 0;
};
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
                     WgcReadbackTimings* timings = nullptr,
                     WgcFrameArrivalDiagnostics* frameDiagnostics = nullptr);
    void Stop();
    bool IsRunning() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
