#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace hi5 {

struct CommandResult {
    int exitCode = 1;
    std::string output;
    std::string error;
    std::int64_t durationMs = 0;
    bool timedOut = false;
    bool truncated = false;
};

CommandResult runShellCommand(
    const std::string& command,
    int timeoutSeconds,
    std::size_t maxOutputBytes = 256 * 1024,
    const std::string& runAs = "root");

nlohmann::json buildCommandResultJson(
    const std::string& command,
    const CommandResult& result);

} // namespace hi5
