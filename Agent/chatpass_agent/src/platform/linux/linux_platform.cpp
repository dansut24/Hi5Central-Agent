#include "../platform.h"

#if defined(__linux__)

namespace hi5 {

PlatformInfo getPlatformInfo() {
    PlatformInfo info;
    info.osName = "Linux";
    info.osVersion = "Linux";
    info.architecture = "unknown";
    info.isServer = false;
    info.hasDesktopSession = false;
    info.isElevated = false;
    return info;
}

} // namespace hi5

#endif