#include "inventory/inventory_snapshot.h"
#include "agent_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <lmcons.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "psapi.lib")

namespace hi5 {
namespace {

using json = nlohmann::json;

std::string RegString(HKEY root, const wchar_t* subkey, const wchar_t* name);
DWORD RegDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD fallback);

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len);
    return out;
}

std::string WideZToUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    return WideToUtf8(value);
}

std::string FileTimeToIsoUtc(const FILETIME& ft) {
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) return {};
    std::ostringstream os;
    os << std::setfill('0')
       << std::setw(4) << st.wYear << "-"
       << std::setw(2) << st.wMonth << "-"
       << std::setw(2) << st.wDay << "T"
       << std::setw(2) << st.wHour << ":"
       << std::setw(2) << st.wMinute << ":"
       << std::setw(2) << st.wSecond << "Z";
    return os.str();
}

std::string UnixSecondsToIsoUtc(std::uint64_t seconds) {
    if (seconds == 0) return {};
    std::time_t tt = static_cast<std::time_t>(seconds);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::string NowIsoUtc() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::string BootTimeIsoUtc() {
    FILETIME ftNow{};
    GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER uli{};
    uli.LowPart = ftNow.dwLowDateTime;
    uli.HighPart = ftNow.dwHighDateTime;
    const std::uint64_t uptime100ns = GetTickCount64() * 10000ULL;
    if (uli.QuadPart > uptime100ns) uli.QuadPart -= uptime100ns;
    FILETIME ftBoot{};
    ftBoot.dwLowDateTime = uli.LowPart;
    ftBoot.dwHighDateTime = uli.HighPart;
    return FileTimeToIsoUtc(ftBoot);
}

std::string DurationText(std::uint64_t seconds) {
    const auto days = seconds / 86400ULL;
    seconds %= 86400ULL;
    const auto hours = seconds / 3600ULL;
    seconds %= 3600ULL;
    const auto minutes = seconds / 60ULL;
    std::ostringstream os;
    if (days) os << days << "d ";
    if (hours || days) os << hours << "h ";
    os << minutes << "m";
    return os.str();
}

std::string ComputerName() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buffer, &len) && len > 0) {
        return WideToUtf8(std::wstring(buffer, len));
    }
    len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &len) && len > 0) {
        return WideToUtf8(std::wstring(buffer, len));
    }
    return {};
}

std::string DomainOrWorkgroup() {
    wchar_t buffer[256]{};
    DWORD len = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    if (GetComputerNameExW(ComputerNameDnsDomain, buffer, &len) && len > 0) {
        return WideToUtf8(std::wstring(buffer, len));
    }
    return "WORKGROUP";
}

std::string LoggedInUser() {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) return {};

    LPSTR user = nullptr;
    DWORD userLen = 0;
    LPSTR domain = nullptr;
    DWORD domainLen = 0;

    std::string out;
    if (WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &user, &userLen) && user && *user) {
        if (WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSDomainName, &domain, &domainLen) && domain && *domain) {
            out = std::string(domain) + "\\" + std::string(user);
        } else {
            out = std::string(user);
        }
    }
    if (user) WTSFreeMemory(user);
    if (domain) WTSFreeMemory(domain);
    return out;
}

std::string LastLoggedOnUser() {
    return RegString(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\LogonUI)", L"LastLoggedOnUser");
}

std::string RegString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return {};

    DWORD type = 0;
    DWORD bytes = 0;
    LONG rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || bytes == 0 || (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)) {
        RegCloseKey(key);
        return {};
    }

    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return {};

    while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    for (auto& ch : buffer) {
        if (ch == L'\0') ch = L' ';
    }
    return WideToUtf8(buffer);
}

DWORD RegDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD fallback = 0) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return fallback;
    DWORD type = 0;
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    const LONG rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return fallback;
    return value;
}

std::string NormalizeWindowsProductName(std::string product, DWORD buildNumber) {
    if (product.empty()) product = "Windows";
    if (buildNumber >= 22000) {
        const std::string needle = "Windows 10";
        const auto pos = product.find(needle);
        if (pos != std::string::npos) {
            product.replace(pos, needle.size(), "Windows 11");
        } else if (product.find("Windows 11") == std::string::npos) {
            product = "Windows 11";
        }
    }
    return product;
}

std::string GetModulePath() {
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0) return {};
    return WideToUtf8(std::wstring(path, len));
}

std::string EscapeForSingleQuotedPowerShell(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

json RunPowerShellJson(const std::string& script, const json& fallback = json::object()) {
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) return fallback;

    wchar_t tempFile[MAX_PATH]{};
    if (!GetTempFileNameW(tempPath, L"h5i", 0, tempFile)) return fallback;

    std::wstring scriptPath = tempFile;
    scriptPath += L".ps1";
    MoveFileW(tempFile, scriptPath.c_str());

    {
        std::ofstream f(WideToUtf8(scriptPath), std::ios::binary | std::ios::trunc);
        if (!f) return fallback;
        f << "$ProgressPreference = 'SilentlyContinue'\n";
        f << "$ErrorActionPreference = 'SilentlyContinue'\n";
        f << script << "\n";
    }

    std::string cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + WideToUtf8(scriptPath) + "\"";
    std::string output;
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe) {
        std::array<char, 4096> buffer{};
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
            output += buffer.data();
            if (output.size() > 4 * 1024 * 1024) break;
        }
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }
    DeleteFileW(scriptPath.c_str());

    const auto first = output.find_first_of("[{\"");
    if (first != std::string::npos) output = output.substr(first);
    const auto lastObj = output.find_last_of("]}");
    if (lastObj != std::string::npos) output = output.substr(0, lastObj + 1);
    if (output.empty()) return fallback;

    try {
        return json::parse(output);
    } catch (...) {
        return fallback;
    }
}

json OsInfo() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);

    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) fn(reinterpret_cast<PRTL_OSVERSIONINFOW>(&vi));
    }

    std::string product = RegString(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", L"ProductName");
    const std::string display = RegString(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", L"DisplayVersion");
    const std::string build = RegString(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", L"CurrentBuildNumber");
    const std::string ubr = std::to_string(RegDword(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", L"UBR", 0));
    const DWORD installDate = RegDword(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", L"InstallDate", 0);

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    std::string arch = "unknown";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) arch = "x64";
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) arch = "arm64";
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) arch = "x86";

    const DWORD effectiveBuild = vi.dwBuildNumber ? vi.dwBuildNumber : static_cast<DWORD>(std::strtoul(build.c_str(), nullptr, 10));
    product = NormalizeWindowsProductName(product, effectiveBuild);

    const auto uptimeSeconds = static_cast<std::uint64_t>(GetTickCount64() / 1000ULL);
    json out = {
        {"name", product},
        {"caption", product},
        {"display_version", display},
        {"build", build.empty() ? std::to_string(effectiveBuild) : build},
        {"os_build", build.empty() ? std::to_string(effectiveBuild) : build},
        {"ubr", ubr == "0" ? "" : ubr},
        {"architecture", arch},
        {"major", vi.dwMajorVersion},
        {"minor", vi.dwMinorVersion},
        {"build_number", effectiveBuild},
        {"install_date", UnixSecondsToIsoUtc(installDate)},
        {"install_date_unix", installDate},
        {"last_boot", BootTimeIsoUtc()},
        {"last_boot_utc", BootTimeIsoUtc()},
        {"uptime_seconds", uptimeSeconds},
        {"uptime", DurationText(uptimeSeconds)}
    };

    std::ostringstream version;
    version << vi.dwMajorVersion << "." << vi.dwMinorVersion << "." << effectiveBuild;
    if (ubr != "0") version << "." << ubr;
    out["version"] = version.str();
    return out;
}

json HardwareInfo() {
    json hw = {
        {"manufacturer", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\BIOS)", L"SystemManufacturer")},
        {"model", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\BIOS)", L"SystemProductName")},
        {"serial_number", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\BIOS)", L"SystemSerialNumber")},
        {"bios_vendor", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\BIOS)", L"BIOSVendor")},
        {"bios_version", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\BIOS)", L"BIOSVersion")},
        {"bios_date", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\BIOS)", L"BIOSReleaseDate")}
    };

    const json wmi = RunPowerShellJson(R"PS(
$cs = Get-CimInstance Win32_ComputerSystem | Select-Object -First 1
$bios = Get-CimInstance Win32_BIOS | Select-Object -First 1
$csp = Get-CimInstance Win32_ComputerSystemProduct | Select-Object -First 1
[pscustomobject]@{
  manufacturer = $cs.Manufacturer
  model = $cs.Model
  asset_tag = $bios.SMBIOSAssetTag
  serial_number = $bios.SerialNumber
  bios_vendor = $bios.Manufacturer
  bios_version = ($bios.SMBIOSBIOSVersion)
  bios_date = if ($bios.ReleaseDate) { $bios.ReleaseDate.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
  device_uuid = $csp.UUID
  sku = $cs.SystemSKUNumber
  system_family = $cs.SystemFamily
} | ConvertTo-Json -Depth 4 -Compress
)PS", json::object());

    if (wmi.is_object()) {
        for (auto it = wmi.begin(); it != wmi.end(); ++it) {
            if (!it.value().is_null() && !(it.value().is_string() && it.value().get<std::string>().empty())) {
                hw[it.key()] = it.value();
            }
        }
    }
    return hw;
}

json CpuInfo() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    return {
        {"name", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"ProcessorNameString")},
        {"vendor", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"VendorIdentifier")},
        {"logical_processors", static_cast<int>(si.dwNumberOfProcessors)},
        {"cores", static_cast<int>(si.dwNumberOfProcessors)},
        {"max_clock_mhz", RegDword(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"~MHz", 0)}
    };
}

json MemoryInfo() {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) return json::object();
    const auto used = mem.ullTotalPhys > mem.ullAvailPhys ? mem.ullTotalPhys - mem.ullAvailPhys : 0;
    const double usedPercent = mem.ullTotalPhys ? (static_cast<double>(used) * 100.0 / static_cast<double>(mem.ullTotalPhys)) : 0.0;
    return {
        {"total_bytes", static_cast<std::uint64_t>(mem.ullTotalPhys)},
        {"available_bytes", static_cast<std::uint64_t>(mem.ullAvailPhys)},
        {"used_bytes", static_cast<std::uint64_t>(used)},
        {"used_percent", usedPercent},
        {"usage_percent", usedPercent},
        {"memory_load_percent", static_cast<int>(mem.dwMemoryLoad)}
    };
}

json BitLockerVolumes() {
    const json result = RunPowerShellJson(R"PS(
$items = @()
if (Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue) {
  $items = Get-BitLockerVolume | ForEach-Object {
    [pscustomobject]@{
      drive = $_.MountPoint
      mount_point = $_.MountPoint
      volume_status = [string]$_.VolumeStatus
      protection_status = [string]$_.ProtectionStatus
      encryption_percentage = [int]$_.EncryptionPercentage
      lock_status = [string]$_.LockStatus
      encryption_method = [string]$_.EncryptionMethod
      key_protector_count = @($_.KeyProtector).Count
      auto_unlock_enabled = $_.AutoUnlockEnabled
    }
  }
}
@($items) | ConvertTo-Json -Depth 5 -Compress
)PS", json::array());
    return result.is_array() ? result : json::array({result});
}

json StorageInfo(const json& bitlockerVolumes) {
    json drives = json::array();
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        wchar_t root[] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;

        ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
        if (!GetDiskFreeSpaceExW(root, &freeAvail, &total, &freeTotal)) continue;

        wchar_t fsName[64]{};
        wchar_t volumeName[MAX_PATH]{};
        DWORD serial = 0;
        DWORD maxComponent = 0;
        DWORD flags = 0;
        GetVolumeInformationW(root, volumeName, MAX_PATH, &serial, &maxComponent, &flags, fsName, static_cast<DWORD>(sizeof(fsName) / sizeof(fsName[0])));

        const auto totalBytes = static_cast<std::uint64_t>(total.QuadPart);
        const auto freeBytes = static_cast<std::uint64_t>(freeTotal.QuadPart);
        const auto usedBytes = totalBytes > freeBytes ? totalBytes - freeBytes : 0;
        const double usedPercent = totalBytes ? (static_cast<double>(usedBytes) * 100.0 / static_cast<double>(totalBytes)) : 0.0;
        const std::string drive = std::string(1, static_cast<char>('A' + i)) + ":";

        json bitlocker = json::object();
        if (bitlockerVolumes.is_array()) {
            for (const auto& bl : bitlockerVolumes) {
                const std::string blDrive = bl.value("drive", bl.value("mount_point", ""));
                if (blDrive == drive || blDrive == drive + "\\") {
                    bitlocker = bl;
                    break;
                }
            }
        }

        drives.push_back({
            {"drive", drive},
            {"mount", WideZToUtf8(root)},
            {"label", WideZToUtf8(volumeName)},
            {"filesystem", WideZToUtf8(fsName)},
            {"type", type == DRIVE_FIXED ? "Fixed" : type == DRIVE_REMOVABLE ? "Removable" : "Network"},
            {"total_bytes", totalBytes},
            {"free_bytes", freeBytes},
            {"used_bytes", usedBytes},
            {"used_percent", usedPercent},
            {"bitlocker", bitlocker},
            {"bitlocker_status", bitlocker.value("protection_status", "")},
            {"encryption_percentage", bitlocker.value("encryption_percentage", json(nullptr))}
        });
    }
    return drives;
}

std::string MacToString(const BYTE* addr, ULONG len) {
    if (!addr || len == 0) return {};
    std::ostringstream os;
    for (ULONG i = 0; i < len; ++i) {
        if (i) os << "-";
        os << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(addr[i]);
    }
    return os.str();
}

json NetworkInfo() {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    DWORD rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size);
    }

    json list = json::array();
    if (rc != NO_ERROR) return {{"adapters", list}};

    char addrBuf[INET6_ADDRSTRLEN]{};
    for (auto* a = adapters; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        json ipv4 = json::array();
        json ipv6 = json::array();
        json dns = json::array();
        json gateway = json::array();

        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            void* src = nullptr;
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                src = &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
                if (InetNtopA(AF_INET, src, addrBuf, sizeof(addrBuf))) ipv4.push_back(addrBuf);
            } else if (u->Address.lpSockaddr->sa_family == AF_INET6) {
                src = &reinterpret_cast<sockaddr_in6*>(u->Address.lpSockaddr)->sin6_addr;
                if (InetNtopA(AF_INET6, src, addrBuf, sizeof(addrBuf))) ipv6.push_back(addrBuf);
            }
        }
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            if (!d->Address.lpSockaddr) continue;
            void* src = nullptr;
            if (d->Address.lpSockaddr->sa_family == AF_INET) {
                src = &reinterpret_cast<sockaddr_in*>(d->Address.lpSockaddr)->sin_addr;
                if (InetNtopA(AF_INET, src, addrBuf, sizeof(addrBuf))) dns.push_back(addrBuf);
            } else if (d->Address.lpSockaddr->sa_family == AF_INET6) {
                src = &reinterpret_cast<sockaddr_in6*>(d->Address.lpSockaddr)->sin6_addr;
                if (InetNtopA(AF_INET6, src, addrBuf, sizeof(addrBuf))) dns.push_back(addrBuf);
            }
        }
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            if (!g->Address.lpSockaddr) continue;
            void* src = nullptr;
            if (g->Address.lpSockaddr->sa_family == AF_INET) {
                src = &reinterpret_cast<sockaddr_in*>(g->Address.lpSockaddr)->sin_addr;
                if (InetNtopA(AF_INET, src, addrBuf, sizeof(addrBuf))) gateway.push_back(addrBuf);
            } else if (g->Address.lpSockaddr->sa_family == AF_INET6) {
                src = &reinterpret_cast<sockaddr_in6*>(g->Address.lpSockaddr)->sin6_addr;
                if (InetNtopA(AF_INET6, src, addrBuf, sizeof(addrBuf))) gateway.push_back(addrBuf);
            }
        }

        std::string connectionType = "Other";
        if (a->IfType == IF_TYPE_IEEE80211) connectionType = "Wi-Fi";
        else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) connectionType = "Ethernet";

        list.push_back({
            {"name", WideZToUtf8(a->FriendlyName)},
            {"adapter", WideZToUtf8(a->FriendlyName)},
            {"description", WideZToUtf8(a->Description)},
            {"mac", MacToString(a->PhysicalAddress, a->PhysicalAddressLength)},
            {"mac_address", MacToString(a->PhysicalAddress, a->PhysicalAddressLength)},
            {"status", a->OperStatus == IfOperStatusUp ? "Up" : "Down"},
            {"connection", connectionType},
            {"connection_type", connectionType},
            {"ipv4", ipv4},
            {"ipv6", ipv6},
            {"dns", dns},
            {"gateway", gateway},
            {"speed_bps", static_cast<std::uint64_t>(a->TransmitLinkSpeed)}
        });
    }

    json primary = json::object();
    for (const auto& a : list) {
        if (a.value("status", "") == "Up" && a.contains("ipv4") && a["ipv4"].is_array() && !a["ipv4"].empty()) {
            const std::string ip = a["ipv4"][0].get<std::string>();
            if (ip.rfind("169.254.", 0) != 0) {
                primary = a;
                break;
            }
        }
    }
    if (primary.empty() && !list.empty()) primary = list[0];

    json out = {
        {"adapters", list},
        {"primary", primary},
        {"primary_ipv4", primary.contains("ipv4") && primary["ipv4"].is_array() && !primary["ipv4"].empty() ? primary["ipv4"][0] : json(nullptr)},
        {"public_ip", json(nullptr)},
        {"mac", primary.value("mac", "")},
        {"mac_address", primary.value("mac", "")},
        {"adapter", primary.value("name", "")},
        {"connection", primary.value("connection", "")},
        {"gateway", primary.contains("gateway") && primary["gateway"].is_array() && !primary["gateway"].empty() ? primary["gateway"][0] : json(nullptr)},
        {"dns", primary.contains("dns") ? primary["dns"] : json::array()},
        {"domain_workgroup", DomainOrWorkgroup()}
    };
    return out;
}

std::string ServiceState(const wchar_t* serviceName) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return "Unknown";
    SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return "Not installed";
    }
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    const BOOL ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!ok) return "Unknown";
    switch (ssp.dwCurrentState) {
    case SERVICE_RUNNING: return "Running";
    case SERVICE_STOPPED: return "Stopped";
    case SERVICE_START_PENDING: return "Starting";
    case SERVICE_STOP_PENDING: return "Stopping";
    case SERVICE_PAUSED: return "Paused";
    default: return "Other";
    }
}

bool ServiceRunning(const wchar_t* serviceName) {
    return ServiceState(serviceName) == "Running";
}

json LocalAdmins() {
    return RunPowerShellJson(R"PS(
$items = @()
if (Get-Command Get-LocalGroupMember -ErrorAction SilentlyContinue) {
  $items = Get-LocalGroupMember -Group 'Administrators' | ForEach-Object { [pscustomobject]@{ name = $_.Name; object_class = [string]$_.ObjectClass; principal_source = [string]$_.PrincipalSource } }
}
[pscustomobject]@{ count = @($items).Count; members = @($items) } | ConvertTo-Json -Depth 5 -Compress
)PS", { {"count", json(nullptr)}, {"members", json::array()} });
}

json TpmInfo() {
    return RunPowerShellJson(R"PS(
$tpmCim = Get-CimInstance -Namespace root\CIMV2\Security\MicrosoftTpm -ClassName Win32_Tpm | Select-Object -First 1
$tpmCmd = $null
if (Get-Command Get-Tpm -ErrorAction SilentlyContinue) { $tpmCmd = Get-Tpm }
[pscustomobject]@{
  present = [bool]($tpmCim -or ($tpmCmd -and $tpmCmd.TpmPresent))
  enabled = if ($tpmCmd) { $tpmCmd.TpmEnabled } elseif ($tpmCim) { $tpmCim.IsEnabled_InitialValue } else { $null }
  activated = if ($tpmCmd) { $tpmCmd.TpmActivated } elseif ($tpmCim) { $tpmCim.IsActivated_InitialValue } else { $null }
  owned = if ($tpmCmd) { $tpmCmd.TpmOwned } elseif ($tpmCim) { $tpmCim.IsOwned_InitialValue } else { $null }
  ready = if ($tpmCmd) { $tpmCmd.TpmReady } else { $null }
  manufacturer_id = if ($tpmCim) { $tpmCim.ManufacturerId } else { $null }
  manufacturer_version = if ($tpmCim) { $tpmCim.ManufacturerVersion } else { $null }
  spec_version = if ($tpmCim) { $tpmCim.SpecVersion } else { $null }
} | ConvertTo-Json -Depth 4 -Compress
)PS", json::object());
}

json SecurityInfo(const json& bitlockerVolumes, const json& tpm, const json& localAdmins) {
    const DWORD disableAntiSpyware = RegDword(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows Defender)", L"DisableAntiSpyware", 0);
    const DWORD disableRealtime = RegDword(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows Defender\Real-Time Protection)", L"DisableRealtimeMonitoring", 0);

    bool secureBootEnabled = false;
    bool secureBootKnown = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LR"(SYSTEM\CurrentControlSet\Control\SecureBoot\State)", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD type = 0;
        DWORD bytes = sizeof(value);
        if (RegQueryValueExW(key, L"UEFISecureBootEnabled", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &bytes) == ERROR_SUCCESS && type == REG_DWORD) {
            secureBootEnabled = value != 0;
            secureBootKnown = true;
        }
        RegCloseKey(key);
    }

    std::string bitlockerStatus = "Unknown";
    if (bitlockerVolumes.is_array() && !bitlockerVolumes.empty()) {
        bitlockerStatus = "Off";
        for (const auto& bl : bitlockerVolumes) {
            const std::string protection = bl.value("protection_status", "");
            if (protection == "On" || protection == "1" || protection == "ProtectionOn") {
                bitlockerStatus = "On";
                break;
            }
        }
    }

    return {
        {"defender_service_running", ServiceRunning(L"WinDefend")},
        {"defender_enabled", disableAntiSpyware == 0 && ServiceRunning(L"WinDefend")},
        {"defender_realtime_enabled", disableRealtime == 0},
        {"defender_real_time", disableRealtime == 0 ? "Enabled" : "Disabled"},
        {"firewall_enabled", ServiceRunning(L"MpsSvc")},
        {"firewall_service_running", ServiceRunning(L"MpsSvc")},
        {"secure_boot_enabled", secureBootKnown ? json(secureBootEnabled) : json(nullptr)},
        {"secure_boot", secureBootKnown ? (secureBootEnabled ? "Enabled" : "Disabled") : "Unknown"},
        {"tpm_present", tpm.value("present", ServiceRunning(L"TBS"))},
        {"tpm_enabled", tpm.value("enabled", json(nullptr))},
        {"tpm_version", tpm.value("spec_version", json(nullptr))},
        {"tpm", tpm},
        {"bitlocker_status", bitlockerStatus},
        {"bitlocker", bitlockerVolumes},
        {"local_admin_count", localAdmins.value("count", json(nullptr))},
        {"local_admins", localAdmins.value("members", json::array())}
    };
}

std::string RegStringFromOpenedKey(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    LONG rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || bytes == 0 || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    if (rc != ERROR_SUCCESS) return {};
    while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    return WideToUtf8(buffer);
}

DWORD RegDwordFromOpenedKey(HKEY key, const wchar_t* name, DWORD fallback = 0) {
    DWORD type = 0;
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    const LONG rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &bytes);
    return (rc == ERROR_SUCCESS && type == REG_DWORD) ? value : fallback;
}

json InstalledSoftwareInventory() {
    struct RootKey { HKEY root; const wchar_t* path; const char* scope; REGSAM view; };
    const RootKey roots[] = {
        {HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)", "machine64", KEY_WOW64_64KEY},
        {HKEY_LOCAL_MACHINE, LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)", "machine32", KEY_WOW64_64KEY},
        {HKEY_CURRENT_USER, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)", "user", 0}
    };

    json items = json::array();
    for (const auto& rk : roots) {
        HKEY rootKey = nullptr;
        if (RegOpenKeyExW(rk.root, rk.path, 0, KEY_READ | rk.view, &rootKey) != ERROR_SUCCESS) continue;

        for (DWORD index = 0;; ++index) {
            wchar_t subName[512]{};
            DWORD subNameLen = static_cast<DWORD>(sizeof(subName) / sizeof(subName[0]));
            const LONG rc = RegEnumKeyExW(rootKey, index, subName, &subNameLen, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;

            HKEY appKey = nullptr;
            if (RegOpenKeyExW(rootKey, subName, 0, KEY_READ | rk.view, &appKey) != ERROR_SUCCESS) continue;
            const std::string name = RegStringFromOpenedKey(appKey, L"DisplayName");
            if (!name.empty() && RegDwordFromOpenedKey(appKey, L"SystemComponent", 0) == 0) {
                items.push_back({
                    {"name", name},
                    {"version", RegStringFromOpenedKey(appKey, L"DisplayVersion")},
                    {"publisher", RegStringFromOpenedKey(appKey, L"Publisher")},
                    {"install_date", RegStringFromOpenedKey(appKey, L"InstallDate")},
                    {"install_location", RegStringFromOpenedKey(appKey, L"InstallLocation")},
                    {"uninstall_string", RegStringFromOpenedKey(appKey, L"UninstallString")},
                    {"quiet_uninstall_string", RegStringFromOpenedKey(appKey, L"QuietUninstallString")},
                    {"estimated_size_kb", RegDwordFromOpenedKey(appKey, L"EstimatedSize", 0)},
                    {"scope", rk.scope},
                    {"registry_key", WideToUtf8(std::wstring(subName, subNameLen))}
                });
            }
            RegCloseKey(appKey);
        }
        RegCloseKey(rootKey);
    }

    std::sort(items.begin(), items.end(), [](const json& a, const json& b) {
        return a.value("name", "") < b.value("name", "");
    });

    json recently = json::array();
    for (const auto& item : items) {
        const std::string d = item.value("install_date", "");
        if (!d.empty()) recently.push_back(item);
    }
    std::sort(recently.begin(), recently.end(), [](const json& a, const json& b) {
        return a.value("install_date", "") > b.value("install_date", "");
    });
    while (recently.size() > 20) recently.erase(recently.end() - 1);

    return {
        {"status", "collected"},
        {"count", items.size()},
        {"installed_apps", items.size()},
        {"items", items},
        {"recently_installed", recently},
        {"recently_installed_count", recently.size()}
    };
}

json WindowsUpdates() {
    return RunPowerShellJson(R"PS(
$updates = @()
$count = $null
$errorText = $null
try {
  $session = New-Object -ComObject Microsoft.Update.Session
  $searcher = $session.CreateUpdateSearcher()
  $result = $searcher.Search("IsInstalled=0 and IsHidden=0 and Type='Software'")
  $count = $result.Updates.Count
  for ($i = 0; $i -lt $result.Updates.Count; $i++) {
    $u = $result.Updates.Item($i)
    $updates += [pscustomobject]@{
      title = $u.Title
      kb = @($u.KBArticleIDs)
      severity = $u.MsrcSeverity
      downloaded = $u.IsDownloaded
      mandatory = $u.IsMandatory
      reboot_required = $u.RebootRequired
      categories = @($u.Categories | ForEach-Object { $_.Name })
    }
  }
} catch { $errorText = $_.Exception.Message }
[pscustomobject]@{
  pending_count = $count
  updates = @($updates)
  last_scan_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  error = $errorText
} | ConvertTo-Json -Depth 8 -Compress
)PS", json::object());
}

json EventHealth() {
    return RunPowerShellJson(R"PS(
$start = (Get-Date).AddDays(-7)
function EventRows($log, $ids, $provider) {
  try {
    $fh = @{ LogName = $log; StartTime = $start; Id = $ids }
    if ($provider) { $fh.ProviderName = $provider }
    @(Get-WinEvent -FilterHashtable $fh -MaxEvents 25 | ForEach-Object {
      [pscustomobject]@{ time_created = $_.TimeCreated.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); id = $_.Id; provider = $_.ProviderName; level = $_.LevelDisplayName; message = ($_.Message -replace "`r?`n", " ").Substring(0, [Math]::Min(350, ($_.Message -replace "`r?`n", " ").Length)) }
    })
  } catch { @() }
}
$shutdowns = EventRows 'System' @(41,6008) $null
$serviceFailures = EventRows 'System' @(7031,7034) 'Service Control Manager'
$updateFailures = EventRows 'System' @(20,25,31,34,43) 'Microsoft-Windows-WindowsUpdateClient'
$appCrashes = EventRows 'Application' @(1000,1001) $null
[pscustomobject]@{
  window_days = 7
  unexpected_shutdowns = @($shutdowns).Count
  service_failures = @($serviceFailures).Count
  update_failures = @($updateFailures).Count
  recent_crashes = @($appCrashes).Count
  shutdown_events = @($shutdowns)
  service_failure_events = @($serviceFailures)
  update_failure_events = @($updateFailures)
  crash_events = @($appCrashes)
} | ConvertTo-Json -Depth 8 -Compress
)PS", json::object());
}

json GpuInfo() {
    json result = RunPowerShellJson(R"PS(
$items = Get-CimInstance Win32_VideoController | ForEach-Object {
  [pscustomobject]@{
    name = $_.Name
    driver_version = $_.DriverVersion
    driver_date = if ($_.DriverDate) { $_.DriverDate.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
    adapter_ram_bytes = if ($_.AdapterRAM) { [int64]$_.AdapterRAM } else { $null }
    video_processor = $_.VideoProcessor
    pnp_device_id = $_.PNPDeviceID
    status = $_.Status
    current_horizontal_resolution = $_.CurrentHorizontalResolution
    current_vertical_resolution = $_.CurrentVerticalResolution
  }
}
@($items) | ConvertTo-Json -Depth 5 -Compress
)PS", json::array());
    if (result.is_object()) result = json::array({result});
    return result.is_array() ? result : json::array();
}

json BatteryInfo() {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) return json::object();
    const bool present = sps.BatteryFlag != 128 && sps.BatteryLifePercent != 255;
    const bool charging = (sps.BatteryFlag & 8) != 0;
    return {
        {"present", present},
        {"battery_present", present},
        {"ac_line_status", sps.ACLineStatus == 1 ? "Online" : sps.ACLineStatus == 0 ? "Offline" : "Unknown"},
        {"charge_percent", sps.BatteryLifePercent == 255 ? json(nullptr) : json(static_cast<int>(sps.BatteryLifePercent))},
        {"battery_percent", sps.BatteryLifePercent == 255 ? json(nullptr) : json(static_cast<int>(sps.BatteryLifePercent))},
        {"charging", charging},
        {"health", present ? "Present" : "No battery"},
        {"battery_life_seconds", sps.BatteryLifeTime == static_cast<DWORD>(-1) ? json(nullptr) : json(static_cast<int>(sps.BatteryLifeTime))}
    };
}

BOOL CALLBACK CountMonitorProc(HMONITOR, HDC, LPRECT, LPARAM data) {
    int* count = reinterpret_cast<int*>(data);
    ++(*count);
    return TRUE;
}

json DisplayInfo(const json& gpu) {
    int count = 0;
    EnumDisplayMonitors(nullptr, nullptr, CountMonitorProc, reinterpret_cast<LPARAM>(&count));
    return {
        {"monitor_count", count},
        {"gpu", gpu.empty() ? json(nullptr) : gpu[0].value("name", "")},
        {"gpu_driver", gpu.empty() ? json(nullptr) : gpu[0].value("driver_version", "")},
        {"gpus", gpu}
    };
}


json DeepHardwareInventory() {
    // One constrained Windows inventory pass (no screen, codec, or input code).
    // A section failure is reported separately so the API can retain the last
    // known good fields instead of replacing them with misleading empty lists.
    return RunPowerShellJson(R"PS(
function SafeDate($value) {
  if (-not $value) { return $null }
  try { return ([datetime]$value).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } catch { return $null }
}
function EdidText($bytes) {
  if (-not $bytes) { return '' }
  return (-join @($bytes | Where-Object { [int]$_ -gt 0 } | ForEach-Object { [char][int]$_ })).Trim()
}
$result = [ordered]@{}
$sections = [ordered]@{}
try {
  $result.memory_modules = @(
    Get-CimInstance Win32_PhysicalMemory -ErrorAction Stop |
      Select-Object -First 64 | ForEach-Object {
        [pscustomobject]@{
          device_locator = $_.DeviceLocator
          bank_label = $_.BankLabel
          capacity_bytes = [int64]$_.Capacity
          manufacturer = $_.Manufacturer
          part_number = ([string]$_.PartNumber).Trim()
          serial_number = $_.SerialNumber
          configured_speed_mhz = if ($_.ConfiguredClockSpeed) { [int]$_.ConfiguredClockSpeed } else { $null }
          speed_mhz = if ($_.Speed) { [int]$_.Speed } else { $null }
        }
      }
  )
  $board = Get-CimInstance Win32_BaseBoard -ErrorAction Stop | Select-Object -First 1
  $bios = Get-CimInstance Win32_BIOS -ErrorAction Stop | Select-Object -First 1
  $result.motherboard = [pscustomobject]@{
    manufacturer = $board.Manufacturer
    product = $board.Product
    version = $board.Version
    serial_number = $board.SerialNumber
    bios_manufacturer = $bios.Manufacturer
    bios_version = $bios.SMBIOSBIOSVersion
    smbios_version = if ($bios.SMBIOSMajorVersion -ne $null) { "$($bios.SMBIOSMajorVersion).$($bios.SMBIOSMinorVersion)" } else { $null }
  }
  $sections.core_hardware = @{ status = 'collected' }
} catch { $sections.core_hardware = @{ status = 'failed' } }
try {
  $disks = @(Get-PhysicalDisk -ErrorAction Stop | Select-Object -First 64)
  $result.physical_disks = @(
    $disks | ForEach-Object {
      $disk = $_
      $reliability = $null
      try { $reliability = Get-StorageReliabilityCounter -PhysicalDisk $disk -ErrorAction Stop } catch {}
      [pscustomobject]@{
        friendly_name = $disk.FriendlyName
        device_id = [string]$disk.DeviceId
        serial_number = $disk.SerialNumber
        media_type = [string]$disk.MediaType
        bus_type = [string]$disk.BusType
        size_bytes = [int64]$disk.Size
        health_status = [string]$disk.HealthStatus
        operational_status = [string](@($disk.OperationalStatus) -join ', ')
        firmware_version = $disk.FirmwareVersion
        temperature_c = if ($reliability -and $reliability.Temperature -ne $null) { [int]$reliability.Temperature } else { $null }
        wear_percent = if ($reliability -and $reliability.Wear -ne $null) { [int]$reliability.Wear } else { $null }
      }
    }
  )
  $sections.storage = @{ status = 'collected' }
} catch {
  try {
    $result.physical_disks = @(
      Get-CimInstance Win32_DiskDrive -ErrorAction Stop |
        Select-Object -First 64 | ForEach-Object {
          [pscustomobject]@{
            friendly_name = $_.Model
            device_id = $_.DeviceID
            serial_number = $_.SerialNumber
            media_type = $_.MediaType
            bus_type = $_.InterfaceType
            size_bytes = [int64]$_.Size
            health_status = $null
            operational_status = $_.Status
            firmware_version = $_.FirmwareRevision
            temperature_c = $null
            wear_percent = $null
          }
        }
    )
    $sections.storage = @{ status = 'collected' }
  } catch { $sections.storage = @{ status = 'failed' } }
}
try {
  $result.monitors = @(
    Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction Stop |
      Select-Object -First 64 | ForEach-Object {
        [pscustomobject]@{
          manufacturer = EdidText $_.ManufacturerName
          model = EdidText $_.UserFriendlyName
          product_code = EdidText $_.ProductCodeID
          serial_number = EdidText $_.SerialNumberID
          manufacture_week = if ($_.WeekOfManufacture -gt 0) { [int]$_.WeekOfManufacture } else { $null }
          manufacture_year = if ($_.YearOfManufacture -gt 0) { [int]$_.YearOfManufacture } else { $null }
          active = [bool]$_.Active
        }
      }
  )
  $sections.monitors = @{ status = 'collected' }
} catch { $sections.monitors = @{ status = 'failed' } }
try {
  $result.drivers = @(
    Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop |
      Where-Object { $_.DeviceName -and $_.DriverVersion } |
      Select-Object -First 400 | ForEach-Object {
        [pscustomobject]@{
          device_name = $_.DeviceName
          device_class = $_.DeviceClass
          driver_version = $_.DriverVersion
          driver_date = SafeDate $_.DriverDate
          driver_provider = $_.DriverProviderName
          manufacturer = $_.Manufacturer
          inf_name = $_.InfName
          signed = if ($_.IsSigned -ne $null) { [bool]$_.IsSigned } else { $null }
          signer = $_.Signer
        }
      }
  )
  $sections.drivers = @{ status = 'collected' }
} catch { $sections.drivers = @{ status = 'failed' } }
try {
  $problem = @(Get-CimInstance Win32_PnPEntity -ErrorAction Stop)
  $result.problem_devices = @(
    $problem | Where-Object {
      $_.ConfigManagerErrorCode -ne $null -and [int]$_.ConfigManagerErrorCode -ne 0
    } | Select-Object -First 128 | ForEach-Object {
      [pscustomobject]@{
        name = $_.Name
        manufacturer = $_.Manufacturer
        pnp_class = $_.PNPClass
        error_code = [int]$_.ConfigManagerErrorCode
        status = $_.Status
        device_id = $_.PNPDeviceID
      }
    }
  )
  $sections.problem_devices = @{ status = 'collected' }
} catch { $sections.problem_devices = @{ status = 'failed' } }
try {
  $result.printers = @(
    Get-CimInstance Win32_Printer -ErrorAction Stop |
      Select-Object -First 64 | ForEach-Object {
        [pscustomobject]@{
          name = $_.Name
          driver_name = $_.DriverName
          port_name = $_.PortName
          network = [bool]$_.Network
          default = [bool]$_.Default
        }
      }
  )
  $result.usb_devices = @(
    Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'USB%'" -ErrorAction Stop |
      Select-Object -First 128 | ForEach-Object {
        [pscustomobject]@{
          name = $_.Name
          manufacturer = $_.Manufacturer
          pnp_class = $_.PNPClass
          status = $_.Status
          device_id = $_.PNPDeviceID
        }
      }
  )
  $sections.peripherals = @{ status = 'collected' }
} catch { $sections.peripherals = @{ status = 'failed' } }


try {
  $full = Get-CimInstance -Namespace root\wmi -ClassName BatteryFullChargedCapacity -ErrorAction Stop | Select-Object -First 1
  $static = Get-CimInstance -Namespace root\wmi -ClassName BatteryStaticData -ErrorAction Stop | Select-Object -First 1
  $cycles = Get-CimInstance -Namespace root\wmi -ClassName BatteryCycleCount -ErrorAction SilentlyContinue | Select-Object -First 1
  $batteryStatus = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue | Select-Object -First 1
  $result.battery_deep = [pscustomobject]@{
    full_charge_capacity_mwh = if ($full -and $full.FullChargedCapacity -ne $null) { [int64]$full.FullChargedCapacity } else { $null }
    design_capacity_mwh = if ($static -and $static.DesignedCapacity -ne $null) { [int64]$static.DesignedCapacity } else { $null }
    cycle_count = if ($cycles -and $cycles.CycleCount -ne $null) { [int]$cycles.CycleCount } else { $null }
    remaining_capacity_mwh = if ($batteryStatus -and $null -ne $batteryStatus.RemainingCapacity) { [int64]$batteryStatus.RemainingCapacity } else { $null }
    voltage_mv = if ($batteryStatus -and $null -ne $batteryStatus.Voltage) { [int64]$batteryStatus.Voltage } else { $null }
    rate_mw = if ($batteryStatus -and $batteryStatus.DischargeRate -gt 0) { -[int64]$batteryStatus.DischargeRate } elseif ($batteryStatus -and $batteryStatus.ChargeRate -gt 0) { [int64]$batteryStatus.ChargeRate } else { $null }
    health_percent = if ($static -and $static.DesignedCapacity -gt 0 -and $full) {
      [math]::Round(100 * [double]$full.FullChargedCapacity / [double]$static.DesignedCapacity)
    } else { $null }
  }
  $sections.battery = @{ status='collected' }
} catch { $sections.battery = @{ status='failed'; reason=[string]$_.Exception.Message } }

# Extended read-only Windows inventory. Every section reports its own outcome.
try {
  $result.installed_hotfixes = @(Get-HotFix -ErrorAction Stop | Sort-Object InstalledOn -Descending |
    Select-Object -First 300 | ForEach-Object {
      [pscustomobject]@{ hotfix_id=$_.HotFixID; description=$_.Description; installed_on=SafeDate $_.InstalledOn; installed_by=$_.InstalledBy }
    })
  $license = Get-CimInstance SoftwareLicensingProduct -Filter 'PartialProductKey IS NOT NULL' -ErrorAction Stop |
    Where-Object { $_.Name -like '*Windows*' } | Sort-Object LicenseStatus -Descending | Select-Object -First 1
  $licenseState = if ($license) {
    switch ([int]$license.LicenseStatus) {
      1 { 'Licensed' }; 0 { 'Unlicensed' }; 2 { 'OOB grace' }; 3 { 'OOT grace' }
      4 { 'Non-genuine grace' }; 5 { 'Notification' }; 6 { 'Extended grace' }; default { 'Unknown' }
    }
  } else { $null }
  $result.windows_licensing = [pscustomobject]@{
    status=$licenseState; name=$license.Name; description=$license.Description; partial_product_key=$license.PartialProductKey
  }
  $reasons = @()
  if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') { $reasons += 'Component servicing' }
  if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') { $reasons += 'Windows Update' }
  $pendingRename = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
  if ($pendingRename -and $pendingRename.PendingFileRenameOperations) { $reasons += 'Pending file rename' }
  $result.reboot_state = [pscustomobject]@{ pending=($reasons.Count -gt 0); reasons=@($reasons) }
  $result.startup_items = @(Get-CimInstance Win32_StartupCommand -ErrorAction Stop |
    Select-Object -First 200 | ForEach-Object {
      [pscustomobject]@{ name=$_.Name; command=$_.Command; location=$_.Location; user=$_.User }
    })
  $sections.windows_state = @{ status='collected' }
} catch { $sections.windows_state = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $result.scheduled_tasks = @(Get-ScheduledTask -ErrorAction Stop | Select-Object -First 500 |
    ForEach-Object {
      [pscustomobject]@{ name=[string]$_.TaskName; path=[string]$_.TaskPath; state=[string]$_.State; author=[string]$_.Author; description=[string]$_.Description }
    })
  $sections.scheduled_tasks = @{ status='collected' }
} catch { $sections.scheduled_tasks = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $adminSids = $null
  try {
    $adminSids = @(Get-LocalGroupMember -SID 'S-1-5-32-544' -ErrorAction Stop | ForEach-Object { [string]$_.SID })
  } catch {}
  $result.local_users = [pscustomobject]@{
    users = @(Get-LocalUser -ErrorAction Stop | Select-Object -First 256 | ForEach-Object {
      [pscustomobject]@{
        name=$_.Name; full_name=$_.FullName; description=$_.Description; enabled=[bool]$_.Enabled; is_admin=if ($null -ne $adminSids) { $adminSids -contains [string]$_.SID } else { $null }
        last_logon=SafeDate $_.LastLogon; password_last_set=SafeDate $_.PasswordLastSet; sid=[string]$_.SID
      }
    })
  }
  $sections.local_users = @{ status='collected' }
} catch { $sections.local_users = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $result.local_groups = @(Get-LocalGroup -ErrorAction Stop | Select-Object -First 128 | ForEach-Object {
    $g = $_
    $members = @()
    try {
      $members = @(Get-LocalGroupMember -SID $g.SID -ErrorAction Stop | Select-Object -First 128 |
        ForEach-Object { [pscustomobject]@{ name=$_.Name; sid=[string]$_.SID; object_class=[string]$_.ObjectClass } })
    } catch {}
    [pscustomobject]@{ name=$g.Name; sid=[string]$g.SID; description=$g.Description; members=@($members) }
  })
  $sections.local_groups = @{ status='collected' }
} catch { $sections.local_groups = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $result.optional_features = @(Get-CimInstance Win32_OptionalFeature -Filter 'InstallState=1' -ErrorAction Stop |
    Select-Object -First 256 | ForEach-Object { [pscustomobject]@{ name=$_.Name; state='Enabled' } })
  $plan = Get-CimInstance -Namespace root\cimv2\power -Class Win32_PowerPlan -ErrorAction Stop |
    Where-Object { $_.IsActive } | Select-Object -First 1
  $result.power_plan = if ($plan) { $plan.ElementName } else { $null }
  $sections.features_power = @{ status='collected' }
} catch { $sections.features_power = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $result.network_profiles = @(Get-NetConnectionProfile -ErrorAction Stop | Select-Object -First 128 |
    ForEach-Object {
      [pscustomobject]@{ name=$_.Name; interface_alias=$_.InterfaceAlias; network_category=[string]$_.NetworkCategory
        ipv4_connectivity=[string]$_.IPv4Connectivity; ipv6_connectivity=[string]$_.IPv6Connectivity }
    })
  $result.network_configurations = @(Get-CimInstance Win32_NetworkAdapterConfiguration -Filter 'IPEnabled=TRUE' -ErrorAction Stop |
    Select-Object -First 128 | ForEach-Object {
      [pscustomobject]@{ description=$_.Description; mac_address=$_.MACAddress
        ip_addresses=@($_.IPAddress | Where-Object { $_ }); dhcp_enabled=[bool]$_.DHCPEnabled
        dhcp_server=$_.DHCPServer; dns_servers=@($_.DNSServerSearchOrder | Where-Object { $_ })
        dns_domain=$_.DNSDomain; gateways=@($_.DefaultIPGateway | Where-Object { $_ }) }
    })
  $result.wifi_interfaces = @(Get-NetAdapter -ErrorAction Stop | Where-Object { $_.ifType -eq 71 } |
    Select-Object -First 16 | ForEach-Object {
      [pscustomobject]@{ name=$_.Name; state=[string]$_.Status; ssid=$null; bssid=$null
        signal=$null; channel=$null; radio_type=$null; authentication=$null; cipher=$null }
    })
  # Wi-Fi association details are not guaranteed for a Windows service; leave unknown rather than inventing them.
  $result.default_routes = @(Get-NetRoute -ErrorAction Stop |
    Where-Object { $_.DestinationPrefix -eq '0.0.0.0/0' -or $_.DestinationPrefix -eq '::/0' } |
    Select-Object -First 64 | ForEach-Object {
      [pscustomobject]@{ destination=$_.DestinationPrefix; next_hop=$_.NextHop
        interface_alias=$_.InterfaceAlias; route_metric=$_.RouteMetric; protocol=[string]$_.Protocol }
    })
  $sections.network = @{ status='collected' }
} catch { $sections.network = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop | Select-Object -First 1
  $dsreg = @(dsregcmd.exe /status 2>$null)
  $ds = @{}
  foreach ($line in $dsreg) {
    if ($line -match '^\s*(AzureAdJoined|WorkplaceJoined|DeviceId|TenantId|TenantName)\s*:\s*(.*?)\s*$') {
      $ds[$Matches[1]] = $Matches[2].Trim()
    }
  }
  $result.directory_join = [pscustomobject]@{
    computer_name=$env:COMPUTERNAME
    domain_joined=[bool]$cs.PartOfDomain
    domain=if ($cs.PartOfDomain) { $cs.Domain } else { $null }
    workgroup=if (-not $cs.PartOfDomain) { $cs.Domain } else { $null }
    domain_role=[int]$cs.DomainRole
    azure_ad_joined=if ($ds.AzureAdJoined) { $ds.AzureAdJoined -eq 'YES' } else { $null }
    workplace_joined=if ($ds.WorkplaceJoined) { $ds.WorkplaceJoined -eq 'YES' } else { $null }
    entra_device_id=$ds.DeviceId
    entra_tenant_id=$ds.TenantId
    entra_tenant_name=$ds.TenantName
  }
  $sections.directory_join = @{ status='collected' }
} catch { $sections.directory_join = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $result.machine_certificates = @(Get-ChildItem Cert:\LocalMachine\My -ErrorAction Stop |
    Select-Object -First 256 | ForEach-Object {
      [pscustomobject]@{
        subject=$_.Subject; issuer=$_.Issuer; thumbprint=$_.Thumbprint
        not_before=SafeDate $_.NotBefore; not_after=SafeDate $_.NotAfter
        has_private_key=[bool]$_.HasPrivateKey
      }
    })
  $sections.certificates = @{ status='collected' }
} catch { $sections.certificates = @{ status='failed'; reason=[string]$_.Exception.Message } }

try {
  $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop | Select-Object -First 1
  $cpu = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1
  $result.virtualization = [pscustomobject]@{
    hypervisor_present=[bool]$cs.HypervisorPresent
    virtualization_firmware_enabled=if ($null -ne $cpu.VirtualizationFirmwareEnabled) { [bool]$cpu.VirtualizationFirmwareEnabled } else { $null }
    vm_monitor_mode_extensions=if ($null -ne $cpu.VMMonitorModeExtensions) { [bool]$cpu.VMMonitorModeExtensions } else { $null }
    second_level_address_translation=if ($null -ne $cpu.SecondLevelAddressTranslationExtensions) { [bool]$cpu.SecondLevelAddressTranslationExtensions } else { $null }
    data_execution_prevention_available=if ($null -ne $cpu.DataExecutionPrevention_Available) { [bool]$cpu.DataExecutionPrevention_Available } else { $null }
  }
  $sections.virtualization = @{ status='collected' }
} catch { $sections.virtualization = @{ status='failed'; reason=[string]$_.Exception.Message } }

$result.deep_inventory_sections = $sections
$result | ConvertTo-Json -Depth 8 -Compress
)PS", json::object());
}

json DueDeepHardwareInventory(bool force) {
    static std::mutex mu;
    static std::chrono::steady_clock::time_point retryAfter{};
    std::lock_guard<std::mutex> lock(mu);
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < retryAfter) return {{"deep_inventory_included", false}, {"deep_inventory_status", "retry_later"}};

    // A local checkpoint makes the eight-hour cadence survive Agent restarts and
    // disconnected devices. Nothing is scheduled or attempted by the server while offline.
    namespace fs = std::filesystem;
    wchar_t programData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    const fs::path root = length > 0 && length < MAX_PATH ? fs::path(programData) : fs::path(L"C:\\ProgramData");
    const fs::path stamp = root / L"Hi5Central" / L"Agent" / L"deep-inventory-last-success.txt";
    if (!force) {
        std::ifstream saved(stamp);
        std::int64_t previous = 0;
        if (saved >> previous) {
            const auto seconds = static_cast<std::int64_t>(std::time(nullptr));
            if (previous > 0 && seconds >= previous && seconds - previous < 8 * 60 * 60) {
                return {{"deep_inventory_included", false}, {"deep_inventory_status", "not_due"}};
            }
        }
    }

    retryAfter = now + std::chrono::minutes(30);
    json deep = DeepHardwareInventory();
    if (!deep.is_object() || !deep.contains("deep_inventory_sections") ||
        !deep["deep_inventory_sections"].is_object()) {
        return {{"deep_inventory_included", false}, {"deep_inventory_status", "failed"},
                {"deep_inventory_error", "Windows deep collector did not produce valid section statuses"}};
    }
    bool anySuccess = false;
    bool anyFailure = false;
    for (auto it = deep["deep_inventory_sections"].begin(); it != deep["deep_inventory_sections"].end(); ++it) {
        if (!it.value().is_object()) continue;
        const std::string status = it.value().value("status", "");
        if (status == "collected") anySuccess = true;
        else if (status == "failed") anyFailure = true;
    }
    if (!anySuccess) {
        return {{"deep_inventory_included", false}, {"deep_inventory_status", "failed"},
                {"deep_inventory_sections", deep["deep_inventory_sections"]}};
    }
    deep["deep_inventory_included"] = true;
    deep["deep_inventory_status"] = anyFailure ? "partial" : "collected";
    deep["deep_inventory_collected_at"] = NowIsoUtc();
    if (!anyFailure) {
        retryAfter = std::chrono::steady_clock::time_point{};
        std::error_code error;
        fs::create_directories(stamp.parent_path(), error);
        if (!error) {
            std::ofstream saved(stamp, std::ios::trunc);
            if (saved) saved << std::time(nullptr);
        }
    }
    return deep;
}

json SessionsInfo() {
    json sessions = json::array();
    int rdpCount = 0;
    WTS_SESSION_INFOA* sessionInfo = nullptr;
    DWORD count = 0;
    if (WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessionInfo, &count)) {
        for (DWORD i = 0; i < count; ++i) {
            LPSTR user = nullptr;
            DWORD userLen = 0;
            std::string userName;
            if (WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, sessionInfo[i].SessionId, WTSUserName, &user, &userLen) && user) {
                userName = user;
                WTSFreeMemory(user);
            }
            if (sessionInfo[i].SessionId != WTSGetActiveConsoleSessionId() && !userName.empty()) ++rdpCount;
            sessions.push_back({
                {"id", sessionInfo[i].SessionId},
                {"station", sessionInfo[i].pWinStationName ? sessionInfo[i].pWinStationName : ""},
                {"state", static_cast<int>(sessionInfo[i].State)},
                {"user", userName}
            });
        }
        WTSFreeMemory(sessionInfo);
    }
    const std::string current = LoggedInUser();
    return {
        {"current_user", current},
        {"console_user", current},
        {"active_console_user", current},
        {"last_logged_in_user", LastLoggedOnUser()},
        {"rdp_sessions", rdpCount},
        {"locked", json(nullptr)},
        {"idle_time", json(nullptr)},
        {"sessions", sessions}
    };
}

json ServicesSummary() {
    DWORD bytesNeeded = 0;
    DWORD servicesReturned = 0;
    DWORD resume = 0;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return json::object();
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &bytesNeeded, &servicesReturned, &resume, nullptr);
    std::vector<BYTE> buffer(bytesNeeded ? bytesNeeded : 64 * 1024);
    resume = 0;
    BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesNeeded, &servicesReturned, &resume, nullptr);
    CloseServiceHandle(scm);
    if (!ok) return json::object();

    auto* rows = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    int running = 0;
    int stoppedCritical = 0;
    json stopped = json::array();
    const std::vector<std::wstring> critical = { L"WinDefend", L"MpsSvc", L"wuauserv", L"EventLog", L"Dhcp", L"Dnscache", L"LanmanWorkstation" };
    for (DWORD i = 0; i < servicesReturned; ++i) {
        const bool isRunning = rows[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING;
        if (isRunning) ++running;
        std::wstring svcName = rows[i].lpServiceName ? rows[i].lpServiceName : L"";
        if (!isRunning && std::find(critical.begin(), critical.end(), svcName) != critical.end()) {
            ++stoppedCritical;
            stopped.push_back(WideToUtf8(svcName));
        }
    }
    return {
        {"running_services", running},
        {"stopped_critical_services", stoppedCritical},
        {"stopped_critical_service_names", stopped},
        {"win_defend_running", ServiceRunning(L"WinDefend")},
        {"firewall_service_running", ServiceRunning(L"MpsSvc")},
        {"windows_update_service", ServiceState(L"wuauserv")}
    };
}

json AgentInfo(const std::string& collectedAt) {
    std::string state = ServiceState(L"Hi5CentralAgent");
    if (state == "Not installed") state = ServiceState(L"Hi5CentralAgent");
    return {
        {"version", kAgentVersion},
        {"service", state},
        {"service_status", state},
        {"install_path", GetModulePath()},
        {"transport", "websocket"},
        {"websocket", "Connected"},
        {"websocket_status", "Connected"},
        {"last_heartbeat", collectedAt},
        {"inventory_sync", collectedAt}
    };
}

json SummaryInfo(const json& os, const json& hardware, const json& memory, const json& storage) {
    json primaryDrive = storage.is_array() && !storage.empty() ? storage[0] : json::object();
    const auto uptimeSeconds = static_cast<std::uint64_t>(GetTickCount64() / 1000ULL);
    return {
        {"hostname", ComputerName()},
        {"domain", DomainOrWorkgroup()},
        {"domain_workgroup", DomainOrWorkgroup()},
        {"logged_in_user", LoggedInUser()},
        {"os_name", os.value("name", "Windows")},
        {"operating_system", os.value("name", "Windows")},
        {"os_version", os.value("version", "")},
        {"os_build", os.value("build", "")},
        {"architecture", os.value("architecture", "")},
        {"manufacturer", hardware.value("manufacturer", "")},
        {"model", hardware.value("model", "")},
        {"serial_number", hardware.value("serial_number", "")},
        {"asset_tag", hardware.value("asset_tag", "")},
        {"device_uuid", hardware.value("device_uuid", "")},
        {"total_memory_bytes", memory.value("total_bytes", 0ULL)},
        {"primary_drive", primaryDrive.value("drive", "")},
        {"primary_drive_used_percent", primaryDrive.value("used_percent", 0.0)},
        {"uptime_seconds", uptimeSeconds},
        {"uptime", DurationText(uptimeSeconds)},
        {"last_boot", os.value("last_boot", "")},
        {"install_date", os.value("install_date", "")}
    };
}

json HealthInfo(const json& memory, const json& storage, const json& security, const json& events, const json& updates) {
    const double memPct = memory.value("used_percent", 0.0);
    double diskPct = 0.0;
    if (storage.is_array() && !storage.empty()) diskPct = storage[0].value("used_percent", 0.0);

    json warnings = json::array();
    if (memPct >= 90.0) warnings.push_back("High memory usage");
    if (diskPct >= 90.0) warnings.push_back("Low disk space");
    if (security.value("defender_enabled", true) == false) warnings.push_back("Microsoft Defender is not running");
    if (security.value("firewall_enabled", true) == false) warnings.push_back("Windows Firewall service is not running");
    if (events.value("unexpected_shutdowns", 0) > 0) warnings.push_back("Unexpected shutdowns detected");
    if (events.value("recent_crashes", 0) > 0) warnings.push_back("Recent application crashes detected");
    if (updates.value("pending_count", 0) > 0) warnings.push_back("Windows updates pending");

    return {
        {"memory_warning", memPct >= 90.0},
        {"disk_warning", diskPct >= 90.0},
        {"security_warning", security.value("defender_enabled", true) == false || security.value("firewall_enabled", true) == false},
        {"update_failures", events.value("update_failures", json(nullptr))},
        {"recent_crashes", events.value("recent_crashes", json(nullptr))},
        {"unexpected_shutdowns", events.value("unexpected_shutdowns", json(nullptr))},
        {"service_failures", events.value("service_failures", json(nullptr))},
        {"warnings", warnings},
        {"overall", warnings.empty() ? "good" : "warning"}
    };
}

} // namespace

json BuildInventorySnapshot(const AgentIdentity& identity, bool forceDeep, bool allowScheduledDeep) {
    const auto collectedAt = NowIsoUtc();

    json bitlocker = BitLockerVolumes();
    json tpm = TpmInfo();
    json localAdmins = LocalAdmins();
    json os = OsInfo();
    json hardware = HardwareInfo();
    json cpu = CpuInfo();
    json memory = MemoryInfo();
    json storage = StorageInfo(bitlocker);
    json security = SecurityInfo(bitlocker, tpm, localAdmins);
    json network = NetworkInfo();
    json battery = BatteryInfo();
    json gpu = GpuInfo();
    json displays = DisplayInfo(gpu);
    json sessions = SessionsInfo();
    json software = InstalledSoftwareInventory();
    json updates = WindowsUpdates();
    json events = EventHealth();
    json services = ServicesSummary();
    json agent = AgentInfo(collectedAt);
    json summary = SummaryInfo(os, hardware, memory, storage);
    json health = HealthInfo(memory, storage, security, events, updates);
    json deep = (forceDeep || allowScheduledDeep) ? DueDeepHardwareInventory(forceDeep)
                                                    : json{{"deep_inventory_included", false}};

    json snapshot = {
        {"type", "inventory_snapshot"},
        {"device_id", identity.deviceId},
        {"collected_at", collectedAt},
        {"summary", summary},
        {"hardware", hardware},
        {"warranty_identity", {
            {"manufacturer", hardware.value("manufacturer", "")},
            {"model", hardware.value("model", "")},
            {"serial_number", hardware.value("serial_number", "")},
            {"asset_tag", hardware.value("asset_tag", "")},
            {"device_uuid", hardware.value("device_uuid", "")},
            {"bios_vendor", hardware.value("bios_vendor", "")},
            {"bios_version", hardware.value("bios_version", "")},
            {"bios_date", hardware.value("bios_date", "")}
        }},
        {"os", os},
        {"cpu", cpu},
        {"memory", memory},
        {"storage", storage},
        {"security", security},
        {"network", network},
        {"sessions", sessions},
        {"displays", displays},
        {"gpu", gpu},
        {"battery", battery},
        {"agent", agent},
        {"health", health},
        {"software", software},
        {"software_summary", {
            {"status", software.value("status", "collected")},
            {"count", software.value("count", 0)},
            {"installed_apps", software.value("installed_apps", 0)},
            {"recently_installed", software.value("recently_installed_count", 0)},
            {"items", software.value("items", json::array())}
        }},
        {"services_summary", services},
        {"windows_updates", updates},
        {"updates", updates},
        {"events_summary", events},
        {"event_health", events},
        {"raw", {
            {"collector", "inventory_pass_2"},
            {"notes", "Includes Windows 11 build-name correction, BitLocker, software, updates, event health, GPU, TPM and warranty-ready WMI identity."}
        }}
    };
    snapshot.update(deep);
    if (snapshot.contains("battery_deep") && snapshot["battery_deep"].is_object()) {
        snapshot["battery"].update(snapshot["battery_deep"]);
        snapshot.erase("battery_deep");
    }
    return snapshot;
}

} // namespace hi5
