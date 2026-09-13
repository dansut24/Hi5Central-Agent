#pragma once

#include <string>

namespace hi5 {

	int InstallService(const std::string& serviceName);
	int UninstallService(const std::string& serviceName);

	// Runs as SCM service if started by SCM; otherwise console fallback.
	int RunServiceMain(const std::string& serviceName);

} // namespace hi5