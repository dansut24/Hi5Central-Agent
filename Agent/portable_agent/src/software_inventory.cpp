#include "software_inventory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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

std::string baseSection(std::string section) {
    section = lower(trim(section));
    const auto slash = section.rfind('/');
    if (slash != std::string::npos) section = section.substr(slash + 1);
    return section;
}

bool desktopFileVisible(const std::string& path) {
    std::ifstream input(path);
    if (!input) return false;
    bool inDesktopEntry = false;
    bool application = false;
    bool hidden = false;
    bool noDisplay = false;
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            inDesktopEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inDesktopEntry) continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const auto key = trim(line.substr(0, pos));
        const auto value = lower(trim(line.substr(pos + 1)));
        if (key == "Type") application = value == "application";
        else if (key == "Hidden") hidden = value == "true";
        else if (key == "NoDisplay") noDisplay = value == "true";
    }
    return application && !hidden && !noDisplay;
}

std::set<std::string> initialOsPackages() {
    std::set<std::string> packages;
    const auto output = runCommand(
        "(gzip -dc /var/log/installer/initial-status.gz 2>/dev/null || "
        "cat /var/log/installer/status 2>/dev/null) | "
        "awk '$1==\"Package:\" {print $2}'");
    std::istringstream stream(output);
    std::string package;
    while (std::getline(stream, package)) {
        package = trim(package);
        if (!package.empty()) packages.insert(package);
    }
    return packages;
}

std::set<std::string> visibleDesktopPackageOwners() {
    std::set<std::string> owners;
    const auto output = runCommand(
        "dpkg-query -S '/usr/share/applications/*' '/usr/local/share/applications/*' 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto marker = line.find(": /");
        if (marker == std::string::npos) continue;
        const std::string ownerList = line.substr(0, marker);
        const std::string path = line.substr(marker + 2);
        if (!desktopFileVisible(path)) continue;
        std::size_t start = 0;
        while (start < ownerList.size()) {
            const auto comma = ownerList.find(',', start);
            auto owner = trim(ownerList.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            const auto archColon = owner.rfind(':');
            if (archColon != std::string::npos) {
                const auto suffix = owner.substr(archColon + 1);
                if (suffix == "amd64" || suffix == "i386" || suffix == "arm64" || suffix == "armhf" || suffix == "all") owner = owner.substr(0, archColon);
            }
            if (!owner.empty()) owners.insert(owner);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return owners;
}

std::set<std::string> visibleDesktopRpmOwners() {
    std::set<std::string> owners;
    const auto output = runCommand(
        "for f in /usr/share/applications/*.desktop /usr/local/share/applications/*.desktop; do "
        "[ -f \"$f\" ] || continue; "
        "owner=$(rpm -qf \"$f\" --qf '%{NAME}' 2>/dev/null) || continue; "
        "printf '%s\\t%s\\n' \"$owner\" \"$f\"; done");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto f = tabs(line);
        if (f.size() < 2) continue;
        const auto owner = trim(f[0]);
        const auto path = trim(f[1]);
        if (owner.empty() || path.empty() || !desktopFileVisible(path)) continue;
        owners.insert(owner);
    }
    return owners;
}

bool criticalRpmComponent(const std::string& packageName) {
    const auto name = lower(packageName);
    static const char* protectedPrefixes[] = {
        "kernel", "systemd", "glibc", "rpm", "dnf", "libdnf", "fedora-release",
        "fedora-repos", "grub", "shim", "dracut", "selinux", "policycoreutils",
        "filesystem", "setup", "bash", "coreutils", "util-linux", "dbus",
        "NetworkManager", "linux-firmware", "crypto-policies"
    };
    for (const auto* raw : protectedPrefixes) {
        const auto prefix = lower(raw);
        if (name == prefix || name.rfind(prefix + "-", 0) == 0) return true;
    }
    return false;
}

bool criticalDpkgComponent(const std::string& packageName, const std::string& section, const std::string& priority, const std::string& essential) {
    const auto normalizedSection = baseSection(section);
    const auto normalizedPriority = lower(trim(priority));
    const auto normalizedEssential = lower(trim(essential));
    const auto name = lower(packageName);
    if (normalizedEssential == "yes") return true;
    if (normalizedPriority == "required" || normalizedPriority == "important") return true;
    static const std::set<std::string> protectedSections = {
        "kernel", "libs", "libdevel", "oldlibs", "debug", "introspection",
        "metapackages", "localization", "translations", "fonts"
    };
    if (protectedSections.count(normalizedSection)) return true;
    static const char* protectedPrefixes[] = {
        "linux-image", "linux-headers", "linux-modules", "linux-firmware",
        "libc", "systemd", "udev", "dbus", "dpkg", "apt", "init",
        "grub", "shim", "base-files", "base-passwd", "coreutils",
        "util-linux", "mount", "login", "passwd"
    };
    for (const auto* prefix : protectedPrefixes) {
        if (name == prefix || name.rfind(std::string(prefix) + "-", 0) == 0) return true;
    }
    return false;
}

bool protectedManagedProduct(const std::string& packageName) {
    const auto name = lower(packageName);
    static const char* protectedTerms[] = {
        "hi5central", "crowdstrike", "falcon-sensor", "sentinelone",
        "sophos", "bitdefender", "cylance", "carbonblack", "huntress"
    };
    for (const auto* term : protectedTerms) if (name.find(term) != std::string::npos) return true;
    return false;
}
void collectDpkg(json& items) {
    const auto desktopOwners = visibleDesktopPackageOwners();
    const auto initialPackages = initialOsPackages();
    const auto output = runCommand(
        "dpkg-query -W -f='${Package}\\t${Version}\\t${Maintainer}\\t${Installed-Size}\\t${Architecture}\\t${Section}\\t${Priority}\\t${Essential}\\n' 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto f = tabs(line);
        if (f.size() < 8 || trim(f[0]).empty()) continue;
        std::uint64_t sizeKb = 0;
        try { sizeKb = std::stoull(trim(f[3])); } catch (...) {}
        const auto name = trim(f[0]);
        const auto arch = trim(f[4]);
        const auto section = trim(f[5]);
        const auto priority = trim(f[6]);
        const auto essential = trim(f[7]);
        const bool desktopApplication = desktopOwners.count(name) > 0;
        const bool initialOsComponent = initialPackages.count(name) > 0;
        const bool criticalComponent = criticalDpkgComponent(name, section, priority, essential);
        const bool protectedProduct = protectedManagedProduct(name);
        // A package being present in the distribution's initial image does not make it
        // an OS component. User-facing applications such as Firefox, 7-Zip and
        // LibreOffice are commonly preinstalled by desktop distributions. Keep them
        // visible while critical/runtime packages remain protected below.
        const bool visibleApplication = desktopApplication && !criticalComponent;
        const bool systemComponent = !visibleApplication;
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
            {"section", section},
            {"priority", priority},
            {"essential", lower(essential) == "yes"},
            {"classification", visibleApplication ? "application" : "system_component"},
            {"classification_reason", criticalComponent ? "critical_os_component" : (desktopApplication ? (initialOsComponent ? "desktop_application_initial_os" : "desktop_application") : (initialOsComponent ? "initial_os_install" : "non_application_package"))},
            {"initial_os_install", initialOsComponent},
            {"display_in_installed_software", visibleApplication},
            {"system_component", systemComponent},
            {"native_actionable", visibleApplication && !protectedProduct},
            {"update_available", false},
            {"latest_version", ""}
        });
    }
}

void collectRpm(json& items) {
    if (!fs::exists("/usr/bin/rpm")) return;
    const auto desktopOwners = visibleDesktopRpmOwners();
    const auto output = runCommand(
        "rpm -qa --qf '%{NAME}\\t%{VERSION}-%{RELEASE}\\t%{VENDOR}\\t%{SIZE}\\t%{ARCH}\\t%{GROUP}\\n' 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto f = tabs(line);
        if (f.size() < 6 || trim(f[0]).empty()) continue;
        const auto name = trim(f[0]);
        const auto arch = trim(f[4]);
        std::uint64_t sizeBytes = 0;
        try { sizeBytes = std::stoull(trim(f[3])); } catch (...) {}
        const bool desktopApplication = desktopOwners.count(name) > 0;
        const bool criticalComponent = criticalRpmComponent(name);
        const bool protectedProduct = protectedManagedProduct(name);
        const bool visibleApplication = desktopApplication && !criticalComponent;
        items.push_back({
            {"name", name},
            {"version", trim(f[1])},
            {"publisher", trim(f[2])},
            {"install_date", ""},
            {"install_location", "/"},
            {"estimated_size_kb", sizeBytes ? json(sizeBytes / 1024ULL) : json(nullptr)},
            {"scope", "system"},
            {"registry_key", "rpm:" + name + ":" + arch},
            {"package_manager", "dnf"},
            {"package_id", name},
            {"architecture", arch},
            {"group", trim(f[5])},
            {"classification", visibleApplication ? "application" : "system_component"},
            {"classification_reason", criticalComponent ? "critical_os_component" : (desktopApplication ? "desktop_application" : "non_application_package")},
            {"display_in_installed_software", visibleApplication},
            {"system_component", !visibleApplication},
            {"native_actionable", visibleApplication && !protectedProduct},
            {"native_update_actionable", visibleApplication && !protectedProduct},
            {"native_uninstall_actionable", false},
            {"update_available", false},
            {"latest_version", ""}
        });
    }
}

void applyDnfUpdates(json& items) {
    if (!fs::exists("/usr/bin/dnf") && !fs::exists("/usr/bin/dnf5")) return;
    const std::string dnf = fs::exists("/usr/bin/dnf") ? "/usr/bin/dnf" : "/usr/bin/dnf5";
    const auto output = runCommand(dnf + " -q check-upgrade 2>/dev/null");
    std::map<std::string, std::string> latest;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line == "Upgrades" || line.rfind("Last metadata", 0) == 0) continue;
        std::istringstream row(line);
        std::string packageArch, version, repository;
        if (!(row >> packageArch >> version >> repository)) continue;
        const auto dot = packageArch.rfind('.');
        const auto packageId = dot == std::string::npos ? packageArch : packageArch.substr(0, dot);
        if (!packageId.empty() && !version.empty()) latest[packageId] = version;
    }

    for (auto& item : items) {
        if (!item.is_object() || item.value("package_manager", "") != "dnf") continue;
        const auto packageId = item.value("package_id", "");
        const auto found = latest.find(packageId);
        if (found == latest.end()) continue;
        item["update_available"] = true;
        item["latest_version"] = found->second;
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
            {"classification", "application"}, {"display_in_installed_software", true},
            {"system_component", false}, {"native_actionable", !protectedManagedProduct(name)},
            {"update_available", false}, {"latest_version", ""}
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
            {"classification", "application"}, {"display_in_installed_software", true},
            {"system_component", false}, {"native_actionable", !protectedManagedProduct(id)},
            {"update_available", false}, {"latest_version", ""}
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
    json components = json::array();
    const bool hasDpkg = fs::exists("/usr/bin/dpkg-query");
    const bool hasRpm = fs::exists("/usr/bin/rpm");
    if (hasDpkg) collectDpkg(components);
    if (hasRpm) collectRpm(components);
    collectSnap(components);
    collectFlatpak(components);
    if (hasDpkg) applyAptUpdates(components);
    if (hasRpm) applyDnfUpdates(components);
    sortItems(components);

    json applications = json::array();
    for (const auto& item : components) {
        if (item.value("display_in_installed_software", false)) {
            applications.push_back(item);
        }
    }

    return {
        {"status", "collected"},
        {"count", applications.size()},
        {"installed_apps", applications.size()},
        {"component_count", components.size()},
        {"hidden_system_components", components.size() - applications.size()},
        {"items", applications},
        {"components", components},
        {"recently_installed", json::array()},
        {"recently_installed_count", 0},
        {"providers", {
            {"dpkg", hasDpkg},
            {"rpm", hasRpm},
            {"dnf", fs::exists("/usr/bin/dnf") || fs::exists("/usr/bin/dnf5")},
            {"snap", fs::exists("/usr/bin/snap")},
            {"flatpak", fs::exists("/usr/bin/flatpak")}
        }}
    };
}
#endif

#if defined(__APPLE__)

bool pathEndsWithApp(const std::string& path) {
    return path.size() >= 4 && lower(path.substr(path.size() - 4)) == ".app";
}

bool macUserFacingApplication(const std::string& path) {
    if (!pathEndsWithApp(path)) return false;
    if (path.rfind("/Applications/", 0) == 0) return true;
    if (path.rfind("/Users/", 0) == 0 && path.find("/Applications/") != std::string::npos) return true;
    return false;
}
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
        const bool visibleApplication = macUserFacingApplication(path);
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
            {"classification", visibleApplication ? "application" : "system_component"},
            {"display_in_installed_software", visibleApplication},
            {"system_component", !visibleApplication},
            {"native_actionable", false}, {"update_available", false},
            {"latest_version", ""}
        });
    }
    return items;
}

json macInventory() {
    json components = macApplications();
    sortItems(components);

    json applications = json::array();
    for (const auto& item : components) {
        if (item.value("display_in_installed_software", false)) {
            applications.push_back(item);
        }
    }

    return {
        {"status", "collected"},
        {"count", applications.size()},
        {"installed_apps", applications.size()},
        {"component_count", components.size()},
        {"hidden_system_components", components.size() - applications.size()},
        {"items", applications},
        {"components", components},
        {"recently_installed", json::array()},
        {"recently_installed_count", 0},
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
    return {{"status","unsupported"},{"count",0},{"installed_apps",0},{"component_count",0},{"hidden_system_components",0},{"items",json::array()},{"components",json::array()}};
#endif
}

} // namespace hi5
