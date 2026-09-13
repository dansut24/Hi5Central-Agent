#include "../platform.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace hi5 {

static std::string getArchitecture() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

static bool isRunningElevated() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;

    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin == TRUE;
}

static bool hasInteractiveDesktop() {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (desktop) {
        CloseDesktop(desktop);
        return true;
    }
    return false;
}

PlatformInfo getPlatformInfo() {
    PlatformInfo info;
    info.osName = "Windows";
    info.osVersion = "Windows";
    info.architecture = getArchitecture();
    info.isServer = false;
    info.hasDesktopSession = hasInteractiveDesktop();
    info.isElevated = isRunningElevated();
    return info;
}

} // namespace hi5

#endif