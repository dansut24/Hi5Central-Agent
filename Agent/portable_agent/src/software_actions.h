#pragma once

#include <nlohmann/json.hpp>

namespace hi5 {

struct NativeSoftwareActionResult {
    bool success = false;
    nlohmann::json result = nlohmann::json::object();
    std::string error;
};

NativeSoftwareActionResult uninstallNativeSoftware(const nlohmann::json& payload);
NativeSoftwareActionResult updateNativeSoftware(const nlohmann::json& payload);

} // namespace hi5
