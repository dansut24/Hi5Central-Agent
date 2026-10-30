#pragma once
// Scoped high-resolution waits for the explicitly opted-in normal-desktop WGC
// streamer only. Do not request global Windows timer-period changes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace hi5 {
class WgcHighResolutionPacer final {
public:
    WgcHighResolutionPacer() = default;
    ~WgcHighResolutionPacer() { if (timer_) CloseHandle(timer_); }
    WgcHighResolutionPacer(const WgcHighResolutionPacer&) = delete;
    WgcHighResolutionPacer& operator=(const WgcHighResolutionPacer&) = delete;
    // Returns true only when a high-resolution Windows wait completed.
    // A missing/unsupported high-resolution timer safely falls back to the
    // existing chrono wait, without busy-spinning or raising system-wide CPU.
    bool WaitUntil(std::chrono::steady_clock::time_point deadline) {
        if (deadline <= std::chrono::steady_clock::now()) return false;
        if (!attempted_) {
            attempted_ = true;
            timer_ = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (!timer_) createError_ = GetLastError();
        }
        if (!timer_) {
            std::this_thread::sleep_until(deadline);
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now) return true;
        const auto remainingNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        LARGE_INTEGER relative{};
        // Negative times are relative. 100 ns units, rounding up rather than
        // waking earlier than the selected capture deadline.
        relative.QuadPart = -std::max<int64_t>(1, (remainingNs + 99) / 100);
        if (!SetWaitableTimerEx(timer_, &relative, 0, nullptr, nullptr, nullptr, 0)) {
            std::this_thread::sleep_until(deadline);
            return false;
        }
        // The caller limits each wait to 25ms so stop/input/desktop-state
        // polling stays responsive even if a timer stops being signalled.
        const DWORD result = WaitForSingleObject(timer_, 30);
        if (result != WAIT_OBJECT_0) {
            if (deadline > std::chrono::steady_clock::now())
                std::this_thread::sleep_until(deadline);
            return false;
        }
        return true;
    }
    bool Available() const { return timer_ != nullptr; }
    DWORD CreateError() const { return createError_; }
private:
    HANDLE timer_ = nullptr;
    DWORD createError_ = ERROR_SUCCESS;
    bool attempted_ = false;
};
} // namespace hi5
