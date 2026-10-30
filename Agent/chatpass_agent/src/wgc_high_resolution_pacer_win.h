#pragma once
// Normal-desktop WGC-only frame pacing. Prefer a local high-resolution
// waitable timer; some Windows 11 builds reject that flag with ERROR 87.
// In that case request 1ms process timer granularity only for the lifetime
// of the explicit manual WGC session and always balance timeEndPeriod.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <thread>

#pragma comment(lib, "winmm.lib")
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace hi5 {
class WgcHighResolutionPacer final {
public:
    WgcHighResolutionPacer() = default;
    ~WgcHighResolutionPacer() { Release(); }
    WgcHighResolutionPacer(const WgcHighResolutionPacer&) = delete;
    WgcHighResolutionPacer& operator=(const WgcHighResolutionPacer&) = delete;

    // Release on WGC -> GDI/UAC or manual -> Automatic transitions,
    // instead of leaving the high-resolution request in place.
    void Release() {
        if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
        if (periodRaised_) { timeEndPeriod(1); periodRaised_ = false; }
        attempted_ = false;
    }
    // True if this wait used high-resolution timing. False means the safe
    // original coarse sleep was used. Never spin or send duplicate frames.
    bool WaitUntil(std::chrono::steady_clock::time_point deadline) {
        if (deadline <= std::chrono::steady_clock::now()) return false;
        if (!attempted_) {
            attempted_ = true;
            timer_ = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (!timer_) {
                createError_ = GetLastError();
                // Tested on Windows 11 build 26200: ERROR_INVALID_PARAMETER
                // for the high-res timer, but a scoped timeBeginPeriod(1)
                // reduced 16ms Sleep from ~25ms to ~16.5ms.
                periodError_ = timeBeginPeriod(1);
                periodRaised_ = (periodError_ == TIMERR_NOERROR);
            }
        }
        if (timer_) {
            const auto now = std::chrono::steady_clock::now();
            if (deadline <= now) return true;
            const auto remainingNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
            LARGE_INTEGER relative{};
            relative.QuadPart = -std::max<int64_t>(1, (remainingNs + 99) / 100);
            if (SetWaitableTimerEx(timer_, &relative, 0, nullptr, nullptr, nullptr, 0)) {
                // Caller caps each wait at 25ms for input/stop responsiveness.
                if (WaitForSingleObject(timer_, 30) == WAIT_OBJECT_0)
                    return true;
            }
        }
        std::this_thread::sleep_until(deadline);
        return periodRaised_;
    }
    bool Available() const { return timer_ || periodRaised_; }
    const char* Mode() const {
        if (timer_) return "high_res_waitable_timer";
        if (periodRaised_) return "scoped_1ms_process_timer";
        return "legacy_sleep";
    }
    DWORD CreateError() const { return createError_; }
    MMRESULT PeriodError() const { return periodError_; }
private:
    HANDLE timer_ = nullptr;
    bool periodRaised_ = false;
    bool attempted_ = false;
    DWORD createError_ = ERROR_SUCCESS;
    MMRESULT periodError_ = TIMERR_NOERROR;
};
} // namespace hi5
