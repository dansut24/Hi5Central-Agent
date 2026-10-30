#pragma once
// Read-only identity used to invalidate a local codec qualification after a
// Windows build or GPU driver changes. Does not run WMI or benchmark on connect.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "codec_universal_selector.h"
#include <cstdint>
#include <string>

namespace hi5::universal_codec {
inline std::string hexVersion(std::uint64_t value) {
    static const char digits[]="0123456789abcdef";
    std::string out(16,'0');
    for(int i=15;i>=0;--i) {out[i]=digits[value&15];value>>=4;}
    return out;
}
inline std::string gpuDriverFingerprint() {
    std::string fingerprint;
    for(DWORD index=0;index<16;++index) {
        DISPLAY_DEVICEW display{};
        display.cb=sizeof(display);
        if(!EnumDisplayDevicesW(nullptr,index,&display,0))break;
        if(!(display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            display.DeviceID[0]==L'\0' || display.DeviceKey[0]==L'\0')continue;
        const std::wstring key=display.DeviceKey;
        // EnumDisplayDevices returns \Registry\Machine\SYSTEM\...
        const std::wstring marker=L"\\Registry\\Machine\\";
        if(key.compare(0,marker.size(),marker)!=0)continue;
        HKEY gpuKey=nullptr;
        if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,key.c_str()+marker.size(),0,
                         KEY_QUERY_VALUE,&gpuKey)!=ERROR_SUCCESS)continue;
        FILETIME lastWrite{};
        const LONG rc=RegQueryInfoKeyW(gpuKey,nullptr,nullptr,nullptr,nullptr,
            nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&lastWrite);
        // A registry write time is part of the driver fingerprint. A driver
        // update invalidates the cache even if a display name stays the same.
        if(rc==ERROR_SUCCESS) {
            const std::wstring id=display.DeviceID;
            std::string serial;
            for(wchar_t ch:id) serial+=static_cast<char>(ch&0x7f);
            const std::uint64_t stamp=(static_cast<std::uint64_t>(
                lastWrite.dwHighDateTime)<<32)|lastWrite.dwLowDateTime;
            fingerprint+=serial+"@"+hexVersion(stamp)+";";
        }
        RegCloseKey(gpuKey);
    }
    if (!fingerprint.empty()) return fingerprint;

    // Session 0 services often have NO attached desktop: EnumDisplayDevicesW
    // returns zero entries even though Win32_VideoController has a working GPU.
    // Fallback to the display driver's HKLM class key (machine-scoped), which
    // is available to the SYSTEM service without an interactive session.
    // DriverVersion, hardware ID and key write-time all invalidate on update.
    constexpr wchar_t displayClass[] =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
        L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY classKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, displayClass, 0,
                      KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
                      &classKey) != ERROR_SUCCESS) return {};
    auto readRegistryText = [](HKEY key, const wchar_t* name) {
        wchar_t value[512]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(key, nullptr, name,
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         nullptr, value, &size) != ERROR_SUCCESS) {
            return std::wstring();
        }
        return std::wstring(value);
    };
    auto asAscii = [](const std::wstring& wide) {
        std::string text;
        text.reserve(wide.size());
        for (wchar_t c : wide) {
            text.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
        return text;
    };
    for (DWORD index = 0; index < 64; ++index) {
        wchar_t keyName[128]{};
        DWORD keySize = static_cast<DWORD>(
            sizeof(keyName) / sizeof(keyName[0]));
        FILETIME enumeratedWrite{};
        const LONG status = RegEnumKeyExW(classKey, index, keyName,
            &keySize, nullptr, nullptr, nullptr, &enumeratedWrite);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || keySize != 4) continue;
        // Class subkeys like "0000" identify display driver instances.
        bool numericKey = true;
        for (DWORD j = 0; j < keySize; ++j) {
            if (keyName[j] < L'0' || keyName[j] > L'9') numericKey = false;
        }
        if (!numericKey) continue;
        HKEY driver = nullptr;
        if (RegOpenKeyExW(classKey, keyName, 0, KEY_QUERY_VALUE,
                          &driver) != ERROR_SUCCESS) continue;
        const std::wstring driverVersion =
            readRegistryText(driver, L"DriverVersion");
        const std::wstring matchingId =
            readRegistryText(driver, L"MatchingDeviceId");
        if (!driverVersion.empty() && !matchingId.empty()) {
            FILETIME lastWrite = enumeratedWrite;
            RegQueryInfoKeyW(driver, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                &lastWrite);
            const std::uint64_t timestamp =
                (static_cast<std::uint64_t>(lastWrite.dwHighDateTime) << 32)
                | lastWrite.dwLowDateTime;
            fingerprint += asAscii(matchingId) + "@" +
                asAscii(driverVersion) + "|" +
                hexVersion(timestamp) + ";";
        }
        RegCloseKey(driver);
    }
    RegCloseKey(classKey);
    // Empty remains fail-closed; an unknown driver can never qualify hardware.
    return fingerprint;
}
inline CacheIdentity windowsCacheIdentity(const std::string& agentBuild,
                                           std::int64_t timestamp) {
    CacheIdentity identity;
    identity.agentBuild=agentBuild;
    identity.encoderConfigVersion="universal-connection-v1";
    identity.qualifiedAtUnixSeconds=timestamp;
    wchar_t build[40]{};
    DWORD len=sizeof(build);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
          L"CurrentBuildNumber",RRF_RT_REG_SZ,nullptr,build,&len)==ERROR_SUCCESS) {
        std::string version;
        for(wchar_t ch:build){if(!ch)break;version+=static_cast<char>(ch);}
        identity.operatingSystem="Windows-build-"+version;
    }
    identity.gpuAndDriver=gpuDriverFingerprint();
    return identity;
}
} // namespace hi5::universal_codec
