#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

inline std::mutex& Hi5LogMutex() {
    static std::mutex m;
    return m;
}

inline std::ofstream& Hi5LogFile() {
    static std::ofstream f;
    return f;
}

inline bool& Hi5LogInitialized() {
    static bool inited = false;
    return inited;
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
        << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

inline std::string Hi5GetLogPath() {
    const std::filesystem::path dir = "C:\\ProgramData\\Hi5Central\\Agent\\Logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / "agent.log").string();
}

inline void Hi5InitLoggingIfNeeded() {
    if (Hi5LogInitialized()) {
        return;
    }

    const std::string path = Hi5GetLogPath();
    Hi5LogFile().open(path, std::ios::out | std::ios::app);
    Hi5LogInitialized() = true;
}

inline void Hi5WriteLogLine(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(Hi5LogMutex());
    Hi5InitLoggingIfNeeded();

    std::ostringstream line;
    line << Hi5NowString()
        << " [" << level << "] "
        << msg;

    const std::string s = line.str();

    if (std::string(level) == "ERROR") {
        std::cerr << s << std::endl;
    }
    else {
        std::cout << s << std::endl;
    }

    if (Hi5LogFile().is_open()) {
        Hi5LogFile() << s << std::endl;
        Hi5LogFile().flush();
    }
}

inline void LogInfo(const std::string& msg) {
    Hi5WriteLogLine("INFO", msg);
}

inline void LogWarn(const std::string& msg) {
    Hi5WriteLogLine("WARN", msg);
}

inline void LogError(const std::string& msg) {
    Hi5WriteLogLine("ERROR", msg);
}