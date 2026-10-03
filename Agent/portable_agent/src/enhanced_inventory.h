#pragma once

#include <nlohmann/json.hpp>

namespace hi5 {

nlohmann::json enhancedHardwareInventory();
nlohmann::json remoteDesktopCapabilities();

} // namespace hi5