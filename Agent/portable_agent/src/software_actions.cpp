#include "software_actions.h"

#include "job_executor.h"
#include "platform_info.h"
#include "software_inventory.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace hi5 {
namespace {

using json = nlohmann::json;

std::string clean(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool safePackageId(const std::string& value) {
    if (value.empty() || value.size() > 300) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '+' || ch == '-' ||
            ch == '_' || ch == ':' || ch == '@';
    });
}

bool protectedPackage(const std::string& name, const std::string& id) {
    const std::string value = lower(name + " " + id);
    static const char* protectedTerms[] = {
        "hi5central", "crowdstrike", "sentinelone", "sophos", "bitdefender",
        "cylance", "carbonblack", "carbon black", "defender", "malwarebytes",
        "huntress", "falcon-sensor"
    };
    for (const auto* term : protectedTerms) {
        if (value.find(term) != std::string::npos) return true;
    }
    return false;
}

struct NativePackage {
    std::string manager;
    std::string id;
    std::string scope;
};

NativePackage packageFromPayload(const json& payload) {
    NativePackage package;
    package.manager = lower(clean(payload.value(
        "package_manager",
        payload.value("packageManager", std::string()))));
    package.id = clean(payload.value(
        "package_id",
        payload.value("packageId", std::string())));
    package.scope = lower(clean(payload.value("scope", std::string("system"))));

    const std::string key = clean(payload.value(
        "registry_key",
        payload.value("registryKey", std::string())));

    if (package.manager.empty() || package.id.empty()) {
        if (key.rfind("dpkg:", 0) == 0) {
            package.manager = "apt";
            std::string body = key.substr(5);
            const auto lastColon = body.rfind(':');
            package.id = lastColon == std::string::npos ? body : body.substr(0, lastColon);
        } else if (key.rfind("snap:", 0) == 0) {
            package.manager = "snap";
            package.id = key.substr(5);
        } else if (key.rfind("flatpak:", 0) == 0) {
            package.manager = "flatpak";
            std::string body = key.substr(8);
            const auto lastColon = body.rfind(':');
            package.id = lastColon == std::string::npos ? body : body.substr(0, lastColon);
            if (lastColon != std::string::npos) package.scope = body.substr(lastColon + 1);
        } else if (key.rfind("app:", 0) == 0) {
            package.manager = "app_bundle";
            package.id = key.substr(4);
        }
    }

    return package;
}

nlohmann::json currentPackageItem(const NativePackage& package) {
    const auto inventory = softwareInventory();
    if (!inventory.is_object()) return nullptr;
    const auto items = inventory.value("items", json::array());
    if (!items.is_array()) return nullptr;

    for (const auto& item : items) {
        if (!item.is_object()) continue;
        if (lower(clean(item.value("package_manager", ""))) != package.manager) continue;
        if (clean(item.value("package_id", "")) != package.id) continue;
        const auto scope = lower(clean(item.value("scope", "system")));
        if (package.manager == "flatpak" && !package.scope.empty() && scope != package.scope) continue;
        return item;
    }
    return nullptr;
}

bool packageStillInstalled(const NativePackage& package) {
    return currentPackageItem(package).is_object();
}

bool aptRemovalIsSafe(const NativePackage& package, std::string* detail) {
    const std::string command = "DEBIAN_FRONTEND=noninteractive /usr/bin/apt-get --simulate remove -- " + package.id;
    const auto simulation = runShellCommand(command, 60, 256 * 1024, "root");
    if (simulation.exitCode != 0 || simulation.timedOut || !simulation.error.empty()) {
        if (detail) *detail = simulation.error.empty() ? "APT removal simulation failed." : simulation.error;
        return false;
    }

    const auto lowered = lower(simulation.output);
    if (lowered.find("essential packages will be removed") != std::string::npos ||
        lowered.find("warning: the following essential") != std::string::npos) {
        if (detail) *detail = "APT reports that an essential package would be removed.";
        return false;
    }

    std::vector<std::string> removals;
    std::istringstream stream(simulation.output);
    std::string line;
    while (std::getline(stream, line)) {
        line = clean(line);
        if (line.rfind("Remv ", 0) != 0) continue;
        std::istringstream row(line.substr(5));
        std::string packageName;
        row >> packageName;
        const auto colon = packageName.rfind(':');
        if (colon != std::string::npos) {
            const auto suffix = packageName.substr(colon + 1);
            if (suffix == "amd64" || suffix == "i386" || suffix == "arm64" || suffix == "armhf") packageName = packageName.substr(0, colon);
        }
        if (!packageName.empty()) removals.push_back(packageName);
    }

    if (removals.empty()) {
        if (detail) *detail = "APT did not produce a verifiable removal plan.";
        return false;
    }
    for (const auto& removal : removals) {
        if (removal != package.id) {
            if (detail) *detail = "APT would also remove dependency/reverse-dependency package: " + removal;
            return false;
        }
    }
    return true;
}

NativeSoftwareActionResult unsupported(
    const std::string& reason,
    const std::string& detail) {
    NativeSoftwareActionResult out;
    out.result = {
        {"status", "not_supported"},
        {"reason", reason},
        {"detail", detail}
    };
    out.error = detail;
    return out;
}

NativeSoftwareActionResult executePackageAction(
    const json& payload,
    bool update) {

    NativeSoftwareActionResult out;
    const auto package = packageFromPayload(payload);
    const std::string name = clean(payload.value("name", package.id));

    if (protectedPackage(name, package.id)) {
        out.result = {
            {"status", "blocked"},
            {"reason", "protected_security_or_agent"},
            {"detail", "Protected Agent or security software cannot be modified by this action."}
        };
        out.error = "Protected software action blocked.";
        return out;
    }

    if (!safePackageId(package.id)) {
        return unsupported(
            "no_safe_silent_uninstaller_found",
            "No trusted native package identity was available for this application.");
    }

#if defined(__linux__)
    const auto currentItem = currentPackageItem(package);
    if (!currentItem.is_object()) {
        return unsupported(
            "software_not_found",
            "The selected package is no longer present in the endpoint inventory.");
    }
    if (!currentItem.value("display_in_installed_software", false) ||
        !currentItem.value("native_actionable", false) ||
        currentItem.value("system_component", true)) {
        return unsupported(
            "protected_system_component",
            "Hi5Central classified this package as an operating-system component or non-actionable package.");
    }
    std::string command;
    std::string runAs = "root";

    if (package.manager == "apt") {
        if (!update) {
            std::string simulationDetail;
            if (!aptRemovalIsSafe(package, &simulationDetail)) {
                return unsupported(
                    "unsafe_dependency_removal",
                    simulationDetail.empty()
                        ? "APT could not prove this uninstall is isolated to the selected application."
                        : simulationDetail);
            }
        }
        command = update
            ? "DEBIAN_FRONTEND=noninteractive /usr/bin/apt-get install -y --only-upgrade -- " + package.id
            : "DEBIAN_FRONTEND=noninteractive /usr/bin/apt-get remove -y -- " + package.id;
    } else if (package.manager == "snap") {
        command = update
            ? "/usr/bin/snap refresh " + package.id
            : "/usr/bin/snap remove " + package.id;
    } else if (package.manager == "flatpak") {
        const bool user = package.scope == "user";
        runAs = user ? "user" : "root";
        command = update
            ? "/usr/bin/flatpak update -y " + package.id
            : "/usr/bin/flatpak uninstall -y " + package.id;
        if (!user) command += " --system";
    } else {
        return unsupported(
            "no_safe_silent_uninstaller_found",
            "This Linux application is not managed by a supported native package manager.");
    }

    const auto result = runShellCommand(command, update ? 900 : 600, 512 * 1024, runAs);
    out.result = buildCommandResultJson(command, result);
    out.result["package_manager"] = package.manager;
    out.result["package_id"] = package.id;
    out.result["scope"] = package.scope;
    out.result["action"] = update ? "update" : "uninstall";

    if (result.exitCode != 0 || !result.error.empty()) {
        out.result["status"] = "failed";
        out.result["reason"] = update ? "native_update_failed" : "silent_uninstall_failed";
        out.error = result.error.empty()
            ? "Native package manager exited with code " + std::to_string(result.exitCode)
            : result.error;
        return out;
    }

    const bool installedAfter = packageStillInstalled(package);
    if (update) {
        out.success = true;
        out.result["status"] = "updated";
        out.result["still_installed"] = installedAfter;
        return out;
    }

    if (installedAfter) {
        out.result["status"] = "failed";
        out.result["reason"] = "silent_uninstall_failed";
        out.error = "Package manager completed but the application is still present in inventory.";
        return out;
    }

    out.success = true;
    out.result["status"] = "uninstalled";
    out.result["reboot_required"] = false;
    return out;
#elif defined(__APPLE__)
    (void)update;
    if (package.manager == "app_bundle") {
        return unsupported(
            "no_safe_silent_uninstaller_found",
            "This macOS application is an app bundle without a verified vendor or package-manager uninstall method.");
    }
    return unsupported(
        "no_safe_silent_uninstaller_found",
        "This macOS software source does not currently expose a verified native package action.");
#else
    return unsupported("unsupported_platform", "Native software actions are not supported on this platform.");
#endif
}

} // namespace

NativeSoftwareActionResult uninstallNativeSoftware(const nlohmann::json& payload) {
    return executePackageAction(payload, false);
}

NativeSoftwareActionResult updateNativeSoftware(const nlohmann::json& payload) {
    return executePackageAction(payload, true);
}

} // namespace hi5
