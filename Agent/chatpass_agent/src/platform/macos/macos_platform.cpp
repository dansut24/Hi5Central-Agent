#include "../platform.h"

#if defined(__APPLE__)

namespace hi5 {

PlatformInfo getPlatformInfo() {
    PlatformInfo info;
    info.osName = "macOS";
    info.osVersion = "macOS";
    info.architecture = "unknown";
    info.isServer = false;
    info.hasDesktopSession = false;
    info.isElevated = false;
    return info;
}

} // namespace hi5

#endif