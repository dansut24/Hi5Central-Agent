#pragma once
// Windows Graphics Capture alternative for normal-desktop DXGI access denials.
// Experimental, explicitly opted-in only. Never capture Winlogon/UAC desktops.
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

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
                     int& stride, std::string& failure);
    void Stop();
    bool IsRunning() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
