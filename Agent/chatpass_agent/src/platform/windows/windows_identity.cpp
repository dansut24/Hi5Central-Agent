#include "platform/platform_identity.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <string>

namespace hi5 {

std::string GetPlatformHostname() {
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(buffer);

    if (GetComputerNameA(buffer, &size) && size > 0) {
        return std::string(buffer, size);
    }

    return "windows-host";
}

std::string GetPlatformMachineId() {
    std::string hostname = GetPlatformHostname();

    std::transform(hostname.begin(), hostname.end(), hostname.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return "windows-hostname:" + hostname;
}

} // namespace hi5

#endif