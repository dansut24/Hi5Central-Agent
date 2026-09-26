#pragma once

#include "../agent_identity.h"

#include <nlohmann/json.hpp>

namespace hi5 {

// Builds a Windows inventory snapshot suitable for sending to the RMM control server.
// The payload returned by this function already includes { type: "inventory_snapshot" }.
nlohmann::json BuildInventorySnapshot(const AgentIdentity& identity);
nlohmann::json BuildBitLockerRecoveryEscrow(const AgentIdentity& identity);

} // namespace hi5
