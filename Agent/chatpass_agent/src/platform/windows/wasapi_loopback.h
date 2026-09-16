#pragma once
#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

namespace hi5 {
class WasapiLoopbackCapture {
public:
    using PcmCallback = std::function<void(const int16_t* samples, size_t frames)>;
    WasapiLoopbackCapture() = default;
    ~WasapiLoopbackCapture();
    bool Start(PcmCallback callback);
    void Stop();
private:
    void Run();
    PcmCallback callback_;
    std::thread thread_;
    std::atomic<bool> running_{ false };
    HANDLE stopEvent_{ nullptr };
};
}
#endif
