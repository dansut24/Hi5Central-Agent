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

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#endif

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

std::uint64_t uintValue(const std::string& text);

std::string jsonString(const json& object, const char* key) {
    if (!object.is_object()) return {};
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
    if (it->is_number_float()) return std::to_string(it->get<double>());
    return {};
}

std::uint64_t jsonUInt(const json& object, const char* key) {
    if (!object.is_object()) return 0;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return 0;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>();
    if (it->is_number_integer()) {
        const auto value = it->get<long long>();
        return value > 0 ? static_cast<std::uint64_t>(value) : 0;
    }
    if (it->is_string()) return uintValue(it->get<std::string>());
    return 0;
}

bool jsonBool(const json& object, const char* key) {
    if (!object.is_object()) return false;
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number_integer()) return it->get<long long>() != 0;
    if (it->is_string()) {
        auto value = it->get<std::string>();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return value == "1" || value == "true" || value == "yes";
    }
    return false;
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
        !runCommand("ldconfig -p 2>/dev/null | grep -m1 'libei.so'").empty();

    const bool wayland = sessionType == "wayland" || waylandSocket;
    const bool x11 = sessionType == "x11" || (!wayland && x11Socket);
    const bool graphical = wayland || x11;
    const bool waylandReady = wayland && portalInstalled && (pipewireSocket || pipewireInstalled);

    std::string backend = "none";
    if (wayland) backend = "wayland_portal";
    else if (x11) backend = "x11";

    return {
        {"available", (waylandReady || x11)},
        {"implementation_ready", x11},
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
        if (!item.is_object() || jsonString(item, "type") != "disk") continue;
        json disk = {
            {"name", jsonString(item, "name")},
            {"device", jsonString(item, "path")},
            {"model", jsonString(item, "model")},
            {"vendor", jsonString(item, "vendor")},
            {"serial_number", jsonString(item, "serial")},
            {"transport", jsonString(item, "tran")},
            {"size_bytes", jsonUInt(item, "size")},
            {"rotational", jsonBool(item, "rota")},
            {"removable", jsonBool(item, "rm")},
            {"partitions", json::array()}
        };
        if (item.contains("children") && item["children"].is_array()) {
            for (const auto& child : item["children"]) {
                if (!child.is_object()) continue;
                disk["partitions"].push_back({
                    {"name", jsonString(child, "name")},
                    {"device", jsonString(child, "path")},
                    {"type", jsonString(child, "type")},
                    {"size_bytes", jsonUInt(child, "size")},
                    {"filesystem", jsonString(child, "fstype")},
                    {"uuid", jsonString(child, "uuid")},
                    {"mountpoints", child.contains("mountpoints") && child["mountpoints"].is_array() ? child["mountpoints"] : json::array()}
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
            auto key = jsonString(row, "field");
            if (!key.empty() && key.back() == ':') key.pop_back();
            if (!key.empty()) cpu[key] = jsonString(row, "data");
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
    const bool desktopSession = !activeUser().empty();

    // TCC permissions belong to the per-user Remote Helper, not to the root
    // LaunchDaemon collecting inventory. The helper performs the authoritative
    // preflight/request when a remote session is started.
    return {
        {"available", desktopSession},
        {"implementation_ready", true},
        {"backend", "macos_screencapturekit"},
        {"codec", "adaptive"},
        {"preferred_codec", "h264_videotoolbox"},
        {"codecs", json::array({
            "h264_videotoolbox",
            "vp8_libvpx",
            "vp9_libvpx",
            "av1_libaom"
        })},
        {"session_type", "aqua"},
        {"desktop_session", desktopSession},
        {"headless", !desktopSession},
        {"management_only", !desktopSession},
        {"attended_supported", desktopSession},
        {"unattended_supported", desktopSession},
        {"backstage_supported", false},
        {"requires_user_session", true},
        {"requires_user_consent", true},
        {"permission_owner", "Hi5Central Remote Helper"},
        {"requires_initial_screen_recording_permission", true},
        {"requires_initial_accessibility_permission", true},
        {"screen_capture", {
            {"available", desktopSession},
            {"provider", "ScreenCaptureKit"},
            {"permission", "Screen Recording"},
            {"permission_state", "checked_by_user_helper"}
        }},
        {"input_control", {
            {"available", desktopSession},
            {"provider", "CGEvent/Accessibility"},
            {"permission", "Accessibility"},
            {"permission_state", "checked_by_user_helper"}
        }}
    };
}

void appendMacStorage(const json& root, const char* key, json& disks) {
    auto it = root.find(key);
    if (it == root.end() || !it->is_array()) return;
    for (const auto& controller : *it) {
        if (!controller.is_object()) continue;
        const auto controllerName = jsonString(controller, "_name");
        const auto pciModel = jsonString(controller, "sppci_model");
        json disk = {
            {"name", controllerName.empty() ? pciModel : controllerName},
            {"model", jsonString(controller, "device_model").empty() ? controllerName : jsonString(controller, "device_model")},
            {"vendor", jsonString(controller, "device_manufacturer")},
            {"serial_number", jsonString(controller, "device_serial")},
            {"size", jsonString(controller, "size")},
            {"protocol", jsonString(controller, "physical_interconnect")},
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
                const auto gpuModel = jsonString(gpu, "sppci_model");
                const auto gpuName = jsonString(gpu, "_name");
                const auto vram = jsonString(gpu, "spdisplays_vram");
                gpus.push_back({
                    {"name", gpuModel.empty() ? gpuName : gpuModel},
                    {"vendor", jsonString(gpu, "spdisplays_vendor")},
                    {"vram", vram.empty() ? jsonString(gpu, "spdisplays_vram_shared") : vram},
                    {"metal_support", jsonString(gpu, "spdisplays_metal")}
                });
                auto displayIt = gpu.find("spdisplays_ndrvs");
                if (displayIt != gpu.end() && displayIt->is_array()) {
                    for (const auto& display : *displayIt) {
                        if (!display.is_object()) continue;
                        const auto primaryResolution = jsonString(display, "_spdisplays_resolution");
                        monitors.push_back({
                            {"name", jsonString(display, "_name")},
                            {"resolution", primaryResolution.empty() ? jsonString(display, "spdisplays_resolution") : primaryResolution},
                            {"main", jsonString(display, "spdisplays_main") == "spdisplays_yes"},
                            {"online", jsonString(display, "spdisplays_online") != "spdisplays_no"},
                            {"connection_type", jsonString(display, "spdisplays_connection_type")}
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
                    battery["cycle_count"] = jsonString(*bit, "sppower_battery_cycle_count");
                    battery["health"] = jsonString(*bit, "sppower_battery_health");
                    battery["condition"] = jsonString(*bit, "sppower_battery_condition");
                }
                if (cit != power.end() && cit->is_object()) {
                    battery["charge_percent"] = jsonString(*cit, "sppower_battery_charge_remaining");
                    battery["charging"] = jsonString(*cit, "sppower_battery_is_charging") == "TRUE";
                }
            }
        }
    }

    const auto totalMemory = runCommand("/usr/sbin/sysctl -n hw.memsize 2>/dev/null");
    json memoryModules = json::array();
    const auto chipType = jsonString(hardware, "chip_type");
    if (!totalMemory.empty()) {
        memoryModules.push_back({
            {"locator", "Unified/System Memory"},
            {"capacity_bytes", uintValue(totalMemory)},
            {"memory_type", chipType.empty() ? "System memory" : "Unified memory"},
            {"manufacturer", "Apple"}
        });
    }

    const auto machineModel = jsonString(hardware, "machine_model");
    const auto machineName = jsonString(hardware, "machine_name");
    const auto hwSerial = jsonString(hardware, "serial_number");
    const json motherboard = {
        {"manufacturer", "Apple Inc."},
        {"product", machineModel},
        {"serial_number", hwSerial},
        {"version", machineName},
        {"bios_version", jsonString(hardware, "boot_rom_version")},
        {"smbios_version", ""}
    };

    const json hardwareDetail = {
        {"manufacturer", "Apple Inc."},
        {"model", machineModel.empty() ? model() : machineModel},
        {"serial_number", hwSerial.empty() ? serialNumber() : hwSerial},
        {"device_uuid", jsonString(hardware, "platform_UUID")},
        {"sku", ""},
        {"system_family", machineName},
        {"bios_date", ""}
    };

    const auto totalCores = jsonString(hardware, "total_number_cores");
    const json cpuDetail = {
        {"name", chipType.empty() ? cpuModel() : chipType},
        {"vendor", "Apple"},
        {"cores", totalCores},
        {"sockets", jsonString(hardware, "number_processors")},
        {"logical_processors", totalCores},
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