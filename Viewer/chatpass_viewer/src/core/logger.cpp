#include "core/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace hi5 {
namespace {
std::mutex g_logMutex;

std::string TimestampNow() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

void LogLine(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << TimestampNow() << " [" << level << "] " << message << std::endl;
}
}

void LogInfo(const std::string& message) { LogLine("INFO", message); }
void LogWarn(const std::string& message) { LogLine("WARN", message); }
void LogError(const std::string& message) { LogLine("ERROR", message); }

} // namespace hi5