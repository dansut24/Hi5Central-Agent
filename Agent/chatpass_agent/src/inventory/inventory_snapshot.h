#pragma once

#include "../agent_identity.h"

#include <nlohmann/json.hpp>

namespace hi5 {

// Builds a Windows inventory snapshot suitable for sending to the RMM control server.
// The payload returned by this function already includes { type: "inventory_snapshot" }.
#ifdef _WIN32
// Deep hardware collection is Windows-specific; never change the one-argument
// inventory ABI used by macOS/Linux Agent implementations.
nlohmann::json BuildInventorySnapshot(const AgentIdentity& identity, bool forceDeep = false, bool allowScheduledDeep = false);
#else
nlohmann::json BuildInventorySnapshot(const AgentIdentity& identity);
#endif

} // namespace hi5
