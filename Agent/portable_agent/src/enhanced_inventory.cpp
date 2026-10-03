#include "enhanced_inventory.h"
#include "platform_info.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <pwd.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace hi5 {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return trim(out.str());
}

std::string runCommand(const std::string& command, std::size_t limit = 4 * 1024 * 1024) {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
        if (output.size() >= limit) break;
    }
    pclose(pipe);
    return trim(output);
}

json commandJson(const std::string& command, std::size_t limit = 4 * 1024 * 1024) {
    const auto text = runCommand(command, limit);
    if (text.empty()) return json();
    auto parsed = json::parse(text, nullptr, false);
    return parsed.is_discarded() ? json() : parsed;
}

bool pathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

bool commandExists(const std::string& command) {
    return !runCommand("command -v " + command + " 2>/dev/null").empty();
}

std::uint64_t uintValue(const std::string& text) {
    try { return static_cast<std::uint64_t>(std::stoull(trim(text))); }
    catch (...) { return 0; }
}

std::uint64_t bytesFromHumanSize(std::string value) {
    value = trim(value);
    if (value.empty() || value == "Unknown" || value == "No Module Installed") return 0;
    std::istringstream in(value);
    double amount = 0;
    std::string unit;
    if (!(in >> amount >> unit)) return 0;
    std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    double multiplier = 1.0;
    if (unit == "KB") multiplier = 1024.0;
    else if (unit == "MB") multiplier = 1024.0 * 1024.0;
    else if (unit == "GB") multiplier = 1024.0 * 1024.0 * 1024.0;
    else if (unit == "TB") multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return static_cast<std::uint64_t>(amount * multiplier);
}

std::map<std::string,std::string> parseKeyValueBlock(const std::string& block) {
    std::map<std::string,std::string> values;
    std::istringstream in(block);
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        const auto key = trim(line.substr(0, pos));
        const auto value = trim(line.substr(pos + 1));
        if (!key.empty()) values[key] = value;
    }
    return values;
}

#if defined(__linux__)

uid_t activeUid() {
    const auto user = activeUser();
    if (user.empty()) return static_cast<uid_t>(-1);
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size < 1024) size = 16384;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    struct passwd pwd {};
    struct passwd* result = nullptr;
    if (getpwnam_r(user.c_str(), &pwd, buffer.data(), buffer.size(), &result) != 0 || !result) {
        return static_cast<uid_t>(-1);
    }
    return pwd.pw_uid;
}

std::string activeSessionType() {
    const auto user = activeUser();
    if (user.empty() || !commandExists("loginctl")) return {};
    const auto session = runCommand(
        "loginctl list-sessions --no-legend 2>/dev/null | "
        "awk '$3==\"" + user + "\" && ($5==\"seat0\" || $4==\"seat0\") {print $1; exit} "
        "$3==\"" + user + "\" {fallback=$1} END {if (fallback) print fallback}' | head -1");
    if (session.empty()) return {};
    auto type = runCommand("loginctl show-session " + session + " -p Type --value 2>/dev/null");
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return type;
}

json linuxRemoteCapabilities() {
    const auto sessionType = activeSessionType();
    const auto uid = activeUid();
    const fs::path runtime = uid == static_cast<uid_t>(-1)
        ? fs::path()
        : fs::path("/run/user") / std::to_string(uid);

    bool waylandSocket = false;
    bool pipewireSocket = false;
    if (!runtime.empty() && pathExists(runtime)) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(runtime, ec)) {
            if (ec) break;
            const auto name = entry.path().filename().string();
            if (name.rfind("wayland-", 0) == 0) waylandSocket = true;
            if (name == "pipewire-0") pipewireSocket = true;
        }
    }

    bool x11Socket = false;
    const fs::path x11Dir("/tmp/.X11-unix");
    if (pathExists(x11Dir)) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(x11Dir, ec)) {
            if (ec) break;
            if (entry.path().filename().string().rfind("X", 0) == 0) {
                x11Socket = true;
                break;
            }
        }
    }

    const bool portalInstalled =
        commandExists("xdg-desktop-portal") ||
        pathExists("/usr/share/dbus-1/services/org.freedesktop.portal.Desktop.service") ||
        pathExists("/usr/share/dbus-1/services/org.freedesktop.portal.ScreenCast.service");
    const bool pipewireInstalled =
        commandExists("pipewire") || commandExists("pw-cli") ||
        pathExists("/usr/lib/x86_64-linux-gnu/libpipewire-0.3.so.0") ||
        pathExists("/usr/lib64/libpipewire-0.3.so.0");
    const bool libeiInstalled =
        pathExists("/usr/lib/x86_64-linux-gnu/libei.so.1") ||
        pathExists("/usr/lib64/libei.so.1") ||
        !runCommand("ldconfig -p 2>/dev/null | grep -m1 'libei\.so'").empty();

    const bool wayland = sessionType == "wayland" || waylandSocket;
    const bool x11 = sessionType == "x11" || (!wayland && x11Socket);
    const bool graphical = wayland || x11;
    const bool waylandReady = wayland && portalInstalled && (pipewireSocket || pipewireInstalled);

    std::string backend = "none";
    if (wayland) backend = "wayland_portal";
    else if (x11) backend = "x11";

    return {
        {"available", (waylandReady || x11)},
        {"backend", backend},
        {"session_type", sessionType.empty() ? (wayland ? "wayland" : (x11 ? "x11" : "none")) : sessionType},
        {"desktop_session", graphical},
        {"headless", !graphical},
        {"management_only", !graphical},
        {"attended_supported", waylandReady || x11},
        {"unattended_supported", x11},
        {"requires_user_session", wayland || x11},
        {"requires_user_consent", wayland},
        {"screen_capture", {
            {"available", wayland ? waylandReady : x11},
            {"provider", wayland ? "xdg-desktop-portal+pipewire" : (x11 ? "x11" : "none")},
            {"requires_user_consent", wayland}
        }},
        {"input_control", {
            {"available", wayland ? (waylandReady && (libeiInstalled || portalInstalled)) : x11},
            {"provider", wayland ? (libeiInstalled ? "xdg-remote-desktop+eis" : "xdg-remote-desktop") : (x11 ? "x11" : "none")},
            {"requires_user_consent", wayland}
        }},
        {"wayland", {
            {"detected", wayland},
            {"portal_installed", portalInstalled},
            {"pipewire_installed", pipewireInstalled},
            {"pipewire_session_socket", pipewireSocket},
            {"eis_available", libeiInstalled}
        }},
        {"x11", {
            {"detected", x11},
            {"socket_available", x11Socket},
            {"unattended_supported", x11}
        }}
    };
}

json linuxMemoryModules() {
    json modules = json::array();
    if (!commandExists("dmidecode")) return modules;
    const auto text = runCommand("dmidecode -t 17 2>/dev/null");
    if (text.empty()) return modules;

    std::istringstream input(text);
    std::string line;
    std::string block;
    auto flush = [&]() {
        if (block.empty()) return;
        auto values = parseKeyValueBlock(block);
        block.clear();
        const auto size = values["Size"];
        if (size.empty() || size == "No Module Installed") return;
        modules.push_back({
            {"locator", values["Locator"]},
            {"bank_locator", values["Bank Locator"]},
            {"capacity_bytes", bytesFromHumanSize(size)},
            {"manufacturer", values["Manufacturer"]},
            {"part_number", values["Part Number"]},
            {"serial_number", values["Serial Number"]},
            {"memory_type", values["Type"]},
            {"form_factor", values["Form Factor"]},
            {"speed", values["Configured Memory Speed"].empty() ? values["Speed"] : values["Configured Memory Speed"]}
        });
    };
    while (std::getline(input, line)) {
        if (line.find("Memory Device") != std::string::npos && !block.empty()) flush();
        if (!trim(line).empty()) block += line + "\n";
    }
    flush();
    return modules;
}

json linuxPhysicalDisks() {
    json disks = json::array();
    if (!commandExists("lsblk")) return disks;
    auto root = commandJson(
        "lsblk -J -b -e 7 -o NAME,KNAME,PATH,TYPE,SIZE,MODEL,SERIAL,TRAN,ROTA,RM,VENDOR,FSTYPE,MOUNTPOINTS,UUID 2>/dev/null");
    if (!root.is_object() || !root.contains("blockdevices") || !root["blockdevices"].is_array()) return disks;
    for (const auto& item : root["blockdevices"]) {
        if (!item.is_object() || item.value("type", "") != "disk") continue;
        json disk = {
            {"name", item.value("name", "")},
            {"device", item.value("path", "")},
            {"model", item.value("model", "")},
            {"vendor", item.value("vendor", "")},
            {"serial_number", item.value("serial", "")},
            {"transport", item.value("tran", "")},
            {"size_bytes", item.value("size", 0ULL)},
            {"rotational", item.value("rota", false)},
            {"removable", item.value("rm", false)},
            {"partitions", json::array()}
        };
        if (item.contains("children") && item["children"].is_array()) {
            for (const auto& child : item["children"]) {
                if (!child.is_object()) continue;
                disk["partitions"].push_back({
                    {"name", child.value("name", "")},
                    {"device", child.value("path", "")},
                    {"type", child.value("type", "")},
                    {"size_bytes", child.value("size", 0ULL)},
                    {"filesystem", child.value("fstype", "")},
                    {"uuid", child.value("uuid", "")},
                    {"mountpoints", child.contains("mountpoints") ? child["mountpoints"] : json::array()}
                });
            }
        }
        disks.push_back(std::move(disk));
    }
    return disks;
}

json linuxGpus() {
    json gpus = json::array();
    if (commandExists("lspci")) {
        const auto text = runCommand("lspci -Dnn 2>/dev/null | grep -Ei 'VGA compatible controller|3D controller|Display controller'");
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (!line.empty()) gpus.push_back({{"name", line}, {"source", "lspci"}});
        }
    }
    if (!gpus.empty()) return gpus;

    std::error_code ec;
    const fs::path drm("/sys/class/drm");
    if (!pathExists(drm)) return gpus;
    for (const auto& entry : fs::directory_iterator(drm, ec)) {
        if (ec) break;
        const auto name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        const auto device = entry.path() / "device";
        if (!pathExists(device)) continue;
        gpus.push_back({
            {"name", name},
            {"vendor_id", readFile(device / "vendor")},
            {"device_id", readFile(device / "device")},
            {"driver", runCommand("basename $(readlink -f '" + (device / "driver").string() + "') 2>/dev/null")}
        });
    }
    return gpus;
}

json linuxMonitors() {
    json monitors = json::array();
    std::error_code ec;
    const fs::path drm("/sys/class/drm");
    if (!pathExists(drm)) return monitors;
    for (const auto& entry : fs::directory_iterator(drm, ec)) {
        if (ec) break;
        const auto name = entry.path().filename().string();
        if (name.find('-') == std::string::npos) continue;
        const auto status = readFile(entry.path() / "status");
        if (status != "connected") continue;
        std::string resolution;
        std::ifstream modes(entry.path() / "modes");
        std::getline(modes, resolution);
        monitors.push_back({
            {"name", name},
            {"connector", name},
            {"connected", true},
            {"resolution", trim(resolution)}
        });
    }
    return monitors;
}

json linuxBattery() {
    json battery = {{"present", false}};
    const fs::path power("/sys/class/power_supply");
    if (!pathExists(power)) return battery;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(power, ec)) {
        if (ec) break;
        const auto name = entry.path().filename().string();
        if (name.rfind("BAT", 0) != 0) continue;
        const auto base = entry.path();
        const auto capacity = uintValue(readFile(base / "capacity"));
        const auto energyDesign = uintValue(readFile(base / "energy_full_design"));
        const auto energyFull = uintValue(readFile(base / "energy_full"));
        const auto energyNow = uintValue(readFile(base / "energy_now"));
        const auto voltageUv = uintValue(readFile(base / "voltage_now"));
        const auto chargeDesign = uintValue(readFile(base / "charge_full_design"));
        const auto chargeFull = uintValue(readFile(base / "charge_full"));
        const auto chargeNow = uintValue(readFile(base / "charge_now"));
        auto toMwh = [&](std::uint64_t energyUwh, std::uint64_t chargeUah) -> std::uint64_t {
            if (energyUwh) return energyUwh / 1000ULL;
            if (chargeUah && voltageUv) return (chargeUah * voltageUv) / 1000000000ULL;
            return 0;
        };
        const auto designMwh = toMwh(energyDesign, chargeDesign);
        const auto fullMwh = toMwh(energyFull, chargeFull);
        const auto nowMwh = toMwh(energyNow, chargeNow);
        const auto status = readFile(base / "status");
        battery = {
            {"present", true},
            {"name", readFile(base / "model_name")},
            {"manufacturer", readFile(base / "manufacturer")},
            {"serial_number", readFile(base / "serial_number")},
            {"technology", readFile(base / "technology")},
            {"charge_percent", capacity},
            {"status", status},
            {"charging", status == "Charging"},
            {"discharging", status == "Discharging"},
            {"design_capacity_mwh", designMwh},
            {"full_charge_capacity_mwh", fullMwh},
            {"remaining_capacity_mwh", nowMwh},
            {"voltage_mv", voltageUv / 1000ULL},
            {"cycle_count", uintValue(readFile(base / "cycle_count"))}
        };
        if (designMwh && fullMwh) {
            const auto health = std::min(100.0, (static_cast<double>(fullMwh) * 100.0) / static_cast<double>(designMwh));
            battery["health_percent"] = health;
            battery["wear_percent"] = std::max(0.0, 100.0 - health);
        }
        return battery;
    }
    return battery;
}

json linuxEnhancedInventory() {
    const auto remote = linuxRemoteCapabilities();
    const auto gpus = linuxGpus();
    const auto monitors = linuxMonitors();
    const auto memoryModules = linuxMemoryModules();
    const auto disks = linuxPhysicalDisks();

    const json motherboard = {
        {"manufacturer", readFile("/sys/class/dmi/id/board_vendor")},
        {"product", readFile("/sys/class/dmi/id/board_name")},
        {"serial_number", readFile("/sys/class/dmi/id/board_serial")},
        {"version", readFile("/sys/class/dmi/id/board_version")},
        {"bios_manufacturer", readFile("/sys/class/dmi/id/bios_vendor")},
        {"bios_version", readFile("/sys/class/dmi/id/bios_version")},
        {"bios_date", readFile("/sys/class/dmi/id/bios_date")},
        {"smbios_version", readFile("/sys/class/dmi/id/bios_release")}
    };

    json cpu = json::object();
    const auto lscpu = commandJson("lscpu -J 2>/dev/null");
    if (lscpu.is_object() && lscpu.contains("lscpu") && lscpu["lscpu"].is_array()) {
        for (const auto& row : lscpu["lscpu"]) {
            if (!row.is_object()) continue;
            auto key = row.value("field", "");
            if (!key.empty() && key.back() == ':') key.pop_back();
            cpu[key] = row.value("data", "");
        }
    }

    const auto virtText = runCommand("systemd-detect-virt 2>/dev/null");
    const auto sockets = uintValue(cpu.value("Socket(s)", "0"));
    const auto coresPerSocket = uintValue(cpu.value("Core(s) per socket", "0"));
    const auto logicalProcessors = uintValue(cpu.value("CPU(s)", "0"));
    json cpuDetail = {
        {"name", cpu.value("Model name", cpuModel())},
        {"vendor", cpu.value("Vendor ID", "")},
        {"cores", sockets && coresPerSocket ? sockets * coresPerSocket : 0},
        {"sockets", sockets},
        {"logical_processors", logicalProcessors},
        {"max_clock_mhz", cpu.value("CPU max MHz", "")},
        {"virtualization", cpu.value("Virtualization", "")}
    };

    const json hardware = {
        {"manufacturer", manufacturer()},
        {"model", model()},
        {"serial_number", serialNumber()},
        {"device_uuid", readFile("/sys/class/dmi/id/product_uuid")},
        {"sku", readFile("/sys/class/dmi/id/product_sku")},
        {"system_family", readFile("/sys/class/dmi/id/product_family")},
        {"bios_date", readFile("/sys/class/dmi/id/bios_date")}
    };

    return {
        {"hardware", hardware},
        {"cpu_detail", cpuDetail},
        {"motherboard", motherboard},
        {"memory_modules", memoryModules},
        {"physical_disks", disks},
        {"gpu", gpus},
        {"monitors", monitors},
        {"displays", {{"gpus", gpus}, {"monitors", monitors}}},
        {"battery", linuxBattery()},
        {"virtualization", {
            {"detected", !virtText.empty() && virtText != "none"},
            {"type", virtText == "none" ? "" : virtText}
        }},
        {"cpu_detail", cpu},
        {"remote_desktop", remote},
        {"deep_inventory_sections", {
            {"core_hardware", {{"status", "collected"}}},
            {"storage", {{"status", "collected"}}},
            {"monitors", {{"status", "collected"}}},
            {"battery", {{"status", "collected"}}},
            {"remote_desktop", {{"status", "collected"}}}
        }}
    };
}

#endif

#if defined(__APPLE__)

json macSystemProfiler() {
    return commandJson(
        "/usr/sbin/system_profiler SPHardwareDataType SPDisplaysDataType SPPowerDataType SPNVMeDataType SPSerialATADataType -json -detailLevel mini 2>/dev/null",
        8 * 1024 * 1024);
}

json macRemoteCapabilities() {
    return {
        {"available", true},
        {"backend", "macos_screencapturekit"},
        {"session_type", "aqua"},
        {"desktop_session", !activeUser().empty()},
        {"headless", activeUser().empty()},
        {"management_only", false},
        {"attended_supported", true},
        {"unattended_supported", true},
        {"requires_user_session", true},
        {"requires_user_consent", true},
        {"requires_initial_screen_recording_permission", true},
        {"requires_initial_accessibility_permission", true},
        {"screen_capture", {
            {"available", true},
            {"provider", "ScreenCaptureKit"},
            {"permission", "Screen Recording"}
        }},
        {"input_control", {
            {"available", true},
            {"provider", "CGEvent/Accessibility"},
            {"permission", "Accessibility"}
        }}
    };
}

void appendMacStorage(const json& root, const char* key, json& disks) {
    auto it = root.find(key);
    if (it == root.end() || !it->is_array()) return;
    for (const auto& controller : *it) {
        if (!controller.is_object()) continue;
        json disk = {
            {"name", controller.value("_name", controller.value("sppci_model", ""))},
            {"model", controller.value("device_model", controller.value("_name", ""))},
            {"vendor", controller.value("device_manufacturer", "")},
            {"serial_number", controller.value("device_serial", "")},
            {"size", controller.value("size", "")},
            {"protocol", controller.value("physical_interconnect", "")},
            {"source", key}
        };
        if (controller.contains("_items") && controller["_items"].is_array()) {
            disk["items"] = controller["_items"];
        }
        disks.push_back(std::move(disk));
    }
}

json macEnhancedInventory() {
    const auto profiler = macSystemProfiler();
    json hardware = json::object();
    if (profiler.is_object()) {
        auto hit = profiler.find("SPHardwareDataType");
        if (hit != profiler.end() && hit->is_array() && !hit->empty() && (*hit)[0].is_object()) {
            hardware = (*hit)[0];
        }
    }

    json gpus = json::array();
    json monitors = json::array();
    if (profiler.is_object()) {
        auto dit = profiler.find("SPDisplaysDataType");
        if (dit != profiler.end() && dit->is_array()) {
            for (const auto& gpu : *dit) {
                if (!gpu.is_object()) continue;
                gpus.push_back({
                    {"name", gpu.value("sppci_model", gpu.value("_name", ""))},
                    {"vendor", gpu.value("spdisplays_vendor", "")},
                    {"vram", gpu.value("spdisplays_vram", gpu.value("spdisplays_vram_shared", ""))},
                    {"metal_support", gpu.value("spdisplays_metal", "")}
                });
                auto displayIt = gpu.find("spdisplays_ndrvs");
                if (displayIt != gpu.end() && displayIt->is_array()) {
                    for (const auto& display : *displayIt) {
                        if (!display.is_object()) continue;
                        monitors.push_back({
                            {"name", display.value("_name", "")},
                            {"resolution", display.value("_spdisplays_resolution", display.value("spdisplays_resolution", ""))},
                            {"main", display.value("spdisplays_main", "") == "spdisplays_yes"},
                            {"online", display.value("spdisplays_online", "") != "spdisplays_no"},
                            {"connection_type", display.value("spdisplays_connection_type", "")}
                        });
                    }
                }
            }
        }
    }

    json disks = json::array();
    if (profiler.is_object()) {
        appendMacStorage(profiler, "SPNVMeDataType", disks);
        appendMacStorage(profiler, "SPSerialATADataType", disks);
    }

    json battery = {{"present", false}};
    if (profiler.is_object()) {
        auto pit = profiler.find("SPPowerDataType");
        if (pit != profiler.end() && pit->is_array() && !pit->empty()) {
            const auto& power = (*pit)[0];
            if (power.is_object()) {
                auto bit = power.find("sppower_battery_health_info");
                auto cit = power.find("sppower_battery_charge_info");
                battery["present"] = bit != power.end() || cit != power.end();
                if (bit != power.end() && bit->is_object()) {
                    battery["cycle_count"] = bit->value("sppower_battery_cycle_count", "");
                    battery["health"] = bit->value("sppower_battery_health", "");
                    battery["condition"] = bit->value("sppower_battery_condition", "");
                }
                if (cit != power.end() && cit->is_object()) {
                    battery["charge_percent"] = cit->value("sppower_battery_charge_remaining", "");
                    battery["charging"] = cit->value("sppower_battery_is_charging", "") == "TRUE";
                }
            }
        }
    }

    const auto totalMemory = runCommand("/usr/sbin/sysctl -n hw.memsize 2>/dev/null");
    json memoryModules = json::array();
    if (!totalMemory.empty()) {
        memoryModules.push_back({
            {"locator", "Unified/System Memory"},
            {"capacity_bytes", uintValue(totalMemory)},
            {"memory_type", hardware.value("chip_type", "").empty() ? "System memory" : "Unified memory"},
            {"manufacturer", "Apple"}
        });
    }

    const json motherboard = {
        {"manufacturer", "Apple Inc."},
        {"product", hardware.value("machine_model", "")},
        {"serial_number", hardware.value("serial_number", "")},
        {"version", hardware.value("machine_name", "")},
        {"bios_version", hardware.value("boot_rom_version", "")},
        {"smbios_version", ""}
    };

    const json hardwareDetail = {
        {"manufacturer", "Apple Inc."},
        {"model", hardware.value("machine_model", model())},
        {"serial_number", hardware.value("serial_number", serialNumber())},
        {"device_uuid", hardware.value("platform_UUID", "")},
        {"sku", ""},
        {"system_family", hardware.value("machine_name", "")},
        {"bios_date", ""}
    };

    const json cpuDetail = {
        {"name", hardware.value("chip_type", cpuModel())},
        {"vendor", "Apple"},
        {"cores", hardware.value("total_number_cores", "")},
        {"sockets", hardware.value("number_processors", "")},
        {"logical_processors", hardware.value("total_number_cores", "")},
        {"max_clock_mhz", ""},
        {"virtualization", ""}
    };

    return {
        {"hardware", hardwareDetail},
        {"cpu_detail", cpuDetail},
        {"motherboard", motherboard},
        {"memory_modules", memoryModules},
        {"physical_disks", disks},
        {"gpu", gpus},
        {"monitors", monitors},
        {"displays", {{"gpus", gpus}, {"monitors", monitors}}},
        {"battery", battery},
        {"virtualization", {
            {"detected", model().find("Virtual") != std::string::npos || model().find("VMware") != std::string::npos},
            {"type", model().find("VMware") != std::string::npos ? "vmware" : ""}
        }},
        {"remote_desktop", macRemoteCapabilities()},
        {"deep_inventory_sections", {
            {"core_hardware", {{"status", "collected"}}},
            {"storage", {{"status", "collected"}}},
            {"monitors", {{"status", "collected"}}},
            {"battery", {{"status", "collected"}}},
            {"remote_desktop", {{"status", "collected"}}}
        }}
    };
}

#endif

} // namespace

nlohmann::json remoteDesktopCapabilities() {
#if defined(__APPLE__)
    return macRemoteCapabilities();
#else
    return linuxRemoteCapabilities();
#endif
}

nlohmann::json enhancedHardwareInventory() {
#if defined(__APPLE__)
    return macEnhancedInventory();
#else
    return linuxEnhancedInventory();
#endif
}

} // namespace hi5