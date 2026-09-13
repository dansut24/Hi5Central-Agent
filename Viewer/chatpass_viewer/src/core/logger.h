#pragma once

#include <string>

namespace hi5 {

void LogInfo(const std::string& message);
void LogWarn(const std::string& message);
void LogError(const std::string& message);

} // namespace hi5