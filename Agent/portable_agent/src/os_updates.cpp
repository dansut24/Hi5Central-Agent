#include "os_updates.h"

#include "job_executor.h"
#include "platform_info.h"
#include "software_inventory.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace hi5 {
namespace {

using json = nlohmann::json;

std::mutex g_cacheMutex;
json g_cachedInventory;
std::chrono::steady_clock::time_point g_cachedAt{};
constexpr auto kCacheTtl = std::chrono::minutes(30);

std::string trim(std::string value) {
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

std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (const char ch : value) {
        if (ch == '\'') result += "'\\''";
        else result.push_back(ch);
    }
    result += "'";
    return result;
}

bool safeLinuxPackageId(const std::string& value) {
    if (value.empty() || value.size() > 300) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '+' || ch == '-' ||
            ch == '_' || ch == ':' || ch == '@';
    });
}

int majorVersion(const std::string& value) {
    std::string digits;
    for (const char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch))) digits.push_back(ch);
        else if (!digits.empty()) break;
    }
    if (digits.empty()) return -1;
    try { return std::stoi(digits); } catch (...) { return -1; }
}

std::string fieldFromCommaLine(const std::string& line, const std::string& key) {
    const auto pos = line.find(key);
    if (pos == std::string::npos) return {};
    const auto start = pos + key.size();
    const auto end = line.find(',', start);
    return trim(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

json baseInventory(const std::string& provider) {
    return {
        {"status", "collected"},
        {"provider", provider},
        {"last_scan_utc", nowIsoUtc()},
        {"pending_count", 0},
        {"security_count", 0},
        {"reboot_required", false},
        {"updates", json::array()}
    };
}

#if defined(__linux__)

std::map<std::string, json> linuxPackageClassification() {
    std::map<std::string, json> result;
    const auto software = softwareInventory();
    if (!software.is_object()) return result;
    const auto components = software.value("components", json::array());
    if (!components.is_array()) return result;

    for (const auto& item : components) {
        if (!item.is_object()) continue;
        if (lower(item.value("package_manager", std::string())) != "apt") continue;
        const auto id = trim(item.value("package_id", std::string()));
        if (!id.empty()) result[id] = item;
    }
    return result;
}

std::string aptLookupId(std::string id) {
    const auto colon = id.rfind(':');
    if (colon == std::string::npos) return id;
    const auto suffix = lower(id.substr(colon + 1));
    if (suffix == "amd64" || suffix == "i386" || suffix == "arm64" ||
        suffix == "armhf" || suffix == "all") {
        return id.substr(0, colon);
    }
    return id;
}

json collectLinuxUpdates(bool refreshMetadata) {
    json inventory = baseInventory("apt");
    std::string refreshError;

    if (refreshMetadata) {
        const auto refresh = runShellCommand(
            "DEBIAN_FRONTEND=noninteractive /usr/bin/apt-get update -qq",
            300,
            512 * 1024,
            "root");
        if (refresh.exitCode != 0 || refresh.timedOut || !refresh.error.empty()) {
            refreshError = refresh.error.empty()
                ? "apt-get update exited with code " + std::to_string(refresh.exitCode)
                : refresh.error;
        }
    }

    const auto classification = linuxPackageClassification();
    const auto listing = runShellCommand(
        "/usr/bin/apt list --upgradable 2>/dev/null",
        120,
        1024 * 1024,
        "root");

    if (listing.exitCode != 0 && listing.output.empty()) {
        inventory["status"] = "error";
        inventory["error"] = listing.error.empty()
            ? "Unable to enumerate APT updates."
            : listing.error;
        return inventory;
    }

    json updates = json::array();
    std::istringstream stream(listing.output);
    std::string line;
    int securityCount = 0;
    bool restartLikely = false;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line.rfind("Listing...", 0) == 0) continue;

        const auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        const std::string rawId = trim(line.substr(0, slash));
        const std::string lookupId = aptLookupId(rawId);
        if (!safeLinuxPackageId(rawId) || lookupId.empty()) continue;

        const auto classIt = classification.find(lookupId);
        if (classIt == classification.end()) continue;
        const auto& classified = classIt->second;
        // Application packages belong to Software Patching. OS Patching only
        // receives the protected/system component side of the package graph.
        if (!classified.value("system_component", true)) continue;

        const auto afterSlash = line.substr(slash + 1);
        const auto firstSpace = afterSlash.find(' ');
        if (firstSpace == std::string::npos) continue;
        const std::string source = trim(afterSlash.substr(0, firstSpace));
        std::istringstream fields(afterSlash.substr(firstSpace + 1));
        std::string availableVersion;
        std::string architecture;
        fields >> availableVersion >> architecture;
        if (availableVersion.empty()) continue;

        std::string installedVersion;
        const std::string marker = "[upgradable from:";
        const auto markerPos = line.find(marker);
        if (markerPos != std::string::npos) {
            const auto valueStart = markerPos + marker.size();
            const auto valueEnd = line.find(']', valueStart);
            installedVersion = trim(line.substr(
                valueStart,
                valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart));
        }

        const auto loweredLine = lower(line);
        const bool security =
            loweredLine.find("-security") != std::string::npos ||
            loweredLine.find("/security") != std::string::npos ||
            loweredLine.find(" security") != std::string::npos;
        const auto loweredId = lower(lookupId);
        const bool packageRestartLikely =
            loweredId.rfind("linux-image", 0) == 0 ||
            loweredId.rfind("linux-headers", 0) == 0 ||
            loweredId.rfind("linux-modules", 0) == 0 ||
            loweredId == "linux-generic" ||
            loweredId == "systemd" ||
            loweredId == "libc6";

        if (security) ++securityCount;
        restartLikely = restartLikely || packageRestartLikely;

        updates.push_back({
            {"id", rawId},
            {"package", lookupId},
            {"name", lookupId},
            {"title", lookupId},
            {"installed_version", installedVersion},
            {"available_version", availableVersion},
            {"architecture", architecture},
            {"source", source},
            {"package_manager", "apt"},
            {"security", security},
            {"severity", security ? "Security" : "Normal"},
            {"category", security ? "Security" : "System"},
            {"reboot_required", packageRestartLikely},
            {"system_component", true},
            {"actionable", true}
        });
    }

    const bool rebootRequired =
        std::filesystem::exists("/var/run/reboot-required") || restartLikely;

    inventory["updates"] = updates;
    inventory["pending_count"] = updates.size();
    inventory["security_count"] = securityCount;
    inventory["reboot_required"] = rebootRequired;
    if (!refreshError.empty()) inventory["refresh_error"] = refreshError;
    return inventory;
}

#endif

#if defined(__APPLE__)

void pushMacUpdate(json& updates, json& current, int& securityCount, bool& rebootRequired) {
    if (!current.is_object()) return;
    const auto id = trim(current.value("id", std::string()));
    if (id.empty()) return;

    auto title = trim(current.value("title", id));
    auto version = trim(current.value("available_version", std::string()));
    const auto lowered = lower(title + " " + id);
    const bool security =
        lowered.find("security") != std::string::npos ||
        lowered.find("rapid response") != std::string::npos ||
        lowered.find("xprotect") != std::string::npos ||
        lowered.find("malware removal") != std::string::npos ||
        lowered.find("mrtconfigdata") != std::string::npos;
    const bool restart = current.value("reboot_required", false);

    const int currentMajor = majorVersion(osVersion());
    const int targetMajor = majorVersion(version);
    const bool macOsUpdate =
        lowered.find("macos") != std::string::npos ||
        lowered.find("mac os") != std::string::npos;
    const bool featureUpgrade =
        macOsUpdate && currentMajor > 0 && targetMajor > 0 && targetMajor > currentMajor;

    current["security"] = security;
    current["severity"] = security ? "Security" : "Normal";
    current["category"] = featureUpgrade ? "Feature" : (security ? "Security" : "Apple");
    current["feature_upgrade"] = featureUpgrade;
    current["actionable"] = !featureUpgrade;
    current["package_manager"] = "softwareupdate";
    current["source"] = "Apple Software Update";
    current["system_component"] = true;

    if (security) ++securityCount;
    rebootRequired = rebootRequired || restart;
    updates.push_back(current);
}

json collectMacUpdates() {
    json inventory = baseInventory("softwareupdate");
    const auto listing = runShellCommand(
        "/usr/sbin/softwareupdate --list --all",
        240,
        1024 * 1024,
        "root");

    // softwareupdate may return 0 with "No new software available.".
    if (listing.exitCode != 0 && listing.output.empty()) {
        inventory["status"] = "error";
        inventory["error"] = listing.error.empty()
            ? "Unable to enumerate Apple software updates."
            : listing.error;
        return inventory;
    }

    json updates = json::array();
    json current;
    int securityCount = 0;
    bool rebootRequired = false;

    std::istringstream stream(listing.output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto stripped = trim(line);
        if (stripped.empty()) continue;

        const auto labelPos = stripped.find("Label:");
        if (!stripped.empty() && stripped[0] == '*' && labelPos != std::string::npos) {
            pushMacUpdate(updates, current, securityCount, rebootRequired);
            current = json::object();
            current["id"] = trim(stripped.substr(labelPos + 6));
            current["title"] = current["id"];
            current["installed_version"] = osVersion();
            current["available_version"] = "";
            current["reboot_required"] = false;
            continue;
        }

        // Some releases emit "* <label>" rather than "* Label: <label>".
        if (!stripped.empty() && stripped[0] == '*' && labelPos == std::string::npos) {
            pushMacUpdate(updates, current, securityCount, rebootRequired);
            current = json::object();
            current["id"] = trim(stripped.substr(1));
            current["title"] = current["id"];
            current["installed_version"] = osVersion();
            current["available_version"] = "";
            current["reboot_required"] = false;
            continue;
        }

        if (!current.is_object() || current.empty()) continue;

        const auto title = fieldFromCommaLine(stripped, "Title:");
        const auto version = fieldFromCommaLine(stripped, "Version:");
        const auto size = fieldFromCommaLine(stripped, "Size:");
        const auto recommended = fieldFromCommaLine(stripped, "Recommended:");
        const auto action = fieldFromCommaLine(stripped, "Action:");

        if (!title.empty()) current["title"] = title;
        if (!version.empty()) current["available_version"] = version;
        if (!size.empty()) current["size"] = size;
        if (!recommended.empty()) current["recommended"] = lower(recommended) == "yes";
        if (lower(action).find("restart") != std::string::npos ||
            lower(stripped).find("action: restart") != std::string::npos) {
            current["reboot_required"] = true;
        }
    }
    pushMacUpdate(updates, current, securityCount, rebootRequired);

    inventory["updates"] = updates;
    inventory["pending_count"] = updates.size();
    inventory["security_count"] = securityCount;
    inventory["reboot_required"] = rebootRequired;
    return inventory;
}

#endif

json collectPlatformUpdates(bool refreshMetadata) {
#if defined(__linux__)
    return collectLinuxUpdates(refreshMetadata);
#elif defined(__APPLE__)
    (void)refreshMetadata;
    return collectMacUpdates();
#else
    (void)refreshMetadata;
    json inventory = baseInventory("unsupported");
    inventory["status"] = "not_supported";
    return inventory;
#endif
}

std::vector<std::string> requestedUpdateIds(const json& payload, const json& inventory) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    const bool all = payload.value("all", false);
    const bool securityOnly = payload.value("security_only", false);

    if (all) {
        const auto updates = inventory.value("updates", json::array());
        if (updates.is_array()) {
            for (const auto& item : updates) {
                if (!item.is_object()) continue;
                if (securityOnly && !item.value("security", false)) continue;
                if (!item.value("actionable", true)) continue;
                const auto id = trim(item.value("id", std::string()));
                if (!id.empty() && seen.insert(id).second) result.push_back(id);
            }
        }
        return result;
    }

    const auto ids = payload.value("update_ids", json::array());
    if (!ids.is_array()) return result;
    for (const auto& value : ids) {
        if (!value.is_string()) continue;
        const auto id = trim(value.get<std::string>());
        if (!id.empty() && seen.insert(id).second) result.push_back(id);
    }
    return result;
}

json itemForId(const json& inventory, const std::string& id) {
    const auto updates = inventory.value("updates", json::array());
    if (!updates.is_array()) return nullptr;
    for (const auto& item : updates) {
        if (item.is_object() && trim(item.value("id", std::string())) == id) return item;
    }
    return nullptr;
}

} // namespace

json osUpdateInventory(bool refreshMetadata) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (!refreshMetadata && g_cachedInventory.is_object() &&
            g_cachedAt.time_since_epoch().count() != 0 &&
            now - g_cachedAt < kCacheTtl) {
            return g_cachedInventory;
        }
    }

    auto inventory = collectPlatformUpdates(refreshMetadata);
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cachedInventory = inventory;
        g_cachedAt = std::chrono::steady_clock::now();
    }
    return inventory;
}

OsUpdateActionResult installOsUpdates(const json& payload) {
    OsUpdateActionResult out;
    const auto before = osUpdateInventory(true);
    if (!before.is_object() || before.value("status", std::string()) == "error") {
        out.error = before.value("error", std::string("OS update inventory is unavailable."));
        out.result = {{"status", "failed"}, {"reason", "inventory_unavailable"}};
        return out;
    }

    const auto ids = requestedUpdateIds(payload, before);
    if (ids.empty()) {
        out.error = "No eligible operating-system updates were selected.";
        out.result = {{"status", "not_applicable"}, {"reason", "no_updates_selected"}};
        return out;
    }

    const bool allowFeatureUpgrade = payload.value("allow_feature_upgrade", false);
    bool rebootRequired = false;
    json selected = json::array();

    for (const auto& id : ids) {
        const auto item = itemForId(before, id);
        if (!item.is_object()) {
            out.error = "Selected update is no longer available: " + id;
            out.result = {{"status", "failed"}, {"reason", "update_not_found"}, {"update_id", id}};
            return out;
        }
        if (item.value("feature_upgrade", false) && !allowFeatureUpgrade) {
            out.error = "macOS major upgrades require an explicit feature-upgrade policy.";
            out.result = {{"status", "blocked"}, {"reason", "feature_upgrade_not_allowed"}, {"update_id", id}};
            return out;
        }
        if (!item.value("actionable", true) && !item.value("feature_upgrade", false)) {
            out.error = "Selected update is not actionable: " + id;
            out.result = {{"status", "blocked"}, {"reason", "update_not_actionable"}, {"update_id", id}};
            return out;
        }
        rebootRequired = rebootRequired || item.value("reboot_required", false);
        selected.push_back(item);
    }

    std::string command;
#if defined(__linux__)
    command = "DEBIAN_FRONTEND=noninteractive /usr/bin/apt-get install -y --only-upgrade --";
    for (const auto& id : ids) {
        if (!safeLinuxPackageId(id)) {
            out.error = "Unsafe Linux package identity: " + id;
            out.result = {{"status", "blocked"}, {"reason", "unsafe_package_id"}, {"update_id", id}};
            return out;
        }
        command += " " + id;
    }
#elif defined(__APPLE__)
    command = "/usr/sbin/softwareupdate --install";
    for (const auto& id : ids) command += " " + shellQuote(id);
#else
    out.error = "OS update installation is not supported on this platform.";
    out.result = {{"status", "not_supported"}};
    return out;
#endif

    const auto execution = runShellCommand(command, 3600, 2 * 1024 * 1024, "root");
    out.result = buildCommandResultJson(command, execution);
    out.result["selected_updates"] = selected;
    out.result["requested_count"] = ids.size();

    if (execution.exitCode != 0 || execution.timedOut || !execution.error.empty()) {
        out.result["status"] = "failed";
        out.result["reason"] = execution.timedOut ? "update_timeout" : "native_os_update_failed";
        out.error = execution.error.empty()
            ? "Native OS updater exited with code " + std::to_string(execution.exitCode)
            : execution.error;
        return out;
    }

    const auto after = osUpdateInventory(true);
    json remaining = json::array();
    for (const auto& id : ids) {
        if (itemForId(after, id).is_object()) remaining.push_back(id);
    }

    out.result["remaining_updates"] = remaining;
    out.result["reboot_required"] =
        rebootRequired || after.value("reboot_required", false);

    if (!remaining.empty()) {
        out.result["status"] = "failed";
        out.result["reason"] = "target_version_not_verified";
        out.error = "Native updater completed but one or more selected updates remain pending.";
        return out;
    }

    out.success = true;
    out.result["status"] = "installed";
    out.result["installed_count"] = ids.size();
    return out;
}

} // namespace hi5
