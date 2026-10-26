#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace hi5 {

class NativeBanner {
public:
    NativeBanner();
    ~NativeBanner();

    void Start(const std::string& technicianName,
        const std::string& sessionId,
        const std::string& chatEventName,
        const std::string& endEventName,
        bool notifyOnStart,
        const std::string& statusText = "Connected",
        bool remoteControlActive = true);
    void Stop();
    void SetTechnicianName(const std::string& technicianName);
    void SetConnectionState(const std::string& statusText, bool remoteControlActive);

private:
    void ThreadMain();

    std::atomic<bool> running_{ false };
    std::thread thread_;
    std::mutex stateMu_;
    std::string technicianName_;
    std::string sessionId_;
    std::string chatEventName_;
    std::string endEventName_;
    bool notifyOnStart_ = true;
    std::string statusText_ = "Connected";
    bool remoteControlActive_ = true;
    unsigned long threadId_ = 0;
    std::atomic<HWND> hwnd_{ nullptr };
};

int RunNativeBannerMain(int argc, char** argv);

} // namespace hi5
