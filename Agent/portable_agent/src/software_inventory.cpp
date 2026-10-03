#include "software_inventory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace hi5 {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string runCommand(const std::string& command) {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
        if (output.size() > 32 * 1024 * 1024) break;
    }
    pclose(pipe);
    return output;
}

std::vector<std::string> tabs(const std::string& line) {
    std::vector<std::string> values;
    std::size_t start = 0;
    for (;;) {
        const auto pos = line.find('\t', start);
        if (pos == std::string::npos) {
            values.push_back(line.substr(start));
            return values;
        }
        values.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
}

void sortItems(json& items) {
    std::sort(items.begin(), items.end(), [](const json& a, const json& b) {
        return a.value("name", "") < b.value("name", "");
    });
}

#if defined(__linux__)
void collectDpkg(json& items) {
    const auto output = runCommand(
        "dpkg-query -W -f='\${Package}\\t\${Version}\\t\${Maintainer}\\t\${Installed-Size}\\t\${Architecture}\\n' 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto f = tabs(line);
        if (f.size() < 5 || trim(f[0]).empty()) continue;
        std::uint64_t sizeKb = 0;
        try { sizeKb = std::stoull(trim(f[3])); } catch (...) {}
        const auto name = trim(f[0]);
        const auto arch = trim(f[4]);
        items.push_back({
            {"name", name},
            {"version", trim(f[1])},
            {"publisher", trim(f[2])},
            {"install_date", ""},
            {"install_location", "/"},
            {"estimated_size_kb", sizeKb},
            {"scope", "system"},
            {"registry_key", "dpkg:" + name + ":" + arch},
            {"package_manager", "apt"},
            {"package_id", name},
            {"architecture", arch},
            {"native_actionable", true},
            {"update_available", false},
            {"latest_version", ""}
        });
    }
}

void collectSnap(json& items) {
    if (!fs::exists("/usr/bin/snap")) return;
    const auto output = runCommand("snap list --unicode=never 2>/dev/null | tail -n +2");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string name, version, revision, tracking, publisher;
        if (!(row >> name >> version >> revision >> tracking >> publisher)) continue;
        items.push_back({
            {"name", name}, {"version", version}, {"publisher", publisher},
            {"install_date", ""}, {"install_location", "/snap/" + name},
            {"estimated_size_kb", nullptr}, {"scope", "system"},
            {"registry_key", "snap:" + name}, {"package_manager", "snap"},
            {"package_id", name}, {"channel", tracking},
            {"native_actionable", true}, {"update_available", false},
            {"latest_version", ""}
        });
    }
}

void collectFlatpak(json& items) {
    if (!fs::exists("/usr/bin/flatpak")) return;
    const auto output = runCommand(
        "flatpak list --app --columns=application,name,version,origin,installation 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto f = tabs(line);
        if (f.size() < 5 || trim(f[0]).empty()) continue;
        const auto id = trim(f[0]);
        const auto install = trim(f[4]);
        items.push_back({
            {"name", trim(f[1]).empty() ? id : trim(f[1])},
            {"version", trim(f[2])}, {"publisher", trim(f[3])},
            {"install_date", ""}, {"install_location", ""},
            {"estimated_size_kb", nullptr},
            {"scope", install == "user" ? "user" : "system"},
            {"registry_key", "flatpak:" + id + ":" + install},
            {"package_manager", "flatpak"}, {"package_id", id},
            {"native_actionable", true}, {"update_available", false},
            {"latest_version", ""}
        });
    }
}

void applyAptUpdates(json& items) {
    const auto output = runCommand("apt list --upgradable 2>/dev/null");
    std::map<std::string, std::string> latest;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line.rfind("Listing...", 0) == 0) continue;

        const auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        const std::string packageId = line.substr(0, slash);

        const auto firstSpace = line.find(' ', slash + 1);
        if (firstSpace == std::string::npos) continue;
        const auto secondSpace = line.find(' ', firstSpace + 1);
        const std::string version = trim(
            line.substr(firstSpace + 1, secondSpace == std::string::npos
                ? std::string::npos
                : secondSpace - firstSpace - 1));
        if (!packageId.empty() && !version.empty()) latest[packageId] = version;
    }

    for (auto& item : items) {
        if (!item.is_object() || item.value("package_manager", "") != "apt") continue;
        const auto packageId = item.value("package_id", "");
        const auto found = latest.find(packageId);
        if (found == latest.end()) continue;
        item["update_available"] = true;
        item["latest_version"] = found->second;
    }
}

json linuxInventory() {
    json items = json::array();
    collectDpkg(items);
    collectSnap(items);
    collectFlatpak(items);
    applyAptUpdates(items);
    sortItems(items);
    return {
        {"status", "collected"}, {"count", items.size()},
        {"installed_apps", items.size()}, {"items", items},
        {"recently_installed", json::array()}, {"recently_installed_count", 0},
        {"providers", {
            {"dpkg", true},
            {"snap", fs::exists("/usr/bin/snap")},
            {"flatpak", fs::exists("/usr/bin/flatpak")}
        }}
    };
}
#endif

#if defined(__APPLE__)
json macApplications() {
    json items = json::array();
    const auto output = runCommand(
        "/usr/sbin/system_profiler SPApplicationsDataType -json -detailLevel mini 2>/dev/null");
    const auto root = json::parse(output, nullptr, false);
    if (!root.is_object()) return items;
    const auto it = root.find("SPApplicationsDataType");
    if (it == root.end() || !it->is_array()) return items;

    for (const auto& app : *it) {
        if (!app.is_object()) continue;
        const auto name = trim(app.value("_name", ""));
        if (name.empty()) continue;
        const auto path = trim(app.value("path", ""));
        const auto source = trim(app.value("obtained_from", ""));
        std::string publisher = source;
        if (source == "apple") publisher = "Apple";
        else if (source == "identified_developer") publisher = "Identified Developer";
        const auto identifier = !path.empty() ? path : name;
        items.push_back({
            {"name", name}, {"version", trim(app.value("version", ""))},
            {"publisher", publisher},
            {"install_date", trim(app.value("lastModified", ""))},
            {"install_location", path}, {"estimated_size_kb", nullptr},
            {"scope", path.rfind("/Users/", 0) == 0 ? "user" : "system"},
            {"registry_key", "app:" + identifier},
            {"package_manager", "app_bundle"}, {"package_id", identifier},
            {"native_actionable", false}, {"update_available", false},
            {"latest_version", ""}
        });
    }
    return items;
}

json macInventory() {
    json items = macApplications();
    sortItems(items);
    return {
        {"status", "collected"}, {"count", items.size()},
        {"installed_apps", items.size()}, {"items", items},
        {"recently_installed", json::array()}, {"recently_installed_count", 0},
        {"providers", {{"applications", true}}}
    };
}
#endif

} // namespace

json softwareInventory() {
#if defined(__APPLE__)
    return macInventory();
#elif defined(__linux__)
    return linuxInventory();
#else
    return {{"status","unsupported"},{"count",0},{"installed_apps",0},{"items",json::array()}};
#endif
}

} // namespace hi5
