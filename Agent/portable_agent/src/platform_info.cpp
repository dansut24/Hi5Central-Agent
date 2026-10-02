#include "platform_info.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#endif

namespace hi5 {
namespace {

std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return trim(out.str());
}

std::string runCommand(const std::string& command) {
    std::array<char, 2048> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
        if (output.size() > 1024 * 1024) break;
    }
    pclose(pipe);
    return trim(output);
}

std::map<std::string, std::string> osRelease() {
    std::map<std::string, std::string> values;
#if defined(__linux__)
    std::ifstream in("/etc/os-release");
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        values[key] = value;
    }
#endif
    return values;
}

std::string unameField(bool machine) {
    struct utsname info {};
    if (uname(&info) != 0) return {};
    return machine ? std::string(info.machine) : std::string(info.release);
}

#if defined(__APPLE__)
template <typename T>
bool sysctlValue(const char* name, T& value) {
    size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0;
}

std::string sysctlString(const char* name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return trim(value);
}
#endif

struct LinuxCpuTicks {
    std::uint64_t idle = 0;
    std::uint64_t total = 0;
    bool valid = false;
};

LinuxCpuTicks readLinuxCpuTicks() {
    LinuxCpuTicks out;
#if defined(__linux__)
    std::ifstream in("/proc/stat");
    std::string label;
    std::uint64_t user=0,nice=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0;
    if (in >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal && label == "cpu") {
        out.idle = idle + iowait;
        out.total = user + nice + system + idle + iowait + irq + softirq + steal;
        out.valid = out.total > 0;
    }
#endif
    return out;
}

} // namespace

std::string platformId() {
#if defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string platformDisplayName() {
#if defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

std::string architecture() {
    const auto machine = unameField(true);
    if (machine == "x86_64" || machine == "amd64") return "x64";
    if (machine == "aarch64" || machine == "arm64") return "arm64";
    if (machine == "i386" || machine == "i686") return "x86";
    return machine.empty() ? "unknown" : machine;
}

std::string hostname() {
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0]) return buffer;
    return platformId() + "-host";
}

std::string machineId() {
#if defined(__APPLE__)
    auto value = runCommand("/usr/sbin/ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | /usr/bin/awk -F'\"' '/IOPlatformUUID/{print $(NF-1); exit}'");
    if (!value.empty()) return "macos-ioplatform:" + value;
#else
    auto value = readFile("/etc/machine-id");
    if (value.empty()) value = readFile("/var/lib/dbus/machine-id");
    if (!value.empty()) return "linux-machine-id:" + value;
#endif
    return platformId() + "-hostname:" + hostname();
}

std::string osVersion() {
#if defined(__APPLE__)
    auto version = runCommand("/usr/bin/sw_vers -productVersion 2>/dev/null");
    return version.empty() ? unameField(false) : version;
#else
    const auto values = osRelease();
    auto it = values.find("VERSION_ID");
    if (it != values.end() && !it->second.empty()) return it->second;
    return unameField(false);
#endif
}

std::string osBuild() {
#if defined(__APPLE__)
    auto build = runCommand("/usr/bin/sw_vers -buildVersion 2>/dev/null");
    return build.empty() ? unameField(false) : build;
#else
    return unameField(false);
#endif
}

std::string manufacturer() {
#if defined(__APPLE__)
    return "Apple Inc.";
#else
    auto value = readFile("/sys/class/dmi/id/sys_vendor");
    return value.empty() ? "Unknown" : value;
#endif
}

std::string model() {
#if defined(__APPLE__)
    auto value = sysctlString("hw.model");
    return value.empty() ? "Mac" : value;
#else
    auto value = readFile("/sys/class/dmi/id/product_name");
    return value.empty() ? "Linux computer" : value;
#endif
}

std::string serialNumber() {
#if defined(__APPLE__)
    return runCommand("/usr/sbin/ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | /usr/bin/awk -F'\"' '/IOPlatformSerialNumber/{print $(NF-1); exit}'");
#else
    return readFile("/sys/class/dmi/id/product_serial");
#endif
}

std::string cpuModel() {
#if defined(__APPLE__)
    auto value = sysctlString("machdep.cpu.brand_string");
    if (value.empty()) value = sysctlString("hw.model");
    return value;
#else
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        const auto key = trim(line.substr(0, pos));
        if (key == "model name" || key == "Hardware") return trim(line.substr(pos + 1));
    }
    return {};
#endif
}

std::string activeUser() {
#if defined(__APPLE__)
    auto value = runCommand("/usr/bin/stat -f%Su /dev/console 2>/dev/null");
    if (value == "root" || value == "loginwindow") return {};
    return value;
#else
    return runCommand("/usr/bin/who 2>/dev/null | /usr/bin/awk 'NF {print $1; exit}'");
#endif
}

std::uint64_t uptimeSeconds() {
#if defined(__APPLE__)
    struct timeval boot {};
    size_t size = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &size, nullptr, 0) == 0 && boot.tv_sec > 0) {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        return now > boot.tv_sec ? static_cast<std::uint64_t>(now - boot.tv_sec) : 0;
    }
    return 0;
#else
    std::ifstream in("/proc/uptime");
    double seconds = 0;
    if (in >> seconds && seconds > 0) return static_cast<std::uint64_t>(seconds);
    return 0;
#endif
}

double cpuPercent() {
#if defined(__APPLE__)
    static host_cpu_load_info_data_t previous {};
    static bool hasPrevious = false;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    host_cpu_load_info_data_t current {};
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
        reinterpret_cast<host_info_t>(&current), &count) != KERN_SUCCESS) return 0.0;

    if (!hasPrevious) {
        previous = current;
        hasPrevious = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return cpuPercent();
    }

    std::uint64_t previousTotal = 0;
    std::uint64_t currentTotal = 0;
    for (int i = 0; i < CPU_STATE_MAX; ++i) {
        previousTotal += previous.cpu_ticks[i];
        currentTotal += current.cpu_ticks[i];
    }
    const auto previousIdle = previous.cpu_ticks[CPU_STATE_IDLE];
    previous = current;
    const auto totalDelta = currentTotal >= previousTotal ? currentTotal - previousTotal : 0;
    const auto idleDelta = current.cpu_ticks[CPU_STATE_IDLE] >= previousIdle
        ? current.cpu_ticks[CPU_STATE_IDLE] - previousIdle
        : 0;
    if (!totalDelta) return 0.0;
    return std::clamp(
        (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)) * 100.0,
        0.0,
        100.0);
#else
    static LinuxCpuTicks previous = readLinuxCpuTicks();
    auto current = readLinuxCpuTicks();
    if (!previous.valid || !current.valid || current.total <= previous.total) {
        previous = current;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        current = readLinuxCpuTicks();
    }
    const auto totalDelta = current.total >= previous.total ? current.total - previous.total : 0;
    const auto idleDelta = current.idle >= previous.idle ? current.idle - previous.idle : 0;
    previous = current;
    if (!totalDelta) return 0.0;
    return std::clamp(
        (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)) * 100.0,
        0.0,
        100.0);
#endif
}

MemoryStats memoryStats() {
    MemoryStats out;
#if defined(__APPLE__)
    std::uint64_t total = 0;
    sysctlValue("hw.memsize", total);

    vm_statistics64_data_t vm {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    host_page_size(mach_host_self(), &pageSize);
    if (host_statistics64(
        mach_host_self(),
        HOST_VM_INFO64,
        reinterpret_cast<host_info64_t>(&vm),
        &count) == KERN_SUCCESS) {
        const std::uint64_t freePages =
            static_cast<std::uint64_t>(vm.free_count) +
            static_cast<std::uint64_t>(vm.inactive_count) +
            static_cast<std::uint64_t>(vm.speculative_count);
        out.totalBytes = total;
        out.availableBytes = freePages * static_cast<std::uint64_t>(pageSize);
    } else {
        out.totalBytes = total;
    }
#else
    std::ifstream in("/proc/meminfo");
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    std::uint64_t totalKb = 0;
    std::uint64_t availableKb = 0;
    while (in >> key >> value >> unit) {
        if (key == "MemTotal:") totalKb = value;
        else if (key == "MemAvailable:") availableKb = value;
    }
    out.totalBytes = totalKb * 1024ULL;
    out.availableBytes = availableKb * 1024ULL;
#endif

    out.usedBytes =
        out.totalBytes > out.availableBytes ? out.totalBytes - out.availableBytes : 0;
    if (out.totalBytes) {
        out.usedPercent =
            static_cast<double>(out.usedBytes) * 100.0 /
            static_cast<double>(out.totalBytes);
    }
    return out;
}

DiskStats rootDiskStats() {
    DiskStats out;
    struct statvfs vfs {};
    if (statvfs("/", &vfs) != 0) return out;

    out.totalBytes = static_cast<std::uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    out.freeBytes = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    out.usedBytes = out.totalBytes > out.freeBytes ? out.totalBytes - out.freeBytes : 0;
    if (out.totalBytes) {
        out.usedPercent =
            static_cast<double>(out.usedBytes) * 100.0 /
            static_cast<double>(out.totalBytes);
    }
    return out;
}

nlohmann::json networkInfo() {
    using json = nlohmann::json;
    json adapters = json::array();
    std::map<std::string, json> byName;

    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || !interfaces) {
        return {{"adapters", adapters}};
    }

    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_name || !item->ifa_addr) continue;

        const int family = item->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;

        char address[NI_MAXHOST] = {};
        const socklen_t length =
            family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (getnameinfo(
            item->ifa_addr,
            length,
            address,
            sizeof(address),
            nullptr,
            0,
            NI_NUMERICHOST) != 0) {
            continue;
        }

        auto& entry = byName[item->ifa_name];
        if (entry.is_null()) {
            entry = {
                {"name", item->ifa_name},
                {"adapter", item->ifa_name},
                {"status", (item->ifa_flags & IFF_UP) ? "Up" : "Down"},
                {"ipv4", json::array()},
                {"ipv6", json::array()}
            };
        }

        if (family == AF_INET) entry["ipv4"].push_back(address);
        else entry["ipv6"].push_back(address);
    }
    freeifaddrs(interfaces);

    for (auto& [name, entry] : byName) adapters.push_back(entry);

    json primary = json::object();
    for (const auto& entry : adapters) {
        if (entry.value("status", "") != "Up") continue;
        if (!entry.contains("ipv4") || !entry["ipv4"].is_array() || entry["ipv4"].empty()) continue;
        const auto ip = entry["ipv4"][0].get<std::string>();
        if (ip.rfind("127.", 0) != 0 && ip.rfind("169.254.", 0) != 0) {
            primary = entry;
            break;
        }
    }

    return {
        {"adapters", adapters},
        {"primary", primary},
        {"primary_ipv4",
            primary.contains("ipv4") && primary["ipv4"].is_array() && !primary["ipv4"].empty()
                ? primary["ipv4"][0]
                : json(nullptr)}
    };
}

std::string nowIsoUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
    gmtime_r(&time, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::filesystem::path defaultStateDirectory() {
    if (geteuid() == 0) {
#if defined(__APPLE__)
        return "/Library/Application Support/Hi5Central/Agent";
#else
        return "/var/lib/hi5central/agent";
#endif
    }

    const char* home = std::getenv("HOME");
    if (!home || !*home) return std::filesystem::current_path();

#if defined(__APPLE__)
    return std::filesystem::path(home) /
        "Library" / "Application Support" / "Hi5Central" / "Agent";
#else
    return std::filesystem::path(home) /
        ".local" / "state" / "hi5central" / "agent";
#endif
}

} // namespace hi5
