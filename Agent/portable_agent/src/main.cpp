#include "agent_websocket.h"
#include "enhanced_inventory.h"
#include "remote_platform.h"
#include "remote_webrtc.h"
#include "http_client.h"
#include "job_executor.h"
#include "platform_info.h"
#include "portable_live_tools.h"
#include "software_inventory.h"
#include "software_actions.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

#include <sys/stat.h>
#include <unistd.h>

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

struct JobQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<json> jobs;
    std::unordered_set<std::string> knownIds;
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

std::string urlEncode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

std::string websocketUrl(const Identity& identity) {
    std::string base = trimSlash(identity.apiBase);
    if (base.rfind("https://", 0) == 0) {
        base.replace(0, 8, "wss://");
    } else if (base.rfind("http://", 0) == 0) {
        base.replace(0, 7, "ws://");
    } else {
        throw std::runtime_error("Unsupported API URL scheme for WebSocket");
    }

    return base + "/agent/ws?device_id=" + urlEncode(identity.deviceId) +
        "&device_key=" + urlEncode(identity.deviceKey);
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

json agentCapabilities() {
    const auto remoteDesktop = hi5::remoteDesktopCapabilities();
    return {
        {"telemetry", true},
        {"inventory", true},
        {"websocket", true},
        {"jobs", true},
        {"custom_command", true},
        {"terminal", true},
        {"files", true},
        {"execution_contexts", json::array({"user", "root"})},
        {"software_inventory", true},
        {"native_software_actions", true},
        {"remote_desktop", remoteDesktop.value("available", false)},
        {"remote_desktop_capabilities", remoteDesktop}
    };
}

json buildHello(const Identity& identity) {
    return {
        {"type", "hello"},
        {"device_id", identity.deviceId},
        {"agent_version", HI5CENTRAL_AGENT_VERSION},
        {"platform", hi5::platformId()},
        {"architecture", hi5::architecture()},
        {"hostname", hi5::hostname()},
        {"capabilities", agentCapabilities()}
    };
}

json buildTelemetry(bool websocketConnected) {
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
        {"websocketStatus", websocketConnected ? "Connected" : "TelemetryOnly"},
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
    const auto collectedAt = hi5::nowIsoUtc();
    const auto enhanced = hi5::enhancedHardwareInventory();

    json inventory = {
        {"type", "inventory_snapshot"},
        {"device_id", identity.deviceId},
        {"platform", hi5::platformId()},
        {"collected_at", collectedAt},
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
        {"software", hi5::softwareInventory()},
        {"sessions", {
            {"current_user", user},
            {"active_console_user", user}
        }},
        {"agent", {
            {"name", "Hi5Central Agent"},
            {"version", HI5CENTRAL_AGENT_VERSION},
            {"platform", hi5::platformId()},
            {"architecture", arch},
            {"transport", "websocket+https"},
            {"capabilities", agentCapabilities()}
        }},
        {"deep_inventory_included", true},
        {"deep_inventory_collected_at", collectedAt}
    };

    if (enhanced.is_object()) {
        for (auto it = enhanced.begin(); it != enhanced.end(); ++it) {
            inventory[it.key()] = it.value();
        }
    }

    if (inventory.contains("cpu_detail") && inventory["cpu_detail"].is_object()) {
        for (auto it = inventory["cpu_detail"].begin(); it != inventory["cpu_detail"].end(); ++it) {
            inventory["cpu"][it.key()] = it.value();
        }
        inventory.erase("cpu_detail");
    }

    return inventory;
}

void postTelemetry(
    const hi5::HttpClient& http,
    const Identity& identity,
    bool websocketConnected) {

    const auto response = http.postJson(
        identity.apiBase + "/api/v1/agent/devices/telemetry",
        buildTelemetry(websocketConnected).dump(),
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

void postJobResult(
    const hi5::HttpClient& http,
    const Identity& identity,
    const std::string& jobId,
    bool success,
    const json& result,
    const std::string& error = {}) {

    if (jobId.empty()) return;

    json body = {
        {"success", success},
        {"result", result}
    };
    if (!error.empty()) body["error"] = error;

    const auto response = http.postJson(
        identity.apiBase + "/api/v1/agent/devices/jobs/" + jobId + "/result",
        body.dump(),
        authHeaders(identity));

    if (!response.ok()) {
        throw std::runtime_error(
            "Job result HTTP " + std::to_string(response.status) +
            (response.error.empty() ? "" : " " + response.error) +
            (response.body.empty() ? "" : " body=" + response.body));
    }
}

bool enqueueJob(JobQueue& queue, const json& job) {
    const std::string jobId = job.value("id", "");
    const std::string jobType = job.value("job_type", "");
    if (jobId.empty() || jobType.empty()) return false;

    std::lock_guard<std::mutex> lock(queue.mutex);
    if (!queue.knownIds.insert(jobId).second) return false;
    queue.jobs.push_back(job);
    queue.cv.notify_one();
    return true;
}

void finishJob(JobQueue& queue, const std::string& jobId) {
    std::lock_guard<std::mutex> lock(queue.mutex);
    queue.knownIds.erase(jobId);
}

void executeJob(
    const hi5::HttpClient& http,
    const Identity& identity,
    const json& job) {

    const std::string jobId = job.value("id", "");
    const std::string jobType = job.value("job_type", "");
    const json payload = job.value("payload", json::object());

    if (jobId.empty() || jobType.empty()) return;

    logLine("INFO", "Executing job id=" + jobId + " type=" + jobType);

    try {
        if (jobType == "inventory.scan" || jobType == "policy.refresh") {
            postInventory(http, identity);
            postJobResult(
                http,
                identity,
                jobId,
                true,
                {
                    {"inventory_sent", true},
                    {"job_type", jobType},
                    {"platform", hi5::platformId()}
                });
            logLine("INFO", "Completed job id=" + jobId + " type=" + jobType);
            return;
        }

        if (jobType == "software.uninstall" || jobType == "software.update") {
            const auto action = jobType == "software.uninstall"
                ? hi5::uninstallNativeSoftware(payload)
                : hi5::updateNativeSoftware(payload);

            if (action.success) {
                try { postInventory(http, identity); }
                catch (const std::exception& inventoryError) {
                    logLine("WARN", std::string("Software action completed but inventory refresh failed: ") + inventoryError.what());
                }
            }

            postJobResult(
                http,
                identity,
                jobId,
                action.success,
                action.result,
                action.error);

            logLine(
                action.success ? "INFO" : "WARN",
                "Completed " + jobType + " id=" + jobId +
                " success=" + (action.success ? "true" : "false"));
            return;
        }

        if (jobType == "custom.command") {
            const std::string command = payload.value("command", "");
            const int timeoutSeconds = std::max(
                5,
                std::min(3600, payload.value("timeout_seconds", 120)));

            const std::string runAs = payload.value(
                "run_as",
                payload.value("runAs", std::string("root")));

            const auto commandResult = hi5::runShellCommand(
                command,
                timeoutSeconds,
                256 * 1024,
                runAs);

            auto result = hi5::buildCommandResultJson(command, commandResult);
            result["run_as"] = runAs;
            const bool success =
                commandResult.error.empty() &&
                commandResult.exitCode == 0;

            postJobResult(
                http,
                identity,
                jobId,
                success,
                result,
                commandResult.error);

            logLine(
                success ? "INFO" : "WARN",
                "Completed custom.command id=" + jobId +
                " exit_code=" + std::to_string(commandResult.exitCode) +
                " duration_ms=" + std::to_string(commandResult.durationMs));
            return;
        }

        postJobResult(
            http,
            identity,
            jobId,
            false,
            {{"job_type", jobType}, {"platform", hi5::platformId()}},
            "Unsupported job type on " + hi5::platformDisplayName() + ": " + jobType);
        logLine("WARN", "Rejected unsupported job id=" + jobId + " type=" + jobType);
    } catch (const std::exception& error) {
        logLine("WARN", "Job id=" + jobId + " failed: " + error.what());
        try {
            postJobResult(
                http,
                identity,
                jobId,
                false,
                json::object(),
                error.what());
        } catch (const std::exception& postError) {
            logLine("WARN", "Failed to post job failure id=" + jobId + ": " + postError.what());
        }
    }
}

void jobWorkerLoop(
    const hi5::HttpClient& http,
    const Identity& identity,
    JobQueue& queue) {

    while (g_running) {
        json job;
        {
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.cv.wait_for(
                lock,
                std::chrono::seconds(1),
                [&queue]() { return !queue.jobs.empty() || !g_running.load(); });

            if (!g_running && queue.jobs.empty()) break;
            if (queue.jobs.empty()) continue;

            job = std::move(queue.jobs.front());
            queue.jobs.pop_front();
        }

        const std::string jobId = job.value("id", "");
        executeJob(http, identity, job);
        finishJob(queue, jobId);
    }
}

void pollJobs(
    const hi5::HttpClient& http,
    const Identity& identity,
    JobQueue& queue) {

    const auto response = http.getJson(
        identity.apiBase + "/api/v1/agent/devices/jobs",
        authHeaders(identity));

    if (!response.ok()) {
        throw std::runtime_error(
            "Job poll HTTP " + std::to_string(response.status) +
            (response.error.empty() ? "" : " " + response.error));
    }

    const auto body = json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.value("success", false)) {
        throw std::runtime_error("Job poll returned an invalid response");
    }

    const auto jobs = body.value("jobs", json::array());
    if (!jobs.is_array()) return;

    std::size_t accepted = 0;
    for (const auto& job : jobs) {
        if (enqueueJob(queue, job)) ++accepted;
    }

    if (accepted) {
        logLine("INFO", "Queued " + std::to_string(accepted) + " polled job(s)");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const int helperResult = hi5::PortableLiveTools::runInternalHelper(argc, argv);
        if (helperResult >= 0) return helperResult;

        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        if (hasArg(argc, argv, "--version")) {
            std::cout << HI5CENTRAL_AGENT_VERSION << std::endl;
            return 0;
        }

        if (hasArg(argc, argv, "--remote-helper")) {
            return hi5::runRemoteDesktopHelper();
        }

        if (hasArg(argc, argv, "--self-test-command")) {
            const auto result = hi5::runShellCommand(
                "printf 'hi5central-command-ok'",
                10,
                4096,
                geteuid() == 0 ? "root" : "current");
            std::cout << hi5::buildCommandResultJson(
                "printf 'hi5central-command-ok'",
                result).dump(2) << std::endl;
            return result.exitCode == 0 && result.output == "hi5central-command-ok" ? 0 : 1;
        }

        if (hasArg(argc, argv, "--self-test-remote-capture")) {
            auto provider = hi5::createRemotePlatform();
            std::string error;
            if (!provider || !provider->start(error)) {
                std::cout << json({
                    {"ok", false},
                    {"backend", provider ? provider->backendName() : "none"},
                    {"error", error.empty() ? "Remote provider unavailable." : error}
                }).dump(2) << std::endl;
                return 2;
            }

            const auto captured = provider->capture();
            std::uint64_t checksum = 1469598103934665603ULL;
            auto hashPlane = [&checksum](const std::vector<std::uint8_t>& plane) {
                const std::size_t step = std::max<std::size_t>(1, plane.size() / 4096);
                for (std::size_t i = 0; i < plane.size(); i += step) {
                    checksum ^= plane[i];
                    checksum *= 1099511628211ULL;
                }
            };
            hashPlane(captured.frame.y);
            hashPlane(captured.frame.u);
            hashPlane(captured.frame.v);
            const auto displays = provider->displays();
            provider->stop();

            std::cout << json({
                {"ok", captured.hasFrame},
                {"backend", provider->backendName()},
                {"width", captured.frame.width},
                {"height", captured.frame.height},
                {"display_count", displays.size()},
                {"sample_checksum", checksum}
            }).dump(2) << std::endl;
            return captured.hasFrame && captured.frame.width > 0 && captured.frame.height > 0 ? 0 : 3;
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
                {"software", hi5::softwareInventory()},
                {"capabilities", agentCapabilities()}
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
        JobQueue jobQueue;
        std::thread jobWorker;

        hi5::AgentWebSocket websocket;
        hi5::PortableLiveTools liveTools(
            [&websocket](const json& message) {
                return websocket.sendText(message.dump());
            });
        hi5::RemoteDesktopManager remoteDesktop(
            [&websocket](const json& message) {
                return websocket.sendText(message.dump());
            });

        if (!once) {
            jobWorker = std::thread(
                [&http, &identity, &jobQueue]() {
                    jobWorkerLoop(http, identity, jobQueue);
                });

            websocket.start(
                websocketUrl(identity),
                [&jobQueue, &liveTools, &remoteDesktop](const std::string& text) {
                    const auto message = json::parse(text, nullptr, false);
                    if (!message.is_object()) return;

                    const std::string type = message.value("type", "");
                    if (type == "job_execute" && message.contains("job") && message["job"].is_object()) {
                        if (enqueueJob(jobQueue, message["job"])) {
                            logLine(
                                "INFO",
                                "Received live job id=" +
                                message["job"].value("id", "") +
                                " type=" +
                                message["job"].value("job_type", ""));
                        }
                    } else if (type == "hello_ack") {
                        logLine("INFO", "Agent WebSocket hello acknowledged");
                    } else if (remoteDesktop.handleMessage(message)) {
                        logLine("INFO", "Handled remote desktop message type=" + type);
                    } else if (liveTools.handleMessage(message)) {
                        logLine("INFO", "Handled live tool message type=" + type);
                    }
                },
                [&websocket, &identity](bool connected, const std::string& detail) {
                    if (connected) {
                        logLine("INFO", "Agent WebSocket connected");
                        websocket.sendText(buildHello(identity).dump());
                    } else {
                        logLine("WARN", "Agent WebSocket " + detail);
                    }
                });
        }

        const auto telemetryInterval = std::chrono::seconds(30);
        const auto inventoryInterval = std::chrono::minutes(15);
        const auto jobPollInterval = std::chrono::seconds(10);

        auto nextTelemetry = std::chrono::steady_clock::now();
        auto nextInventory = std::chrono::steady_clock::now();
        auto nextJobPoll = std::chrono::steady_clock::now();

        logLine(
            "INFO",
            std::string("Hi5Central portable Agent ") + HI5CENTRAL_AGENT_VERSION +
            " started platform=" + hi5::platformId() +
            " device_id=" + identity.deviceId);

        do {
            const auto now = std::chrono::steady_clock::now();

            if (now >= nextTelemetry) {
                try {
                    postTelemetry(http, identity, websocket.connected());
                    logLine(
                        "INFO",
                        std::string("Telemetry uploaded transport=") +
                        (websocket.connected() ? "websocket+https" : "https"));
                } catch (const std::exception& error) {
                    logLine("WARN", error.what());
                }
                nextTelemetry = now + telemetryInterval;
            }

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

            if (!once && now >= nextJobPoll) {
                try {
                    pollJobs(http, identity, jobQueue);
                } catch (const std::exception& error) {
                    logLine("WARN", error.what());
                }
                nextJobPoll = now + jobPollInterval;
            }

            if (once) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } while (g_running);

        liveTools.stopAll();
        websocket.stop();
        jobQueue.cv.notify_all();
        if (jobWorker.joinable()) jobWorker.join();

        logLine("INFO", "Hi5Central portable Agent stopped");
        return 0;
    } catch (const std::exception& error) {
        logLine("ERROR", error.what());
        return 1;
    }
}