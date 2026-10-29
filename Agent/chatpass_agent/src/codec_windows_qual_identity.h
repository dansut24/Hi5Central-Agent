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
    return fingerprint; // empty => cached hardware never trusted
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
