#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace hi5 {

class NativeBanner {
public:
    NativeBanner();
    ~NativeBanner();

    void Start(const std::string& technicianName);
    void Stop();
    void SetTechnicianName(const std::string& technicianName);

private:
    void ThreadMain();

    std::atomic<bool> running_{ false };
    std::thread thread_;
    std::string technicianName_;
    unsigned long threadId_ = 0;
};

int RunNativeBannerMain(int argc, char** argv);

} // namespace hi5
