#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

inline constexpr std::uintmax_t kHi5MaxLogBytes = 5u * 1024u * 1024u;
inline constexpr int kHi5LogBackups = 3;

inline std::mutex& Hi5LogMutex() {
    static std::mutex m;
    return m;
}

inline std::string Hi5NowString() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);

    std::tm tmLocal{};
    localtime_s(&tmLocal, &t);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(&tmLocal, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

inline std::filesystem::path Hi5GetLogPath() {
    const std::filesystem::path dir = L"C:\\ProgramData\\Hi5Central\\Agent\\Logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"agent.log";
}

inline std::filesystem::path Hi5BackupLogPath(
    const std::filesystem::path& active, int index) {
    std::filesystem::path result = active;
    result += L"." + std::to_wstring(index);
    return result;
}

inline void Hi5TrimLegacyBackupIfNeeded(const std::filesystem::path& backup) {
    // On the first upgraded run, the previous unbounded log may be much
    // larger than 5 MiB. Preserve only its most recent complete lines.
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(backup, ec);
    if (ec || bytes <= kHi5MaxLogBytes) return;

    auto temp = backup;
    temp += L".tmp";
    std::ifstream in(backup, std::ios::binary);
    if (!in) return;
    const auto skip = bytes - kHi5MaxLogBytes + 4096u;
    in.seekg(static_cast<std::streamoff>(skip), std::ios::beg);
    std::string partialLine;
    std::getline(in, partialLine);

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << in.rdbuf();
        out.flush();
        if (!out) return;
    }
    std::filesystem::remove(backup, ec);
    if (ec) return;
    std::filesystem::rename(temp, backup, ec);
    // On an I/O failure keep console output and retry rotation next time.
}

inline void Hi5RotateIfNeeded(const std::filesystem::path& active,
                              std::size_t incomingBytes) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(active, ec);
    if (ec || bytes + incomingBytes <= kHi5MaxLogBytes) return;

    // Every Agent process (service and desktop helpers) takes the same named
    // interprocess mutex before rotating or opening the append handle.
    for (int n = kHi5LogBackups; n >= 1; --n) {
        const auto from = n == 1 ? active : Hi5BackupLogPath(active, n - 1);
        const auto to = Hi5BackupLogPath(active, n);
        if (!std::filesystem::exists(from, ec)) {
            ec.clear();
            continue;
        }
        std::filesystem::remove(to, ec);
        ec.clear();
        std::filesystem::rename(from, to, ec);
        if (ec) {
            // Keep logging even if antivirus or an old-version helper holds a
            // file open; subsequent writes will retry once it is released.
            return;
        }
    }
    Hi5TrimLegacyBackupIfNeeded(Hi5BackupLogPath(active, 1));
}

inline HANDLE Hi5AcquireCrossProcessLogMutex(bool& acquired) {
    acquired = false;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const BOOL haveDescriptor = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)",
        SDDL_REVISION_1, &descriptor, nullptr);
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = haveDescriptor ? descriptor : nullptr;
    HANDLE mutex = CreateMutexW(haveDescriptor ? &attributes : nullptr, FALSE,
                                L"Global\\Hi5CentralAgentLogRotation");
    if (descriptor) LocalFree(descriptor);
    if (!mutex) return nullptr;
    const DWORD wait = WaitForSingleObject(mutex, 3000);
    acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (!acquired) {
        CloseHandle(mutex);
        return nullptr;
    }
    return mutex;
}

inline void Hi5WriteLogLine(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(Hi5LogMutex());
    const std::string s = Hi5NowString() + " [" + level + "] " + msg;

    if (std::string(level) == "ERROR") std::cerr << s << std::endl;
    else std::cout << s << std::endl;

    bool acquired = false;
    HANDLE guard = Hi5AcquireCrossProcessLogMutex(acquired);
    if (!acquired) return; // Console output retained; never rotate unsafely.

    const auto active = Hi5GetLogPath();
    Hi5RotateIfNeeded(active, s.size() + 2u);
    // Open and close on each message: a long-lived ofstream would continue
    // writing to a renamed backup after another helper rotates the file.
    {
        std::ofstream out(active, std::ios::binary | std::ios::app);
        if (out) out << s << "\r\n";
    }
    ReleaseMutex(guard);
    CloseHandle(guard);
}

inline void LogInfo(const std::string& msg) { Hi5WriteLogLine("INFO", msg); }
inline void LogWarn(const std::string& msg) { Hi5WriteLogLine("WARN", msg); }
inline void LogError(const std::string& msg) { Hi5WriteLogLine("ERROR", msg); }
