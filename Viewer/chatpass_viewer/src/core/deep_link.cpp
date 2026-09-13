#include "core/deep_link.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>

namespace hi5 {
namespace {

std::string UrlDecode(const std::string& input) {
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const auto hex = input.substr(i + 1, 2);
            char ch = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            out.push_back(ch);
            i += 2;
        } else if (input[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(input[i]);
        }
    }

    return out;
}

std::unordered_map<std::string, std::string> ParseQuery(const std::string& query) {
    std::unordered_map<std::string, std::string> result;
    std::stringstream ss(query);
    std::string part;

    while (std::getline(ss, part, '&')) {
        const auto pos = part.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        const std::string key = UrlDecode(part.substr(0, pos));
        const std::string value = UrlDecode(part.substr(pos + 1));
        result[key] = value;
    }

    return result;
}

} // namespace

DeepLinkLaunch ParseDeepLink(const std::string& raw) {
    DeepLinkLaunch launch;
    launch.raw = raw;

    const bool supportedScheme =
        raw.rfind("hi5central-viewer://connect", 0) == 0 ||
        raw.rfind("hi5central-viewer://session", 0) == 0 ||
        raw.rfind("hi5viewer://connect", 0) == 0 ||
        raw.rfind("hi5viewer://session", 0) == 0 ||
        raw.rfind("hi5tech://connect", 0) == 0 ||
        raw.rfind("hi5tech://session", 0) == 0;

    if (!supportedScheme) {
        return launch;
    }

    const auto qpos = raw.find('?');
    if (qpos == std::string::npos || qpos + 1 >= raw.size()) {
        return launch;
    }

    const auto params = ParseQuery(raw.substr(qpos + 1));

    auto get = [&](const char* key) -> std::string {
        auto it = params.find(key);
        return it == params.end() ? std::string{} : it->second;
    };

    launch.sessionId = get("session_id");
    if (launch.sessionId.empty()) launch.sessionId = get("sessionId");

    launch.token = get("token");
    if (launch.token.empty()) launch.token = get("viewer_token");
    if (launch.token.empty()) launch.token = get("viewerToken");

    launch.deviceId = get("device_id");
    if (launch.deviceId.empty()) launch.deviceId = get("deviceId");

    launch.wssUrl = get("wss_url");
    if (launch.wssUrl.empty()) launch.wssUrl = get("wssUrl");
    if (launch.wssUrl.empty()) launch.wssUrl = get("signaling_url");
    if (launch.wssUrl.empty()) launch.wssUrl = get("signalingUrl");

    launch.mode = get("mode");
    if (launch.mode == "background" || launch.mode == "background_mode") {
        launch.mode = "backstage";
    }
    if (launch.mode != "backstage") {
        launch.mode = "console";
    }

    launch.valid = !launch.sessionId.empty() && !launch.deviceId.empty() && !launch.wssUrl.empty();
    return launch;
}

} // namespace hi5
