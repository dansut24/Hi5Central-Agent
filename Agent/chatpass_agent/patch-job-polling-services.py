from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

# 1) Add HTTP GET helper before ReadMachineEnvironmentValue.
end_marker = """        static std::string ReadMachineEnvironmentValue(const char* name) {
"""

helper = r'''
        static std::string HttpGetJsonWithAgentAuth(
            const std::string& url,
            const AgentIdentity& ident
        ) {
            const std::wstring wideUrl = Utf8ToWide(url);

            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);

            wchar_t host[256]{};
            wchar_t path[2048]{};

            parts.lpszHostName = host;
            parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
            parts.lpszUrlPath = path;
            parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
            parts.dwSchemeLength = static_cast<DWORD>(-1);
            parts.dwExtraInfoLength = static_cast<DWORD>(-1);

            if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
                throw std::runtime_error("job poll WinHttpCrackUrl failed");
            }

            std::wstring requestPath(path, parts.dwUrlPathLength);
            if (parts.dwExtraInfoLength != static_cast<DWORD>(-1) && parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo) {
                requestPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            }

            const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
            const INTERNET_PORT port = parts.nPort;

            HINTERNET session = WinHttpOpen(
                L"Hi5CentralAgent/jobs",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0
            );

            if (!session) {
                throw std::runtime_error("job poll WinHttpOpen failed");
            }

            HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), port, 0);
            if (!connect) {
                WinHttpCloseHandle(session);
                throw std::runtime_error("job poll WinHttpConnect failed");
            }

            HINTERNET request = WinHttpOpenRequest(
                connect,
                L"GET",
                requestPath.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                https ? WINHTTP_FLAG_SECURE : 0
            );

            if (!request) {
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                throw std::runtime_error("job poll WinHttpOpenRequest failed");
            }

            DWORD timeoutMs = 10000;
            WinHttpSetOption(request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

            std::wstring headers =
                L"x-hi5-device-id: " + Utf8ToWide(ident.deviceId) + L"\r\n"
                L"x-hi5-agent-secret: " + Utf8ToWide(ident.deviceKey) + L"\r\n";

            BOOL ok = WinHttpSendRequest(
                request,
                headers.c_str(),
                static_cast<DWORD>(headers.size()),
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0
            );

            if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
                DWORD err = GetLastError();
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                throw std::runtime_error("job poll WinHTTP request failed err=" + std::to_string(err));
            }

            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX
            );

            std::string response;
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;

                std::string chunk;
                chunk.resize(available);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                chunk.resize(read);
                response += chunk;
            }

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);

            if (status < 200 || status >= 300) {
                throw std::runtime_error("job poll HTTP status " + std::to_string(status) + " body=" + response);
            }

            return response;
        }

'''

if "static std::string HttpGetJsonWithAgentAuth(" not in text:
    if end_marker not in text:
        raise SystemExit("helper insertion point not found")
    text = text.replace(end_marker, helper + end_marker, 1)

# 2) Start job loop after telemetry loop.
old = """                StartTelemetryLoop(ident);

                auto sendFn = [this](const std::string& payload) {
"""
new = """                StartTelemetryLoop(ident);
                StartJobLoop(ident);

                auto sendFn = [this](const std::string& payload) {
"""

if old in text:
    text = text.replace(old, new, 1)
elif "StartJobLoop(ident);" not in text:
    raise SystemExit("StartTelemetryLoop insertion point not found")

# 3) Add services scripts, job result poster, job executor, and job loop.
anchor = """            void SendInventorySnapshotSafe(const AgentIdentity& ident) {
"""

insert = r'''
            std::string BuildServicesListScript() {
                return R"HI5PS(
$ErrorActionPreference = 'Continue'
$items = Get-CimInstance Win32_Service | Sort-Object DisplayName | ForEach-Object {
    [pscustomobject]@{
        name = $_.Name
        display_name = $_.DisplayName
        state = $_.State
        status = $_.Status
        start_mode = $_.StartMode
        start_name = $_.StartName
        process_id = [int]$_.ProcessId
        path_name = $_.PathName
        description = $_.Description
    }
}
[pscustomobject]@{
    action = 'services.list'
    status = 'ok'
    count = @($items).Count
    services = @($items)
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

            std::string BuildServiceControlScript(const std::string& actionType, const json& payload) {
                const std::string serviceName = payload.value("serviceName", payload.value("service_name", std::string()));
                const std::string psServiceName = PsSingleQuote(serviceName);

                std::string action = "restart";
                if (actionType == "services.start") action = "start";
                else if (actionType == "services.stop") action = "stop";

                return std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$serviceName = )HI5PS") + psServiceName + R"HI5PS(
$action = ')HI5PS" + action + R"HI5PS('

if ([string]::IsNullOrWhiteSpace($serviceName)) {
    throw 'serviceName is required'
}

$before = Get-Service -Name $serviceName -ErrorAction Stop

if ($action -eq 'start') {
    Start-Service -Name $serviceName -ErrorAction Stop
} elseif ($action -eq 'stop') {
    Stop-Service -Name $serviceName -ErrorAction Stop
} else {
    Restart-Service -Name $serviceName -Force -ErrorAction Stop
}

Start-Sleep -Milliseconds 700
$after = Get-Service -Name $serviceName -ErrorAction Stop

[pscustomobject]@{
    action = "services.$action"
    status = 'ok'
    service_name = $serviceName
    display_name = $after.DisplayName
    previous_state = $before.Status.ToString()
    state = $after.Status.ToString()
    completed_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 6 -Compress
)HI5PS";
            }

            void PostJobResult(const AgentIdentity& ident, const std::string& jobId, bool success, const json& result, const std::string& errorMessage = std::string()) {
                if (jobId.empty()) return;

                json body = {
                    {"success", success},
                    {"result", result}
                };

                if (!errorMessage.empty()) {
                    body["error"] = errorMessage;
                }

                HttpPostJsonWithAgentAuth(
                    "https://api.hi5central.com/api/v1/agent/devices/jobs/" + jobId + "/result",
                    body.dump(),
                    ident
                );
            }

            void ExecuteClaimedJob(const AgentIdentity& ident, const json& job) {
                const std::string jobId = job.value("id", std::string());
                const std::string jobType = job.value("job_type", std::string());
                const json payload = job.value("payload", json::object());

                if (jobId.empty() || jobType.empty()) return;

                try {
                    LogI("job claimed id=" + jobId + " type=" + jobType);

                    if (jobType == "inventory.scan" || jobType == "policy.refresh") {
                        SendInventorySnapshotSafe(ident);
                        PostJobResult(ident, jobId, true, json{
                            {"inventory_sent", true},
                            {"job_type", jobType}
                        });
                        return;
                    }

                    if (jobType == "custom.command") {
                        const std::string command = payload.value("command", std::string("whoami"));
                        const int timeoutSeconds = std::max(5, std::min(3600, payload.value("timeout_seconds", 120)));
                        CommandResult cr = RunPowerShellCommand(jobId, command, timeoutSeconds);
                        json result = BuildCommandActionResult(command, cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0;
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "services.list") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildServicesListScript(), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "services.start" || jobType == "services.stop" || jobType == "services.restart") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildServiceControlScript(jobType, payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    PostJobResult(ident, jobId, false, json{ {"job_type", jobType} }, "Unsupported job type: " + jobType);
                }
                catch (const std::exception& ex) {
                    try {
                        PostJobResult(ident, jobId, false, json::object(), ex.what());
                    } catch (...) {}
                }
                catch (...) {
                    try {
                        PostJobResult(ident, jobId, false, json::object(), "unknown error");
                    } catch (...) {}
                }
            }

            void StartJobLoop(AgentIdentity ident) {
                std::thread([this, ident = std::move(ident)]() mutable {
                    while (!stop_.load()) {
                        for (int i = 0; i < 5 && !stop_.load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }

                        if (stop_.load()) break;

                        try {
                            const std::string response = HttpGetJsonWithAgentAuth(
                                "https://api.hi5central.com/api/v1/agent/devices/jobs",
                                ident
                            );

                            const auto data = json::parse(response, nullptr, false);
                            if (data.is_discarded() || !data.value("success", false)) {
                                LogW("job poll returned invalid response");
                                continue;
                            }

                            const auto jobs = data.value("jobs", json::array());
                            if (!jobs.is_array() || jobs.empty()) {
                                continue;
                            }

                            LogI("job poll received count=" + std::to_string(jobs.size()));

                            for (const auto& job : jobs) {
                                if (stop_.load()) break;
                                ExecuteClaimedJob(ident, job);
                            }
                        }
                        catch (const std::exception& ex) {
                            LogW(std::string("job poll failed: ") + ex.what());
                        }
                        catch (...) {
                            LogW("job poll failed: unknown error");
                        }
                    }
                    }).detach();
            }

'''

if "void StartJobLoop(AgentIdentity ident)" not in text:
    if anchor not in text:
        raise SystemExit("SendInventorySnapshotSafe anchor not found")
    text = text.replace(anchor, insert + anchor, 1)

path.write_text(text, encoding="utf-8")
