#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

inline std::mutex& Hi5LogMutex() {
    static std::mutex m;
    return m;
}

inline HANDLE Hi5NamedLogMutex() {
    static HANDLE handle = CreateMutexW(nullptr, FALSE, L"Global\\Hi5CentralAgentLogMutexV2");
    return handle;
}

class Hi5InterprocessLogLock {
public:
    Hi5InterprocessLogLock() : handle_(Hi5NamedLogMutex()) {
        if (handle_) {
            // Diagnostics must never stall capture/input. A helper can be killed
            // during logout/UAC while owning this mutex; WAIT_ABANDONED still
            // transfers ownership to us and therefore must be released. The old
            // code ignored WAIT_ABANDONED and could leave the mutex owned forever,
            // making every other process pause for five seconds per log line.
            const DWORD rc = WaitForSingleObject(handle_, 100);
            acquired_ = (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED);
        }
    }
    ~Hi5InterprocessLogLock() {
        if (handle_ && acquired_) ReleaseMutex(handle_);
    }
private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

inline std::string Hi5NowString(bool includeMilliseconds = true) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    std::tm tmLocal{};
    localtime_s(&tmLocal, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmLocal, "%Y-%m-%d %H:%M:%S");
    if (includeMilliseconds) {
        const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        oss << '.' << std::setw(3) << std::setfill('0') << ms.count();
    }
    return oss.str();
}

inline std::filesystem::path Hi5LogDirectory() {
    const std::filesystem::path dir = "C:\\ProgramData\\Hi5Central\\Agent\\Logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

inline std::filesystem::path Hi5SupportLogPath() {
    return Hi5LogDirectory() / "agent.log";
}

inline std::filesystem::path Hi5DiagnosticLogPath() {
    return Hi5LogDirectory() / "diagnostics.log";
}

inline void Hi5RotateLog(const std::filesystem::path& path, uintmax_t maxBytes, int backups) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size < maxBytes) return;

    // Old Agent builds could leave an arbitrarily large agent.log. Do not keep
    // a huge legacy file around as a rotated backup and defeat the new cap.
    if (size > maxBytes * 2) {
        std::filesystem::remove(path, ec);
        return;
    }

    for (int i = backups; i >= 1; --i) {
        const auto dst = std::filesystem::path(path.string() + "." + std::to_string(i));
        const auto src = (i == 1) ? path : std::filesystem::path(path.string() + "." + std::to_string(i - 1));
        std::filesystem::remove(dst, ec);
        ec.clear();
        if (std::filesystem::exists(src, ec)) {
            std::filesystem::rename(src, dst, ec);
        }
        ec.clear();
    }
}

inline void Hi5AppendLogLine(const std::filesystem::path& path,
    const std::string& line,
    uintmax_t maxBytes,
    int backups) {
    std::lock_guard<std::mutex> processLock(Hi5LogMutex());
    Hi5InterprocessLogLock interprocessLock;
    Hi5RotateLog(path, maxBytes, backups);

    HANDLE file = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    const std::string text = line + "\r\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    // Opening with FILE_APPEND_DATA and closing the handle after each line is enough
    // durability for support diagnostics. Avoid FlushFileBuffers on every debug line:
    // it turns verbose WebRTC/capture logging into synchronous disk I/O.
    CloseHandle(file);
}

inline void Hi5WriteLogLine(const char* level, const std::string& msg) {
    std::ostringstream line;
    line << Hi5NowString(true) << " [" << level << "] " << msg;
    const std::string s = line.str();

    if (std::string(level) == "ERROR") std::cerr << s << std::endl;
    else std::cout << s << std::endl;

    constexpr uintmax_t kDiagnosticMaxBytes = 5ull * 1024ull * 1024ull;
    constexpr int kDiagnosticBackups = 3;
    Hi5AppendLogLine(Hi5DiagnosticLogPath(), s, kDiagnosticMaxBytes, kDiagnosticBackups);
}

inline void LogSupportEvent(const std::string& msg) {
    std::string safe = msg;
    for (char& ch : safe) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (safe.size() > 2048) safe.resize(2048);

    const std::string s = Hi5NowString(false) + " - " + safe;
    constexpr uintmax_t kSupportMaxBytes = 2ull * 1024ull * 1024ull;
    constexpr int kSupportBackups = 4;
    Hi5AppendLogLine(Hi5SupportLogPath(), s, kSupportMaxBytes, kSupportBackups);
    // Keep the same event in diagnostics for correlation without making the
    // end-user log carry packet/frame/debug noise.
    Hi5AppendLogLine(Hi5DiagnosticLogPath(), Hi5NowString(true) + " [EVENT] " + safe,
        5ull * 1024ull * 1024ull, 3);
}

inline void LogInfo(const std::string& msg) { Hi5WriteLogLine("INFO", msg); }
inline void LogWarn(const std::string& msg) { Hi5WriteLogLine("WARN", msg); }
inline void LogError(const std::string& msg) { Hi5WriteLogLine("ERROR", msg); }
