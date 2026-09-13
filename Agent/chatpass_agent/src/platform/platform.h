#pragma once

#include <string>

namespace hi5 {

struct PlatformInfo {
    std::string osName;
    std::string osVersion;
    std::string architecture;
    bool isServer = false;
    bool hasDesktopSession = false;
    bool isElevated = false;
};

PlatformInfo getPlatformInfo();

} // namespace hi5