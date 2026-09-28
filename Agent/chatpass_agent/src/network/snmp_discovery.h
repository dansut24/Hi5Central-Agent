#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace hi5::network {

nlohmann::json RunSnmpDiscovery(const nlohmann::json& payload, std::string& error);
nlohmann::json RunNetworkDiscoveryEnrichment(const nlohmann::json& payload, std::string& error);

}  // namespace hi5::network
