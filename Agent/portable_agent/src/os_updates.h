#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace hi5 {

struct OsUpdateActionResult {
    bool success = false;
    nlohmann::json result = nlohmann::json::object();
    std::string error;
};

// Returns platform-native operating-system update inventory. When
// refreshMetadata is true, Linux refreshes package indexes before discovery.
nlohmann::json osUpdateInventory(bool refreshMetadata = false);

// Installs the selected platform-native OS updates. The payload accepts:
//   update_ids: [string, ...]
//   all: bool
//   security_only: bool
//   allow_feature_upgrade: bool (macOS major upgrades; false by default)
OsUpdateActionResult installOsUpdates(const nlohmann::json& payload);

} // namespace hi5
