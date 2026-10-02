#include "http_client.h"
#include "platform_info.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/stat.h>

#ifndef HI5CENTRAL_AGENT_VERSION
#define HI5CENTRAL_AGENT_VERSION "0.3.0-alpha"
#endif

namespace {

using json = nlohmann::json;

std::atomic<bool> g_running{true};

struct Identity {
    std::string deviceId;
    std::string deviceKey;
    std::string tenantId;
    std::string apiBase;
};

void signalHandler(int) {
    g_running = false;
}

std::string trimSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

void logLine(const std::string& level, const std::string& message) {
    std::cerr << hi5::nowIsoUtc() << " [" << level << "] " << message << std::endl;
}

std::string getenvString(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
}

std::string argValue(int argc, char* argv[], const std::string& name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && name == argv[i] && argv[i + 1]) return argv[i + 1];
    }
    return {};
}

bool hasArg(int argc, char* argv[], const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && name == argv[i]) return true;
    }
    return false;
}

std::filesystem::path stateFile(const std::filesystem::path& dir) {
    return dir / "agent.json";
}

void saveIdentity(const std::filesystem::path& dir, const Identity& identity) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("Unable to create Agent state directory: " + ec.message());
    }

    const auto path = stateFile(dir);
    const auto temp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Unable to write Agent identity");
        out << json({
            {"device_id", identity.deviceId},
            {"device_key", identity.deviceKey},
            {"tenant_id", identity.tenantId},
            {"api_base_url", identity.apiBase},
            {"agent_version", HI5CENTRAL_AGENT_VERSION},
            {"platform", hi5::platformId()}
        }).dump(2) << "\n";
    }

    chmod(temp.c_str(), S_IRUSR | S_IWUSR);
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        throw std::runtime_error("Unable to commit Agent identity: " + ec.message());
    }
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
}

Identity loadIdentity(const std::filesystem::path& dir) {
    const auto path = stateFile(dir);
    std::ifstream in(path);
    if (!in) return {};

    auto value = json::parse(in, nullptr, false);
    if (!value.is_object()) return {};

    return {
        value.value("device_id", ""),
        value.value("device_key", ""),
        value.value("tenant_id", ""),
        trimSlash(value.value("api_base_url", "https://api.hi5central.com"))
    };
}

Identity enroll(
    const hi5::HttpClient& http,
    const std::string& apiBase,
    const std::string& token) {

    const json request = {
        {"enrollmentToken", token},
        {"hostname", hi5::hostname()},
        {"platform", hi5::platformId()},
        {"architecture", hi5::architecture()},
        {"agentVersion", HI5CENTRAL_AGENT_VERSION},
        {"fingerprint", hi5::machineId()}
    };

    const auto response = http.postJson(
        apiBase + "/api/v1/agent/enroll",
        request.dump());

    if (!response.ok()) {
        throw std::runtime_error(
            "Enrollment failed HTTP " + std::to_string(response.status) +
            (response.error.empty() ? "" : " " + response.error) +
            (response.body.empty() ? "" : " body=" + response.body));
    }

    const auto body = json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.value("success", false)) {
        throw std::runtime_error("Enrollment response was invalid: " + response.body);
    }

    Identity identity;
    identity.deviceId = body.value("device_id", "");
    identity.deviceKey = body.value("device_key", "");
    identity.tenantId = body.value("tenant_id", "");
    identity.apiBase = apiBase;

    if (identity.deviceId.empty() || identity.deviceKey.empty()) {
        throw std::runtime_error("Enrollment did not return a device identity");
    }

    return identity;
}

std::map<std::string, std::string> authHeaders(const Identity& identity) {
    return {
        {"x-hi5-device-id", identity.deviceId},
        {"x-hi5-agent-secret", identity.deviceKey}
    };
}

json buildTelemetry() {
    const auto memory = hi5::memoryStats();
    const auto disk = hi5::rootDiskStats();

    return {
        {"cpuPercent", hi5::cpuPercent()},
        {"memoryUsedPercent", memory.usedPercent},
        {"memoryTotalBytes", memory.totalBytes},
        {"memoryUsedBytes", memory.usedBytes},
        {"diskUsedPercent", disk.usedPercent},
        {"uptimeSeconds", hi5::uptimeSeconds()},
        {"activeUser", hi5::activeUser()},
        {"serviceStatus", "Running"},
        {"websocketStatus", "TelemetryOnly"},
        {"platform", hi5::platformId()},
        {"agentVersion", HI5CENTRAL_AGENT_VERSION}
    };
}

json buildInventory(const Identity& identity) {
    const auto memory = hi5::memoryStats();
    const auto disk = hi5::rootDiskStats();
    const auto user = hi5::activeUser();
    const auto operatingSystem = hi5::platformDisplayName();
    const auto version = hi5::osVersion();
    const auto build = hi5::osBuild();
    const auto arch = hi5::architecture();

    return {
        {"type", "inventory_snapshot"},
        {"device_id", identity.deviceId},
        {"platform", hi5::platformId()},
        {"collected_at", hi5::nowIsoUtc()},
        {"summary", {
            {"hostname", hi5::hostname()},
            {"platform", hi5::platformId()},
            {"operating_system", operatingSystem},
            {"os_name", operatingSystem},
            {"os_version", version},
            {"os_build", build},
            {"manufacturer", hi5::manufacturer()},
            {"model", hi5::model()},
            {"serial_number", hi5::serialNumber()},
            {"total_memory_bytes", memory.totalBytes},
            {"logged_in_user", user}
        }},
        {"os", {
            {"name", operatingSystem},
            {"platform", hi5::platformId()},
            {"version", version},
            {"build", build},
            {"architecture", arch},
            {"uptime_seconds", hi5::uptimeSeconds()}
        }},
        {"hardware", {
            {"manufacturer", hi5::manufacturer()},
            {"model", hi5::model()},
            {"serial_number", hi5::serialNumber()}
        }},
        {"cpu", {
            {"name", hi5::cpuModel()},
            {"logical_processors", std::thread::hardware_concurrency()},
            {"architecture", arch}
        }},
        {"memory", {
            {"total_bytes", memory.totalBytes},
            {"available_bytes", memory.availableBytes},
            {"used_bytes", memory.usedBytes},
            {"used_percent", memory.usedPercent}
        }},
        {"storage", json::array({
            {
                {"drive", "/"},
                {"mount", "/"},
                {"type", "Fixed"},
                {"total_bytes", disk.totalBytes},
                {"free_bytes", disk.freeBytes},
                {"used_bytes", disk.usedBytes},
                {"used_percent", disk.usedPercent}
            }
        })},
        {"network", hi5::networkInfo()},
        {"sessions", {
            {"current_user", user},
            {"active_console_user", user}
        }},
        {"agent", {
            {"name", "Hi5Central Agent"},
            {"version", HI5CENTRAL_AGENT_VERSION},
            {"platform", hi5::platformId()},
            {"architecture", arch},
            {"transport", "https-polling"},
            {"capabilities", {
                {"telemetry", true},
                {"inventory", true},
                {"jobs", false},
                {"terminal", false},
                {"files", false},
                {"remote_desktop", false}
            }}
        }},
        {"deep_inventory_included", false}
    };
}

void postTelemetry(const hi5::HttpClient& http, const Identity& identity) {
    const auto response = http.postJson(
        identity.apiBase + "/api/v1/agent/devices/telemetry",
        buildTelemetry().dump(),
        authHeaders(identity));

    if (!response.ok()) {
        throw std::runtime_error(
            "Telemetry HTTP " + std::to_string(response.status) +
            (response.error.empty() ? "" : " " + response.error) +
            (response.body.empty() ? "" : " body=" + response.body));
    }
}

void postInventory(const hi5::HttpClient& http, const Identity& identity) {
    const auto response = http.postJson(
        identity.apiBase + "/api/v1/agent/devices/inventory",
        buildInventory(identity).dump(),
        authHeaders(identity));

    if (!response.ok()) {
        throw std::runtime_error(
            "Inventory HTTP " + std::to_string(response.status) +
            (response.error.empty() ? "" : " " + response.error) +
            (response.body.empty() ? "" : " body=" + response.body));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        if (hasArg(argc, argv, "--version")) {
            std::cout << HI5CENTRAL_AGENT_VERSION << std::endl;
            return 0;
        }

        if (hasArg(argc, argv, "--self-test")) {
            const auto memory = hi5::memoryStats();
            const auto disk = hi5::rootDiskStats();
            json output = {
                {"version", HI5CENTRAL_AGENT_VERSION},
                {"platform", hi5::platformId()},
                {"hostname", hi5::hostname()},
                {"architecture", hi5::architecture()},
                {"osVersion", hi5::osVersion()},
                {"osBuild", hi5::osBuild()},
                {"manufacturer", hi5::manufacturer()},
                {"model", hi5::model()},
                {"serialNumber", hi5::serialNumber()},
                {"cpuModel", hi5::cpuModel()},
                {"activeUser", hi5::activeUser()},
                {"uptimeSeconds", hi5::uptimeSeconds()},
                {"cpuPercent", hi5::cpuPercent()},
                {"memory", {
                    {"totalBytes", memory.totalBytes},
                    {"usedPercent", memory.usedPercent}
                }},
                {"disk", {
                    {"totalBytes", disk.totalBytes},
                    {"usedPercent", disk.usedPercent}
                }},
                {"network", hi5::networkInfo()}
            };
            std::cout << output.dump(2) << std::endl;
            return 0;
        }

        std::filesystem::path stateDir = hi5::defaultStateDirectory();
        const auto customState = argValue(argc, argv, "--state-dir");
        if (!customState.empty()) stateDir = customState;

        std::string apiBase = argValue(argc, argv, "--api-base");
        if (apiBase.empty()) apiBase = getenvString("HI5_API_BASE");
        if (apiBase.empty()) apiBase = "https://api.hi5central.com";
        apiBase = trimSlash(apiBase);

        hi5::HttpClient http;
        auto identity = loadIdentity(stateDir);
        if (identity.apiBase.empty()) identity.apiBase = apiBase;

        if (identity.deviceId.empty() || identity.deviceKey.empty()) {
            std::string token = argValue(argc, argv, "--enrollment-token");
            if (token.empty()) token = getenvString("HI5_ENROLLMENT_TOKEN");

            if (token.empty()) {
                throw std::runtime_error(
                    "Agent is not enrolled. Supply --enrollment-token <token> or HI5_ENROLLMENT_TOKEN.");
            }

            logLine(
                "INFO",
                "Enrolling " + hi5::hostname() +
                " as " + hi5::platformId() + "/" + hi5::architecture());

            identity = enroll(http, apiBase, token);
            saveIdentity(stateDir, identity);
            logLine("INFO", "Enrollment succeeded device_id=" + identity.deviceId);
        }

        if (hasArg(argc, argv, "--enroll-only")) return 0;

        const bool once = hasArg(argc, argv, "--once");
        const auto telemetryInterval = std::chrono::seconds(30);
        const auto inventoryInterval = std::chrono::minutes(15);
        auto nextInventory = std::chrono::steady_clock::now();

        logLine(
            "INFO",
            std::string("Hi5Central portable Agent ") + HI5CENTRAL_AGENT_VERSION +
            " started platform=" + hi5::platformId() +
            " device_id=" + identity.deviceId);

        while (g_running) {
            try {
                postTelemetry(http, identity);
                logLine("INFO", "Telemetry uploaded");
            } catch (const std::exception& error) {
                logLine("WARN", error.what());
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextInventory) {
                try {
                    postInventory(http, identity);
                    nextInventory = now + inventoryInterval;
                    logLine("INFO", "Inventory uploaded");
                } catch (const std::exception& error) {
                    logLine("WARN", error.what());
                    nextInventory = now + std::chrono::minutes(1);
                }
            }

            if (once) break;

            for (int elapsed = 0;
                 elapsed < telemetryInterval.count() && g_running;
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        logLine("INFO", "Hi5Central portable Agent stopped");
        return 0;
    } catch (const std::exception& error) {
        logLine("ERROR", error.what());
        return 1;
    }
}
