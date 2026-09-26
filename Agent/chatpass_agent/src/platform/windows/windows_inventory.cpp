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

json PowerShellDiagnosticFallback(
    const json& fallback,
    const char* diagnosticTag,
    const std::string& reason,
    std::size_t outputBytes = 0,
    int processStatus = -1
) {
    if (!diagnosticTag || !*diagnosticTag) return fallback;
    json out = fallback.is_object() ? fallback : json::object();
    out["collector_error"] = std::string(diagnosticTag) + ":" + reason;
    out["collector_output_bytes"] = static_cast<std::uint64_t>(outputBytes);
    if (processStatus >= 0) out["collector_process_status"] = processStatus;
    return out;
}

json RunPowerShellJson(
    const std::string& script,
    const json& fallback = json::object(),
    const char* diagnosticTag = nullptr
) {
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) return PowerShellDiagnosticFallback(fallback, diagnosticTag, "temp_path_failed");

    wchar_t tempFile[MAX_PATH]{};
    if (!GetTempFileNameW(tempPath, L"h5i", 0, tempFile)) return PowerShellDiagnosticFallback(fallback, diagnosticTag, "temp_file_failed");

    std::wstring scriptPath = tempFile;
    scriptPath += L".ps1";
    MoveFileW(tempFile, scriptPath.c_str());

    {
        std::ofstream f(WideToUtf8(scriptPath), std::ios::binary | std::ios::trunc);
        if (!f) return PowerShellDiagnosticFallback(fallback, diagnosticTag, "script_open_failed");
        f << "$ProgressPreference = 'SilentlyContinue'\n";
        f << "$ErrorActionPreference = 'SilentlyContinue'\n";
        f << "$WarningPreference = 'SilentlyContinue'\n";
        f << "$InformationPreference = 'SilentlyContinue'\n";
        f << "$VerbosePreference = 'SilentlyContinue'\n";
        f << script << "\n";
    }

    std::string cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + WideToUtf8(scriptPath) + "\"";
    std::string output;
    bool outputOverflow = false;
    int processStatus = -1;
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        DeleteFileW(scriptPath.c_str());
        return PowerShellDiagnosticFallback(fallback, diagnosticTag, "process_start_failed");
    }
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
        if (output.size() > 8 * 1024 * 1024) {
            outputOverflow = true;
            break;
        }
    }
#if defined(_WIN32)
    processStatus = _pclose(pipe);
#else
    processStatus = pclose(pipe);
#endif
    DeleteFileW(scriptPath.c_str());
    if (outputOverflow) {
        return PowerShellDiagnosticFallback(fallback, diagnosticTag, "output_overflow", output.size(), processStatus);
    }

    const std::size_t rawOutputBytes = output.size();
    constexpr const char* kJsonBegin = "__HI5_JSON_BEGIN__";
    constexpr const char* kJsonEnd = "__HI5_JSON_END__";
    const auto markedBegin = output.rfind(kJsonBegin);
    if (markedBegin != std::string::npos) {
        const auto jsonStart = markedBegin + std::char_traits<char>::length(kJsonBegin);
        const auto markedEnd = output.find(kJsonEnd, jsonStart);
        if (markedEnd == std::string::npos) {
            return PowerShellDiagnosticFallback(fallback, diagnosticTag, "missing_end_marker", rawOutputBytes, processStatus);
        }
        output = output.substr(jsonStart, markedEnd - jsonStart);
    } else {
        if (diagnosticTag && *diagnosticTag) {
            const std::string reason = output.empty() ? "empty_output" : "missing_begin_marker";
            return PowerShellDiagnosticFallback(fallback, diagnosticTag, reason, rawOutputBytes, processStatus);
        }
        const auto first = output.find_first_of("[{\"");
        if (first != std::string::npos) output = output.substr(first);
        const auto lastObj = output.find_last_of("]}");
        if (lastObj != std::string::npos) output = output.substr(0, lastObj + 1);
    }
    if (output.empty()) {
        return PowerShellDiagnosticFallback(fallback, diagnosticTag, "empty_json_payload", rawOutputBytes, processStatus);
    }

    try {
        return json::parse(output);
    } catch (...) {
        return PowerShellDiagnosticFallback(fallback, diagnosticTag, "json_parse_failed", rawOutputBytes, processStatus);
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
    json out = {
        {"name", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"ProcessorNameString")},
        {"vendor", RegString(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"VendorIdentifier")},
        {"logical_processors", static_cast<int>(si.dwNumberOfProcessors)},
        {"cores", static_cast<int>(si.dwNumberOfProcessors)},
        {"max_clock_mhz", RegDword(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", L"~MHz", 0)}
    };
    const json wmi = RunPowerShellJson(R"PS(
$cpus = @(Get-CimInstance Win32_Processor)
[pscustomobject]@{
  cores = [int](($cpus | Measure-Object -Property NumberOfCores -Sum).Sum)
  logical_processors = [int](($cpus | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum)
  max_clock_mhz = [int](($cpus | Measure-Object -Property MaxClockSpeed -Maximum).Maximum)
  sockets = @($cpus).Count
} | ConvertTo-Json -Compress
)PS", json::object());
    if (wmi.is_object()) {
        for (const auto& key : {"cores","logical_processors","max_clock_mhz","sockets"}) {
            if (wmi.contains(key) && !wmi[key].is_null()) out[key] = wmi[key];
        }
    }
    return out;
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
      recovery_protector_count = @($_.KeyProtector | Where-Object { [string]$_.KeyProtectorType -eq 'RecoveryPassword' }).Count
      recovery_password_present = @($_.KeyProtector | Where-Object { [string]$_.KeyProtectorType -eq 'RecoveryPassword' }).Count -gt 0
      key_protectors = @($_.KeyProtector | ForEach-Object {
        [pscustomobject]@{
          id = [string]$_.KeyProtectorId
          type = [string]$_.KeyProtectorType
        }
      })
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

        const std::uint64_t rawLinkSpeed = static_cast<std::uint64_t>(a->TransmitLinkSpeed);
        const std::uint64_t saneLinkSpeed = rawLinkSpeed > 1000000000000ULL ? 0ULL : rawLinkSpeed;
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
            {"speed_bps", saneLinkSpeed}
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

json LocalUsers() {
    return RunPowerShellJson(R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$admins = @{}
if (Get-Command Get-LocalGroupMember -ErrorAction SilentlyContinue) {
  Get-LocalGroupMember -Group 'Administrators' | ForEach-Object {
    $n = [string]$_.Name
    if ($n) {
      $admins[$n.ToLowerInvariant()] = $true
      $leaf = ($n -split '\\')[-1]
      if ($leaf) { $admins[$leaf.ToLowerInvariant()] = $true }
    }
  }
}
$users = @()
if (Get-Command Get-LocalUser -ErrorAction SilentlyContinue) {
  $users = @(Get-LocalUser | ForEach-Object {
    $name = [string]$_.Name
    $sid = ''
    try { $sid = [string]$_.SID.Value } catch { try { $sid = [string]$_.SID } catch {} }
    [pscustomobject]@{
      name = $name
      full_name = [string]$_.FullName
      enabled = [bool]$_.Enabled
      description = [string]$_.Description
      sid = $sid
      principal_source = [string]$_.PrincipalSource
      is_admin = [bool]($admins.ContainsKey($name.ToLowerInvariant()))
      last_logon = $(if ($_.LastLogon) { $_.LastLogon.ToUniversalTime().ToString('o') } else { $null })
      password_last_set = $(if ($_.PasswordLastSet) { $_.PasswordLastSet.ToUniversalTime().ToString('o') } else { $null })
      password_expires = $(if ($_.PasswordExpires) { $_.PasswordExpires.ToUniversalTime().ToString('o') } else { $null })
      account_expires = $(if ($_.AccountExpires) { $_.AccountExpires.ToUniversalTime().ToString('o') } else { $null })
      user_may_change_password = $(try { [bool]$_.UserMayChangePassword } catch { $null })
      password_required = $(try { [bool]$_.PasswordRequired } catch { $null })
    }
  })
} else {
  $users = @(Get-CimInstance Win32_UserAccount -Filter "LocalAccount=True" | ForEach-Object {
    [pscustomobject]@{
      name=[string]$_.Name; full_name=[string]$_.FullName; enabled=(-not [bool]$_.Disabled);
      description=[string]$_.Description; sid=[string]$_.SID; principal_source='Local';
      is_admin=[bool]($admins.ContainsKey(([string]$_.Name).ToLowerInvariant()));
      last_logon=$null; password_last_set=$null; password_expires=$null; account_expires=$null;
      user_may_change_password=$null; password_required=[bool]$_.PasswordRequired
    }
  })
}
[pscustomobject]@{ count=@($users).Count; users=@($users) } | ConvertTo-Json -Depth 6 -Compress
)PS", { {"count", 0}, {"users", json::array()} });
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

    json out = {
        {"defender_service_running", ServiceRunning(L"WinDefend")},
        {"defender_enabled", disableAntiSpyware == 0 && ServiceRunning(L"WinDefend")},
        {"defender_realtime_enabled", disableRealtime == 0},
        {"defender_real_time", disableRealtime == 0 ? "Enabled" : "Disabled"},
        {"firewall_enabled", ServiceRunning(L"MpsSvc")},
        {"firewall_service_running", ServiceRunning(L"MpsSvc")},
        {"mde_service_state", ServiceState(L"Sense")},
        {"mde_present", ServiceState(L"Sense") != "Not installed"},
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

    const json detailed = RunPowerShellJson(R"PS(
$mp = $null
if (Get-Command Get-MpComputerStatus -ErrorAction SilentlyContinue) {
  try { $mp = Get-MpComputerStatus } catch {}
}
$profiles = @()
if (Get-Command Get-NetFirewallProfile -ErrorAction SilentlyContinue) {
  try {
    $profiles = @(Get-NetFirewallProfile | ForEach-Object {
      [pscustomobject]@{
        name = [string]$_.Name
        enabled = [bool]$_.Enabled
        default_inbound_action = [string]$_.DefaultInboundAction
        default_outbound_action = [string]$_.DefaultOutboundAction
        notify_on_listen = [bool]$_.NotifyOnListen
        log_file = [string]$_.LogFileName
        log_allowed = [bool]$_.LogAllowed
        log_blocked = [bool]$_.LogBlocked
      }
    })
  } catch {}
}
[pscustomobject]@{
  defender = if ($mp) {
    [pscustomobject]@{
      antivirus_enabled = [bool]$mp.AntivirusEnabled
      antispyware_enabled = [bool]$mp.AntispywareEnabled
      realtime_protection_enabled = [bool]$mp.RealTimeProtectionEnabled
      behavior_monitor_enabled = [bool]$mp.BehaviorMonitorEnabled
      ioav_protection_enabled = [bool]$mp.IoavProtectionEnabled
      nis_enabled = [bool]$mp.NISEnabled
      tamper_protection_source = [string]$mp.TamperProtectionSource
      is_tamper_protected = if ($mp.IsTamperProtected -ne $null) { [bool]$mp.IsTamperProtected } else { $null }
      engine_version = [string]$mp.AMEngineVersion
      product_version = [string]$mp.AMProductVersion
      antivirus_signature_version = [string]$mp.AntivirusSignatureVersion
      antivirus_signature_last_updated = if ($mp.AntivirusSignatureLastUpdated) { $mp.AntivirusSignatureLastUpdated.ToUniversalTime().ToString('o') } else { $null }
      antispyware_signature_version = [string]$mp.AntispywareSignatureVersion
      nis_signature_version = [string]$mp.NISSignatureVersion
      quick_scan_age_days = $mp.QuickScanAge
      quick_scan_end = if ($mp.QuickScanEndTime) { $mp.QuickScanEndTime.ToUniversalTime().ToString('o') } else { $null }
      full_scan_age_days = $mp.FullScanAge
      full_scan_end = if ($mp.FullScanEndTime) { $mp.FullScanEndTime.ToUniversalTime().ToString('o') } else { $null }
      computer_state = [string]$mp.ComputerState
    }
  } else { $null }
  firewall_profiles = @($profiles)
} | ConvertTo-Json -Depth 6 -Compress
)PS", json::object());
    if (detailed.is_object()) {
        for (auto it = detailed.begin(); it != detailed.end(); ++it) out[it.key()] = it.value();
    }
    return out;
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
    auto enablePrivilege = [](const wchar_t* privilegeName) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
            CloseHandle(token);
            return false;
        }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        const DWORD err = GetLastError();
        CloseHandle(token);
        return ok && err == ERROR_SUCCESS;
    };

    auto collectRoot = [](HKEY root, const std::wstring& path, const std::string& scope,
        const std::string& userSid, const std::string& userProfile, REGSAM view, json& items) {
        HKEY rootKey = nullptr;
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | view, &rootKey) != ERROR_SUCCESS) return;

        for (DWORD index = 0;; ++index) {
            wchar_t subName[512]{};
            DWORD subNameLen = static_cast<DWORD>(sizeof(subName) / sizeof(subName[0]));
            const LONG rc = RegEnumKeyExW(rootKey, index, subName, &subNameLen, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;

            HKEY appKey = nullptr;
            if (RegOpenKeyExW(rootKey, subName, 0, KEY_READ | view, &appKey) != ERROR_SUCCESS) continue;
            const std::string name = RegStringFromOpenedKey(appKey, L"DisplayName");
            if (!name.empty() && RegDwordFromOpenedKey(appKey, L"SystemComponent", 0) == 0) {
                json item = {
                    {"name", name},
                    {"version", RegStringFromOpenedKey(appKey, L"DisplayVersion")},
                    {"publisher", RegStringFromOpenedKey(appKey, L"Publisher")},
                    {"install_date", RegStringFromOpenedKey(appKey, L"InstallDate")},
                    {"install_location", RegStringFromOpenedKey(appKey, L"InstallLocation")},
                    {"uninstall_string", RegStringFromOpenedKey(appKey, L"UninstallString")},
                    {"quiet_uninstall_string", RegStringFromOpenedKey(appKey, L"QuietUninstallString")},
                    {"estimated_size_kb", RegDwordFromOpenedKey(appKey, L"EstimatedSize", 0)},
                    {"scope", scope},
                    {"registry_key", WideToUtf8(std::wstring(subName, subNameLen))}
                };
                if (!userSid.empty()) item["user_sid"] = userSid;
                if (!userProfile.empty()) item["user_profile"] = userProfile;
                items.push_back(std::move(item));
            }
            RegCloseKey(appKey);
        }
        RegCloseKey(rootKey);
    };

    json items = json::array();
    collectRoot(HKEY_LOCAL_MACHINE,
        LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)",
        "machine64", "", "", KEY_WOW64_64KEY, items);
    collectRoot(HKEY_LOCAL_MACHINE,
        LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)",
        "machine32", "", "", KEY_WOW64_64KEY, items);

    enablePrivilege(L"SeBackupPrivilege");
    enablePrivilege(L"SeRestorePrivilege");

    HKEY profileList = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList)",
        0, KEY_READ | KEY_WOW64_64KEY, &profileList) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t sidName[256]{};
            DWORD sidLen = static_cast<DWORD>(sizeof(sidName) / sizeof(sidName[0]));
            const LONG rc = RegEnumKeyExW(profileList, index, sidName, &sidLen, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;

            const std::wstring sid(sidName, sidLen);
            if (sid.rfind(L"S-1-5-21-", 0) != 0 && sid.rfind(L"S-1-12-1-", 0) != 0) continue;

            HKEY profileKey = nullptr;
            if (RegOpenKeyExW(profileList, sid.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &profileKey) != ERROR_SUCCESS) continue;
            std::string profileUtf8 = RegStringFromOpenedKey(profileKey, L"ProfileImagePath");
            RegCloseKey(profileKey);

            std::wstring profilePath = Utf8ToWide(profileUtf8);
            if (!profilePath.empty()) {
                wchar_t expanded[32768]{};
                const DWORD n = ExpandEnvironmentStringsW(profilePath.c_str(), expanded,
                    static_cast<DWORD>(sizeof(expanded) / sizeof(expanded[0])));
                if (n > 0 && n < static_cast<DWORD>(sizeof(expanded) / sizeof(expanded[0]))) {
                    profilePath.assign(expanded);
                    profileUtf8 = WideToUtf8(profilePath);
                }
            }

            const std::string sidUtf8 = WideToUtf8(sid);
            const std::string scope = "user:" + sidUtf8;
            HKEY loadedHive = nullptr;
            if (RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, &loadedHive) == ERROR_SUCCESS) {
                RegCloseKey(loadedHive);
                collectRoot(HKEY_USERS, sid + LR"(\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)",
                    scope, sidUtf8, profileUtf8, 0, items);
                continue;
            }

            if (profilePath.empty()) continue;
            std::wstring ntUser = profilePath;
            if (!ntUser.empty() && ntUser.back() != L'\\' && ntUser.back() != L'/') ntUser += L"\\";
            ntUser += L"NTUSER.DAT";
            if (GetFileAttributesW(ntUser.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

            const std::wstring mountName = L"Hi5CentralTemp_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(index);
            if (RegLoadKeyW(HKEY_USERS, mountName.c_str(), ntUser.c_str()) == ERROR_SUCCESS) {
                collectRoot(HKEY_USERS, mountName + LR"(\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)",
                    scope, sidUtf8, profileUtf8, 0, items);
                RegUnLoadKeyW(HKEY_USERS, mountName.c_str());
            }
        }
        RegCloseKey(profileList);
    }

    std::sort(items.begin(), items.end(), [](const json& a, const json& b) {
        const std::string an = a.value("name", "");
        const std::string bn = b.value("name", "");
        if (an != bn) return an < bn;
        return a.value("scope", "") < b.value("scope", "");
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

json DeepInventoryInfo() {
    static std::mutex cacheMutex;
    static json cached = json::object();
    static std::chrono::steady_clock::time_point cachedAt{};
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (!cached.empty() && cachedAt.time_since_epoch().count() != 0 &&
            now - cachedAt < std::chrono::minutes(15)) return cached;
    }

    json combined = {{"collected_at", NowIsoUtc()}, {"section_status", json::object()}};
    int successfulSections = 0;
    int failedSections = 0;
    auto mergeSection = [&](const char* name, const json& section) {
        const bool valid = section.is_object() && section.contains("collected_at");
        const std::string error = section.is_object() ? section.value("collector_error", std::string()) : std::string();
        json status = {{"status", valid && error.empty() ? "collected" : "failed"}};
        if (section.is_object() && section.contains("collected_at")) status["collected_at"] = section["collected_at"];
        if (!error.empty()) status["error"] = error.substr(0, 320);
        if (section.is_object() && section.contains("collector_output_bytes")) status["output_bytes"] = section["collector_output_bytes"];
        if (section.is_object() && section.contains("collector_process_status")) status["process_status"] = section["collector_process_status"];
        combined["section_status"][name] = status;
        if (!valid || !error.empty()) { ++failedSections; return; }
        ++successfulSections;
        for (auto it = section.begin(); it != section.end(); ++it) {
            if (it.key() == "collected_at" || it.key() == "collector_error" ||
                it.key() == "collector_output_bytes" || it.key() == "collector_process_status") continue;
            combined[it.key()] = it.value();
        }
    };

    mergeSection("core_hardware", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$memoryModules = @(Get-CimInstance Win32_PhysicalMemory | ForEach-Object {
  [pscustomobject]@{
    bank_label = $_.BankLabel
    device_locator = $_.DeviceLocator
    capacity_bytes = [uint64]$_.Capacity
    manufacturer = (Text $_.Manufacturer).Trim()
    part_number = (Text $_.PartNumber).Trim()
    serial_number = (Text $_.SerialNumber).Trim()
    speed_mhz = [int]$_.Speed
    configured_speed_mhz = [int]$_.ConfiguredClockSpeed
    form_factor = [int]$_.FormFactor
    memory_type = [int]$_.SMBIOSMemoryType
  }
})

$board = Get-CimInstance Win32_BaseBoard | Select-Object -First 1
$bios = Get-CimInstance Win32_BIOS | Select-Object -First 1
$motherboard = [pscustomobject]@{
  manufacturer = Text $board.Manufacturer
  product = Text $board.Product
  serial_number = Text $board.SerialNumber
  version = Text $board.Version
  bios_manufacturer = Text $bios.Manufacturer
  bios_version = Text $bios.SMBIOSBIOSVersion
  bios_serial_number = Text $bios.SerialNumber
  smbios_version = if ($bios.SMBIOSMajorVersion -ne $null) { ([string]$bios.SMBIOSMajorVersion + '.' + [string]$bios.SMBIOSMinorVersion) } else { '' }
}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  memory_modules = @($memoryModules)
  motherboard = $motherboard
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_core_hardware"));

    mergeSection("storage", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$physicalDisks = @()
if (Get-Command Get-PhysicalDisk -ErrorAction SilentlyContinue) {
  $physicalDisks = @(Get-PhysicalDisk | ForEach-Object {
    $pd = $_
    $rel = $null
    try { $rel = $pd | Get-StorageReliabilityCounter -ErrorAction Stop } catch {}
    [pscustomobject]@{
      friendly_name = Text $pd.FriendlyName
      serial_number = (Text $pd.SerialNumber).Trim()
      unique_id = Text $pd.UniqueId
      device_id = Text $pd.DeviceId
      media_type = Text $pd.MediaType
      bus_type = Text $pd.BusType
      health_status = Text $pd.HealthStatus
      operational_status = (@($pd.OperationalStatus) -join ', ')
      size_bytes = [uint64]$pd.Size
      firmware_version = Text $pd.FirmwareVersion
      can_pool = [bool]$pd.CanPool
      temperature_c = if ($rel -and $rel.Temperature -ne $null) { [int]$rel.Temperature } else { $null }
      wear_percent = if ($rel -and $rel.Wear -ne $null) { [int]$rel.Wear } else { $null }
      power_on_hours = if ($rel -and $rel.PowerOnHours -ne $null) { [uint64]$rel.PowerOnHours } else { $null }
      read_errors_total = if ($rel -and $rel.ReadErrorsTotal -ne $null) { [uint64]$rel.ReadErrorsTotal } else { $null }
      write_errors_total = if ($rel -and $rel.WriteErrorsTotal -ne $null) { [uint64]$rel.WriteErrorsTotal } else { $null }
    }
  })
}
if (-not $physicalDisks.Count) {
  $physicalDisks = @(Get-CimInstance Win32_DiskDrive | ForEach-Object {
    [pscustomobject]@{
      friendly_name = Text $_.Model
      serial_number = (Text $_.SerialNumber).Trim()
      unique_id = Text $_.PNPDeviceID
      device_id = Text $_.DeviceID
      media_type = Text $_.MediaType
      bus_type = Text $_.InterfaceType
      health_status = Text $_.Status
      operational_status = Text $_.Status
      size_bytes = [uint64]$_.Size
      firmware_version = Text $_.FirmwareRevision
      can_pool = $false
      temperature_c = $null
      wear_percent = $null
      power_on_hours = $null
      read_errors_total = $null
      write_errors_total = $null
    }
  })
}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  physical_disks = @($physicalDisks)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_storage"));

    mergeSection("monitors", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$monitors = @()
try {
  $monitors = @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction Stop | ForEach-Object {
    [pscustomobject]@{
      instance_name = Text $_.InstanceName
      manufacturer = WmiChars $_.ManufacturerName
      model = WmiChars $_.UserFriendlyName
      serial_number = WmiChars $_.SerialNumberID
      product_code = WmiChars $_.ProductCodeID
      active = [bool]$_.Active
      manufacture_week = [int]$_.WeekOfManufacture
      manufacture_year = [int]$_.YearOfManufacture
    }
  })
} catch {}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  monitors = @($monitors)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_monitors"));

    mergeSection("drivers", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$drivers = @(Get-CimInstance Win32_PnPSignedDriver |
  Where-Object { $_.DeviceName } |
  Sort-Object DeviceClass,DeviceName |
  Select-Object -First 600 |
  ForEach-Object {
    [pscustomobject]@{
      device_name = Text $_.DeviceName
      device_class = Text $_.DeviceClass
      manufacturer = Text $_.Manufacturer
      driver_provider = Text $_.DriverProviderName
      driver_version = Text $_.DriverVersion
      driver_date = if ($_.DriverDate) { $_.DriverDate.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
      inf_name = Text $_.InfName
      pnp_device_id = Text $_.DeviceID
      signed = [bool]$_.IsSigned
      signer = Text $_.Signer
    }
  })

$problemDevices = @(Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.ConfigManagerErrorCode -ne $null -and [int]$_.ConfigManagerErrorCode -ne 0 } |
  Select-Object -First 200 |
  ForEach-Object {
    [pscustomobject]@{
      name = Text $_.Name
      device_id = Text $_.DeviceID
      pnp_class = Text $_.PNPClass
      status = Text $_.Status
      error_code = [int]$_.ConfigManagerErrorCode
      manufacturer = Text $_.Manufacturer
    }
  })
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  drivers = @($drivers)
  problem_devices = @($problemDevices)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_drivers"));

    mergeSection("windows_state", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$hotfixes = @(Get-HotFix |
  Sort-Object InstalledOn -Descending |
  Select-Object -First 200 |
  ForEach-Object {
    [pscustomobject]@{
      hotfix_id = Text $_.HotFixID
      description = Text $_.Description
      installed_by = Text $_.InstalledBy
      installed_on = if ($_.InstalledOn) { $_.InstalledOn.ToString('yyyy-MM-dd') } else { $null }
    }
  })

$licenseProduct = Get-CimInstance SoftwareLicensingProduct |
  Where-Object { $_.PartialProductKey -and $_.Name -like 'Windows*' } |
  Sort-Object LicenseStatus -Descending |
  Select-Object -First 1
$licenseMap = @{0='Unlicensed';1='Licensed';2='OOB grace';3='OOT grace';4='Non-genuine grace';5='Notification';6='Extended grace'}
$licensing = [pscustomobject]@{
  status_code = if ($licenseProduct) { [int]$licenseProduct.LicenseStatus } else { $null }
  status = if ($licenseProduct) { $licenseMap[[int]$licenseProduct.LicenseStatus] } else { 'Not reported' }
  name = if ($licenseProduct) { Text $licenseProduct.Name } else { '' }
  description = if ($licenseProduct) { Text $licenseProduct.Description } else { '' }
  partial_product_key = if ($licenseProduct) { Text $licenseProduct.PartialProductKey } else { '' }
}

$rebootReasons = @()
if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') { $rebootReasons += 'Component Based Servicing' }
if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') { $rebootReasons += 'Windows Update' }
try {
  $rename = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction Stop).PendingFileRenameOperations
  if ($rename) { $rebootReasons += 'Pending file rename operations' }
} catch {}
try {
  $active = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\ComputerName\ActiveComputerName' -Name ComputerName -ErrorAction Stop).ComputerName
  $pending = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\ComputerName\ComputerName' -Name ComputerName -ErrorAction Stop).ComputerName
  if ($active -and $pending -and $active -ne $pending) { $rebootReasons += 'Pending computer rename' }
} catch {}
$rebootState = [pscustomobject]@{ pending = [bool]$rebootReasons.Count; reasons = @($rebootReasons) }

$startupItems = @(Get-CimInstance Win32_StartupCommand |
  Sort-Object Name |
  Select-Object -First 300 |
  ForEach-Object {
    [pscustomobject]@{
      name = Text $_.Name
      command = Text $_.Command
      location = Text $_.Location
      user = Text $_.User
    }
  })
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  installed_hotfixes = @($hotfixes)
  windows_licensing = $licensing
  reboot_state = $rebootState
  startup_items = @($startupItems)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_windows_state"));

    mergeSection("scheduled_tasks", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$scheduledTasks = @()
if (Get-Command Get-ScheduledTask -ErrorAction SilentlyContinue) {
  $scheduledTasks = @(Get-ScheduledTask |
    Sort-Object TaskPath,TaskName |
    Select-Object -First 500 |
    ForEach-Object {
      [pscustomobject]@{
        name = Text $_.TaskName
        path = Text $_.TaskPath
        state = Text $_.State
        author = Text $_.Author
        description = Text $_.Description
      }
    })
}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  scheduled_tasks = @($scheduledTasks)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_scheduled_tasks"));

    mergeSection("local_groups", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$localGroups = @()
if (Get-Command Get-LocalGroup -ErrorAction SilentlyContinue) {
  $localGroups = @(Get-LocalGroup | Sort-Object Name | ForEach-Object {
    $group = $_
    $members = @()
    try {
      $members = @(Get-LocalGroupMember -Group $group.Name -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{ name = Text $_.Name; object_class = Text $_.ObjectClass; principal_source = Text $_.PrincipalSource; sid = Text $_.SID }
      })
    } catch {}
    [pscustomobject]@{
      name = Text $group.Name
      description = Text $group.Description
      sid = Text $group.SID
      members = @($members)
    }
  })
}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  local_groups = @($localGroups)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_local_groups"));

    mergeSection("peripherals", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$printers = @(Get-CimInstance Win32_Printer | Sort-Object Name | Select-Object -First 150 | ForEach-Object {
  [pscustomobject]@{
    name = Text $_.Name
    driver_name = Text $_.DriverName
    port_name = Text $_.PortName
    network = [bool]$_.Network
    shared = [bool]$_.Shared
    default = [bool]$_.Default
    status = Text $_.PrinterStatus
  }
})

$usbDevices = @(Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.PNPDeviceID -like 'USB*' -and $_.Name } |
  Sort-Object Name |
  Select-Object -First 250 |
  ForEach-Object {
    [pscustomobject]@{
      name = Text $_.Name
      device_id = Text $_.DeviceID
      pnp_class = Text $_.PNPClass
      manufacturer = Text $_.Manufacturer
      status = Text $_.Status
      service = Text $_.Service
    }
  })
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  printers = @($printers)
  usb_devices = @($usbDevices)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_peripherals"));

    mergeSection("features_power", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$optionalFeatures = @()
if (Get-Command Get-WindowsOptionalFeature -ErrorAction SilentlyContinue) {
  try {
    $optionalFeatures = @(Get-WindowsOptionalFeature -Online -ErrorAction Stop |
      Where-Object { [string]$_.State -eq 'Enabled' } |
      Select-Object -First 300 |
      ForEach-Object { [pscustomobject]@{ name = Text $_.FeatureName; state = Text $_.State } })
  } catch {}
}

$powerPlan = ''
try {
  $powerLine = (& powercfg.exe /getactivescheme 2>$null | Select-Object -First 1)
  if ($powerLine -match '\((.+)\)') { $powerPlan = $Matches[1] } else { $powerPlan = Text $powerLine }
} catch {}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  optional_features = @($optionalFeatures)
  power_plan = $powerPlan
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_features_power"));

    mergeSection("network", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$networkProfiles = @()
if (Get-Command Get-NetConnectionProfile -ErrorAction SilentlyContinue) {
  $networkProfiles = @(Get-NetConnectionProfile | ForEach-Object {
    [pscustomobject]@{
      name = Text $_.Name
      interface_alias = Text $_.InterfaceAlias
      interface_index = [int]$_.InterfaceIndex
      network_category = Text $_.NetworkCategory
      ipv4_connectivity = Text $_.IPv4Connectivity
      ipv6_connectivity = Text $_.IPv6Connectivity
    }
  })
}

$networkConfigurations = @(Get-CimInstance Win32_NetworkAdapterConfiguration |
  Where-Object { $_.IPEnabled } |
  ForEach-Object {
    [pscustomobject]@{
      description = Text $_.Description
      setting_id = Text $_.SettingID
      interface_index = [int]$_.InterfaceIndex
      dhcp_enabled = [bool]$_.DHCPEnabled
      dhcp_server = Text $_.DHCPServer
      dhcp_lease_obtained = if ($_.DHCPLeaseObtained) { $_.DHCPLeaseObtained.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
      dhcp_lease_expires = if ($_.DHCPLeaseExpires) { $_.DHCPLeaseExpires.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
      dns_host_name = Text $_.DNSHostName
      dns_domain = Text $_.DNSDomain
      dns_servers = @($_.DNSServerSearchOrder)
      ip_addresses = @($_.IPAddress)
      subnets = @($_.IPSubnet)
      gateways = @($_.DefaultIPGateway)
      wins_primary = Text $_.WINSPrimaryServer
      wins_secondary = Text $_.WINSSecondaryServer
      mac_address = Text $_.MACAddress
    }
  })

$wifiInterfaces = @()
try {
  $rawWifi = @(& netsh.exe wlan show interfaces 2>$null)
  $current = $null
  foreach ($line in $rawWifi) {
    if ($line -match '^\s*Name\s*:\s*(.+)$') {
      if ($current) { $wifiInterfaces += [pscustomobject]$current }
      $current = [ordered]@{ name=$Matches[1].Trim(); state=''; ssid=''; bssid=''; signal=''; channel=''; radio_type=''; authentication=''; cipher=''; receive_rate_mbps=''; transmit_rate_mbps='' }
      continue
    }
    if (-not $current) { continue }
    if ($line -match '^\s*State\s*:\s*(.+)$') { $current.state = $Matches[1].Trim(); continue }
    if ($line -match '^\s*SSID\s*:\s*(.+)$' -and $line -notmatch 'BSSID') { $current.ssid = $Matches[1].Trim(); continue }
    if ($line -match '^\s*BSSID\s*:\s*(.+)$') { $current.bssid = $Matches[1].Trim(); continue }
    if ($line -match '^\s*Signal\s*:\s*(.+)$') { $current.signal = $Matches[1].Trim(); continue }
    if ($line -match '^\s*Channel\s*:\s*(.+)$') { $current.channel = $Matches[1].Trim(); continue }
    if ($line -match '^\s*Radio type\s*:\s*(.+)$') { $current.radio_type = $Matches[1].Trim(); continue }
    if ($line -match '^\s*Authentication\s*:\s*(.+)$') { $current.authentication = $Matches[1].Trim(); continue }
    if ($line -match '^\s*Cipher\s*:\s*(.+)$') { $current.cipher = $Matches[1].Trim(); continue }
    if ($line -match '^\s*Receive rate \(Mbps\)\s*:\s*(.+)$') { $current.receive_rate_mbps = $Matches[1].Trim(); continue }
    if ($line -match '^\s*Transmit rate \(Mbps\)\s*:\s*(.+)$') { $current.transmit_rate_mbps = $Matches[1].Trim(); continue }
  }
  if ($current) { $wifiInterfaces += [pscustomobject]$current }
} catch {}

$defaultRoutes = @()
if (Get-Command Get-NetRoute -ErrorAction SilentlyContinue) {
  $defaultRoutes = @(Get-NetRoute -ErrorAction SilentlyContinue |
    Where-Object { $_.DestinationPrefix -in @('0.0.0.0/0','::/0') } |
    Sort-Object RouteMetric,InterfaceMetric |
    Select-Object -First 20 |
    ForEach-Object {
      [pscustomobject]@{
        destination = Text $_.DestinationPrefix
        next_hop = Text $_.NextHop
        interface_alias = Text $_.InterfaceAlias
        interface_index = [int]$_.InterfaceIndex
        route_metric = [int]$_.RouteMetric
        protocol = Text $_.Protocol
        address_family = Text $_.AddressFamily
      }
    })
}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  network_profiles = @($networkProfiles)
  network_configurations = @($networkConfigurations)
  wifi_interfaces = @($wifiInterfaces)
  default_routes = @($defaultRoutes)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_network"));

    mergeSection("directory_join", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$directoryJoin = [pscustomobject]@{}
try {
  $csJoin = Get-CimInstance Win32_ComputerSystem | Select-Object -First 1
  $azureAdJoined = $null
  $domainJoined = [bool]$csJoin.PartOfDomain
  $workplaceJoined = $null
  $deviceId = ''
  $tenantId = ''
  $tenantName = ''
  try {
    $dsreg = @(& dsregcmd.exe /status 2>$null)
    foreach ($line in $dsreg) {
      if ($line -match '^\s*AzureAdJoined\s*:\s*(YES|NO)') { $azureAdJoined = $Matches[1] -eq 'YES'; continue }
      if ($line -match '^\s*DomainJoined\s*:\s*(YES|NO)') { $domainJoined = $Matches[1] -eq 'YES'; continue }
      if ($line -match '^\s*WorkplaceJoined\s*:\s*(YES|NO)') { $workplaceJoined = $Matches[1] -eq 'YES'; continue }
      if ($line -match '^\s*DeviceId\s*:\s*(.+)$') { $deviceId = $Matches[1].Trim(); continue }
      if ($line -match '^\s*TenantId\s*:\s*(.+)$') { $tenantId = $Matches[1].Trim(); continue }
      if ($line -match '^\s*TenantName\s*:\s*(.+)$') { $tenantName = $Matches[1].Trim(); continue }
    }
  } catch {}
  $roleMap = @{0='Standalone workstation';1='Member workstation';2='Standalone server';3='Member server';4='Backup domain controller';5='Primary domain controller'}
  $directoryJoin = [pscustomobject]@{
    computer_name = Text $csJoin.Name
    part_of_domain = [bool]$csJoin.PartOfDomain
    domain = Text $csJoin.Domain
    workgroup = Text $csJoin.Workgroup
    domain_role_code = [int]$csJoin.DomainRole
    domain_role = $roleMap[[int]$csJoin.DomainRole]
    azure_ad_joined = $azureAdJoined
    domain_joined = $domainJoined
    workplace_joined = $workplaceJoined
    entra_device_id = $deviceId
    entra_tenant_id = $tenantId
    entra_tenant_name = $tenantName
  }
} catch {}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  directory_join = $directoryJoin
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_directory_join"));

    mergeSection("certificates", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$machineCertificates = @()
foreach ($storePath in @('Cert:\LocalMachine\My','Cert:\LocalMachine\WebHosting')) {
  if (-not (Test-Path $storePath)) { continue }
  try {
    $storeName = ($storePath -split '\\')[-1]
    $machineCertificates += @(Get-ChildItem $storePath -ErrorAction Stop | Select-Object -First 250 | ForEach-Object {
      [pscustomobject]@{
        store = $storeName
        subject = Text $_.Subject
        issuer = Text $_.Issuer
        thumbprint = Text $_.Thumbprint
        serial_number = Text $_.SerialNumber
        not_before = if ($_.NotBefore) { $_.NotBefore.ToUniversalTime().ToString('o') } else { $null }
        not_after = if ($_.NotAfter) { $_.NotAfter.ToUniversalTime().ToString('o') } else { $null }
        has_private_key = [bool]$_.HasPrivateKey
        friendly_name = Text $_.FriendlyName
        signature_algorithm = if ($_.SignatureAlgorithm) { Text $_.SignatureAlgorithm.FriendlyName } else { '' }
        public_key_algorithm = if ($_.PublicKey -and $_.PublicKey.Oid) { Text $_.PublicKey.Oid.FriendlyName } else { '' }
        enhanced_key_usage = @($_.EnhancedKeyUsageList | ForEach-Object { Text $_.FriendlyName })
      }
    })
  } catch {}
}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  machine_certificates = @($machineCertificates)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_certificates"));

    mergeSection("virtualization", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$virtualization = [pscustomobject]@{}
try {
  $csVirtual = Get-CimInstance Win32_ComputerSystem | Select-Object -First 1
  $cpuVirtual = Get-CimInstance Win32_Processor | Select-Object -First 1
  $virtualization = [pscustomobject]@{
    hypervisor_present = if ($csVirtual.HypervisorPresent -ne $null) { [bool]$csVirtual.HypervisorPresent } else { $null }
    vm_monitor_mode_extensions = if ($cpuVirtual.VMMonitorModeExtensions -ne $null) { [bool]$cpuVirtual.VMMonitorModeExtensions } else { $null }
    virtualization_firmware_enabled = if ($cpuVirtual.VirtualizationFirmwareEnabled -ne $null) { [bool]$cpuVirtual.VirtualizationFirmwareEnabled } else { $null }
    second_level_address_translation = if ($cpuVirtual.SecondLevelAddressTranslationExtensions -ne $null) { [bool]$cpuVirtual.SecondLevelAddressTranslationExtensions } else { $null }
    data_execution_prevention_available = if ($cpuVirtual.DataExecutionPrevention_Available -ne $null) { [bool]$cpuVirtual.DataExecutionPrevention_Available } else { $null }
  }
} catch {}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  virtualization = $virtualization
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_virtualization"));

    mergeSection("security", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$defender = [pscustomobject]@{}
if (Get-Command Get-MpComputerStatus -ErrorAction SilentlyContinue) {
  try {
    $mp = Get-MpComputerStatus -ErrorAction Stop
    $defender = [pscustomobject]@{
      antivirus_enabled = [bool]$mp.AntivirusEnabled
      antispyware_enabled = [bool]$mp.AntispywareEnabled
      realtime_protection_enabled = [bool]$mp.RealTimeProtectionEnabled
      behavior_monitor_enabled = [bool]$mp.BehaviorMonitorEnabled
      ioav_protection_enabled = [bool]$mp.IoavProtectionEnabled
      nis_enabled = [bool]$mp.NISEnabled
      antivirus_signature_version = Text $mp.AntivirusSignatureVersion
      antivirus_signature_last_updated = if ($mp.AntivirusSignatureLastUpdated) { $mp.AntivirusSignatureLastUpdated.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
      antispyware_signature_version = Text $mp.AntispywareSignatureVersion
      engine_version = Text $mp.AMEngineVersion
      product_version = Text $mp.AMProductVersion
      is_tamper_protected = if ($mp.IsTamperProtected -ne $null) { [bool]$mp.IsTamperProtected } else { $null }
      tamper_protection_source = Text $mp.TamperProtectionSource
      quick_scan_age_days = if ($mp.QuickScanAge -ne $null) { [int]$mp.QuickScanAge } else { $null }
      quick_scan_end = if ($mp.QuickScanEndTime) { $mp.QuickScanEndTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
      full_scan_age_days = if ($mp.FullScanAge -ne $null) { [int]$mp.FullScanAge } else { $null }
      full_scan_end = if ($mp.FullScanEndTime) { $mp.FullScanEndTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { $null }
      reboot_required = if ($mp.RebootRequired -ne $null) { [bool]$mp.RebootRequired } else { $null }
      computer_state = Text $mp.ComputerState
    }
  } catch {}
}

$firewallProfiles = @()
if (Get-Command Get-NetFirewallProfile -ErrorAction SilentlyContinue) {
  try {
    $firewallProfiles = @(Get-NetFirewallProfile -ErrorAction Stop | ForEach-Object {
      [pscustomobject]@{
        name = Text $_.Name
        enabled = [bool]$_.Enabled
        default_inbound_action = Text $_.DefaultInboundAction
        default_outbound_action = Text $_.DefaultOutboundAction
        notify_on_listen = [bool]$_.NotifyOnListen
        allow_inbound_rules = Text $_.AllowInboundRules
        allow_local_firewall_rules = Text $_.AllowLocalFirewallRules
        log_allowed = [bool]$_.LogAllowed
        log_blocked = [bool]$_.LogBlocked
        log_file_name = Text $_.LogFileName
      }
    })
  } catch {}
}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  defender = $defender
  firewall_profiles = @($firewallProfiles)
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_security"));

    mergeSection("battery", RunPowerShellJson(R"PS(
function Text($v) { if ($null -eq $v) { return '' }; return [string]$v }
function WmiChars($v) {
  if ($null -eq $v) { return '' }
  return (-join @($v | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })).Trim()
}

$collectorError = ''
try {
$battery = [pscustomobject]@{}
try {
  $wb = Get-CimInstance Win32_Battery | Select-Object -First 1
  $bs = Get-CimInstance -Namespace root\wmi -ClassName BatteryStaticData -ErrorAction SilentlyContinue | Select-Object -First 1
  $bf = Get-CimInstance -Namespace root\wmi -ClassName BatteryFullChargedCapacity -ErrorAction SilentlyContinue | Select-Object -First 1
  $bc = Get-CimInstance -Namespace root\wmi -ClassName BatteryCycleCount -ErrorAction SilentlyContinue | Select-Object -First 1
  $bst = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue | Select-Object -First 1
  $design = if ($bs -and $bs.DesignedCapacity) { [uint64]$bs.DesignedCapacity } elseif ($wb -and $wb.DesignCapacity) { [uint64]$wb.DesignCapacity } else { $null }
  $full = if ($bf -and $bf.FullChargedCapacity) { [uint64]$bf.FullChargedCapacity } elseif ($wb -and $wb.FullChargeCapacity) { [uint64]$wb.FullChargeCapacity } else { $null }
  $healthPct = if ($design -and $full -and $design -gt 0) { [math]::Round(100.0 * $full / $design, 1) } else { $null }
  $battery = [pscustomobject]@{
    name = if ($wb) { Text $wb.Name } else { '' }
    manufacturer = if ($wb) { Text $wb.Manufacturer } else { '' }
    chemistry = if ($wb) { Text $wb.Chemistry } else { '' }
    design_capacity_mwh = $design
    full_charge_capacity_mwh = $full
    health_percent = $healthPct
    wear_percent = if ($healthPct -ne $null) { [math]::Round([math]::Max(0,100-$healthPct),1) } else { $null }
    cycle_count = if ($bc -and $bc.CycleCount -ne $null) { [int]$bc.CycleCount } else { $null }
    voltage_mv = if ($bst -and $bst.Voltage -ne $null) { [int]$bst.Voltage } else { $null }
    rate_mw = if ($bst -and $bst.Rate -ne $null) { [int]$bst.Rate } else { $null }
    remaining_capacity_mwh = if ($bst -and $bst.RemainingCapacity -ne $null) { [uint64]$bst.RemainingCapacity } else { $null }
    power_online = if ($bst -and $bst.PowerOnline -ne $null) { [bool]$bst.PowerOnline } else { $null }
    discharging = if ($bst -and $bst.Discharging -ne $null) { [bool]$bst.Discharging } else { $null }
    charging = if ($bst -and $bst.Charging -ne $null) { [bool]$bst.Charging } else { $null }
  }
} catch {}
} catch {
  $collectorError = [string]$_.Exception.Message
}
$result = [pscustomobject]@{
  collected_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  collector_error = $collectorError
  battery = $battery
}
$json = $result | ConvertTo-Json -Depth 9 -Compress
Write-Output ('__HI5_JSON_BEGIN__' + $json + '__HI5_JSON_END__')
)PS", json::object(), "deep_battery"));

    combined["successful_section_count"] = successfulSections;
    combined["failed_section_count"] = failedSections;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cached = combined;
        cachedAt = now;
    }
    return combined;
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
        {"version", hi5::kAgentVersion},
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

json BuildBitLockerRecoveryEscrow(const AgentIdentity& identity) {
    json entries = RunPowerShellJson(R"PS(
$items = @()
if (Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue) {
  foreach ($volume in @(Get-BitLockerVolume)) {
    foreach ($protector in @($volume.KeyProtector)) {
      if ([string]$protector.KeyProtectorType -ne 'RecoveryPassword') { continue }
      $password = [string]$protector.RecoveryPassword
      if ([string]::IsNullOrWhiteSpace($password)) { continue }
      $items += [pscustomobject]@{
        drive = [string]$volume.MountPoint
        protector_id = [string]$protector.KeyProtectorId
        protector_type = [string]$protector.KeyProtectorType
        recovery_password = $password
      }
    }
  }
}
@($items) | ConvertTo-Json -Depth 5 -Compress
)PS", json::array());
    if (entries.is_object()) entries = json::array({entries});
    if (!entries.is_array()) entries = json::array();
    return {
        {"type", "bitlocker_recovery_escrow"},
        {"device_id", identity.deviceId},
        {"collected_at", NowIsoUtc()},
        {"entries", entries}
    };
}

json BuildInventorySnapshot(const AgentIdentity& identity, bool includeDeepInventory) {
    const auto collectedAt = NowIsoUtc();

    json bitlocker = BitLockerVolumes();
    json tpm = TpmInfo();
    json localAdmins = LocalAdmins();
    json localUsers = LocalUsers();
    json os = OsInfo();
    json hardware = HardwareInfo();
    json cpu = CpuInfo();
    json memory = MemoryInfo();
    json storage = StorageInfo(bitlocker);
    json security = SecurityInfo(bitlocker, tpm, localAdmins);
    json network = NetworkInfo();
    json deep = includeDeepInventory ? DeepInventoryInfo() : json::object();
    if (deep.contains("network_configurations")) network["configurations"] = deep["network_configurations"];
    if (deep.contains("wifi_interfaces")) network["wifi_interfaces"] = deep["wifi_interfaces"];
    if (deep.contains("default_routes")) network["default_routes"] = deep["default_routes"];
    if (deep.contains("defender")) security["defender"] = deep["defender"];
    if (deep.contains("firewall_profiles")) security["firewall_profiles"] = deep["firewall_profiles"];
    json battery = BatteryInfo();
    if (deep.contains("battery") && deep["battery"].is_object()) {
        for (auto it = deep["battery"].begin(); it != deep["battery"].end(); ++it) {
            if (!it.value().is_null() && !(it.value().is_string() && it.value().get<std::string>().empty())) {
                battery[it.key()] = it.value();
            }
        }
    }
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
        {"local_users", localUsers},
        {"displays", displays},
        {"gpu", gpu},
        {"battery", battery},
        {"memory_modules", deep.value("memory_modules", json::array())},
        {"motherboard", deep.value("motherboard", json::object())},
        {"physical_disks", deep.value("physical_disks", json::array())},
        {"monitors", deep.value("monitors", json::array())},
        {"drivers", deep.value("drivers", json::array())},
        {"problem_devices", deep.value("problem_devices", json::array())},
        {"installed_hotfixes", deep.value("installed_hotfixes", json::array())},
        {"windows_licensing", deep.value("windows_licensing", json::object())},
        {"reboot_state", deep.value("reboot_state", json::object())},
        {"startup_items", deep.value("startup_items", json::array())},
        {"scheduled_tasks", deep.value("scheduled_tasks", json::array())},
        {"local_groups", deep.value("local_groups", json::array())},
        {"printers", deep.value("printers", json::array())},
        {"usb_devices", deep.value("usb_devices", json::array())},
        {"optional_features", deep.value("optional_features", json::array())},
        {"power_plan", deep.value("power_plan", "")},
        {"network_profiles", deep.value("network_profiles", json::array())},
        {"network_configurations", deep.value("network_configurations", json::array())},
        {"wifi_interfaces", deep.value("wifi_interfaces", json::array())},
        {"default_routes", deep.value("default_routes", json::array())},
        {"directory_join", deep.value("directory_join", json::object())},
        {"machine_certificates", deep.value("machine_certificates", json::array())},
        {"virtualization", deep.value("virtualization", json::object())},
        {"deep_inventory_collected_at", deep.value("collected_at", "")},
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
    const int successfulDeepSections = includeDeepInventory && deep.is_object()
        ? deep.value("successful_section_count", 0) : 0;
    const int failedDeepSections = includeDeepInventory && deep.is_object()
        ? deep.value("failed_section_count", 0) : 0;
    const bool deepInventoryAvailable = successfulDeepSections > 0;
    const bool deepInventoryPartial = successfulDeepSections > 0 && failedDeepSections > 0;
    snapshot["deep_inventory_included"] = deepInventoryAvailable;
    snapshot["deep_inventory_mode"] = includeDeepInventory ? "sectioned" : "core";
    snapshot["deep_inventory_successful_sections"] = successfulDeepSections;
    snapshot["deep_inventory_failed_sections"] = failedDeepSections;
    if (deep.is_object() && deep.contains("section_status")) {
        snapshot["deep_inventory_sections"] = deep["section_status"];
    }
    if (!includeDeepInventory) snapshot["deep_inventory_status"] = "not_requested";
    else if (!deepInventoryAvailable) snapshot["deep_inventory_status"] = "failed";
    else if (deepInventoryPartial) snapshot["deep_inventory_status"] = "partial";
    else snapshot["deep_inventory_status"] = "included";
    if (!deepInventoryAvailable) {
        for (const auto* key : {
            "memory_modules","motherboard","physical_disks","monitors","drivers","problem_devices",
            "installed_hotfixes","windows_licensing","reboot_state","startup_items","scheduled_tasks",
            "local_groups","printers","usb_devices","optional_features","power_plan","network_profiles",
            "network_configurations","wifi_interfaces","default_routes","directory_join",
            "machine_certificates","virtualization","deep_inventory_collected_at"
        }) {
            snapshot.erase(key);
        }
    }
    return snapshot;
}

} // namespace hi5