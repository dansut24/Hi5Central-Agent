#pragma once

#include <atomic>
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
        bool notifyOnStart);
    void Stop();
    void SetTechnicianName(const std::string& technicianName);

private:
    void ThreadMain();

    std::atomic<bool> running_{ false };
    std::thread thread_;
    std::string technicianName_;
    std::string sessionId_;
    std::string chatEventName_;
    std::string endEventName_;
    bool notifyOnStart_ = true;
    unsigned long threadId_ = 0;
};

int RunNativeBannerMain(int argc, char** argv);

} // namespace hi5
