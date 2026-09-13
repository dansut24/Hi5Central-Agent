#pragma once

#include <string>

struct AgentIdentity {
    std::string deviceId;
    std::string deviceKey;
    std::string agentWsBaseUrl;
};

// Loads the per-device identity from encrypted state.dat.
// If state.dat does not exist, performs first-run enrolment, writes state.dat,
// and migrates legacy config.ini/secrets.dat out of active use.
AgentIdentity loadAgentIdentityFromDir(
    const std::wstring& dir,
    const std::string& defaultAgentWsBaseUrl
);
