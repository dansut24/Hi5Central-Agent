#pragma once

#include <string>

namespace hi5 {

struct DeepLinkLaunch {
    bool valid = false;
    std::string raw;
    std::string sessionId;
    std::string token;
    std::string deviceId;
    std::string wssUrl;
    std::string mode = "console";
};

DeepLinkLaunch ParseDeepLink(const std::string& raw);

} // namespace hi5