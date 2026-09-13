from pathlib import Path

path = Path("src/service/service_main.cpp")
text = path.read_text(encoding="utf-8")

old = """#include <wtsapi32.h>
#include <userenv.h>
"""
new = """#include <wtsapi32.h>
#include <userenv.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
"""
if old not in text:
    raise SystemExit("include insertion point not found")
text = text.replace(old, new, 1)

marker = """        static void LogI(const std::string& s) { LogInfo("[service] " + s); }
        static void LogW(const std::string& s) { LogWarn("[service] " + s); }
        static void LogE(const std::string& s) { LogError("[service] " + s); }

"""

helpers = r'''        static std::wstring Utf8ToWide(const std::string& value) {
            if (value.empty()) return std::wstring();

            const int needed = MultiByteToWideChar(
                CP_UTF8,
                0,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0
            );

            if (needed <= 0) return std::wstring();

            std::wstring out(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(
                CP_UTF8,
                0,
                value.data(),
                static_cast<int>(value.size()),
                out.data(),
                needed
            );

            return out;
        }

        static std::string WideToUtf8(const std::wstring& value) {
            if (value.empty()) return std::string();

            const int needed = WideCharToMultiByte(
                CP_UTF8,
                0,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr
            );

            if (needed <= 0) return std::string();

            std::string out(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                value.data(),
                static_cast<int>(value.size()),
                out.data(),
                needed,
                nullptr,
                nullptr
            );

            return out;
        }

        static uint64_t FileTimeToU64(const FILETIME& ft) {
            ULARGE_INTEGER uli{};
            uli.LowPart = ft.dwLowDateTime;
            uli.HighPart = ft.dwHighDateTime;
            return uli.QuadPart;
        }

        struct CpuSample {
            uint64_t idle = 0;
            uint64_t kernel = 0;
            uint64_t user = 0;
        };

        static bool ReadCpuSample(CpuSample& sample) {
            FILETIME idleTime{}, kernelTime{}, userTime{};
            if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
                return false;
            }

            sample.idle = FileTimeToU64(idleTime);
            sample.kernel = FileTimeToU64(kernelTime);
            sample.user = FileTimeToU64(userTime);
            return true;
        }

        static double CpuPercentSince(CpuSample& previous, bool& hasPrevious) {
            CpuSample current{};
            if (!ReadCpuSample(current)) return -1.0;

            if (!hasPrevious) {
                previous = current;
                hasPrevious = true;
                return -1.0;
            }

            const uint64_t idleDelta = current.idle - previous.idle;
            const uint64_t kernelDelta = current.kernel - previous.kernel;
            const uint64_t userDelta = current.user - previous.user;
            const uint64_t totalDelta = kernelDelta + userDelta;

            previous = current;

            if (totalDelta == 0) return -1.0;

            double busy = static_cast<double>(totalDelta - idleDelta) * 100.0 / static_cast<double>(totalDelta);
            if (busy < 0.0) busy = 0.0;
            if (busy > 100.0) busy = 100.0;
            return busy;
        }

        static std::string ActiveConsoleUser() {
            DWORD sessionId = WTSGetActiveConsoleSessionId();
            if (sessionId == 0xFFFFFFFF) return {};

            LPWSTR user = nullptr;
            DWORD userBytes = 0;
            LPWSTR domain = nullptr;
            DWORD domainBytes = 0;

            std::wstring result;

            if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &user, &userBytes) &&
                user && user[0] != L'\0') {
                if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSDomainName, &domain, &domainBytes) &&
                    domain && domain[0] != L'\0') {
                    result = std::wstring(domain) + L"\\" + std::wstring(user);
                } else {
                    result = std::wstring(user);
                }
            }

            if (user) WTSFreeMemory(user);
            if (domain) WTSFreeMemory(domain);

            return WideToUtf8(result);
        }

        static double PrimaryDiskUsedPercent() {
            ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
            if (!GetDiskFreeSpaceExW(L"C:\\", &freeAvail, &total, &freeTotal)) {
                return -1.0;
            }

            if (total.QuadPart == 0) return -1.0;

            const uint64_t used = total.QuadPart - freeTotal.QuadPart;
            return static_cast<double>(used) * 100.0 / static_cast<double>(total.QuadPart);
        }

        static json BuildTelemetryPayload(CpuSample& previousCpu, bool& hasPreviousCpu) {
            MEMORYSTATUSEX mem{};
            mem.dwLength = sizeof(mem);
            const bool hasMemory = GlobalMemoryStatusEx(&mem) == TRUE;

            const double cpuPercent = CpuPercentSince(previousCpu, hasPreviousCpu);
            const double diskPercent = PrimaryDiskUsedPercent();

            json payload = {
                {"cpuPercent", cpuPercent >= 0.0 ? json(cpuPercent) : json(nullptr)},
                {"memoryUsedPercent", hasMemory ? json(static_cast<double>(mem.dwMemoryLoad)) : json(nullptr)},
                {"memoryTotalBytes", hasMemory ? json(static_cast<uint64_t>(mem.ullTotalPhys)) : json(nullptr)},
                {"memoryUsedBytes", hasMemory ? json(static_cast<uint64_t>(mem.ullTotalPhys - mem.ullAvailPhys)) : json(nullptr)},
                {"diskUsedPercent", diskPercent >= 0.0 ? json(diskPercent) : json(nullptr)},
                {"uptimeSeconds", static_cast<uint64_t>(GetTickCount64() / 1000ULL)},
                {"activeUser", ActiveConsoleUser()},
                {"serviceStatus", "Running"},
                {"websocketStatus", "Connected"}
            };

            return payload;
        }

        static void HttpPostJsonWithAgentAuth(
            const std::string& url,
            const std::string& body,
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
                throw std::runtime_error("telemetry WinHttpCrackUrl failed");
            }

            std::wstring requestPath(path, parts.dwUrlPathLength);
            if (parts.dwExtraInfoLength != static_cast<DWORD>(-1) && parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo) {
                requestPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            }

            const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
            const INTERNET_PORT port = parts.nPort;

            HINTERNET session = WinHttpOpen(
                L"Hi5CentralAgent/telemetry",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0
            );

            if (!session) throw std::runtime_error("telemetry WinHttpOpen failed");

            HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), port, 0);
            if (!connect) {
                WinHttpCloseHandle(session);
                throw std::runtime_error("telemetry WinHttpConnect failed");
            }

            HINTERNET request = WinHttpOpenRequest(
                connect,
                L"POST",
                requestPath.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                https ? WINHTTP_FLAG_SECURE : 0
            );

            if (!request) {
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                throw std::runtime_error("telemetry WinHttpOpenRequest failed");
            }

            DWORD timeoutMs = 10000;
            WinHttpSetOption(request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

            std::wstring headers =
                L"Content-Type: application/json\r\n"
                L"x-hi5-device-id: " + Utf8ToWide(ident.deviceId) + L"\r\n"
                L"x-hi5-agent-secret: " + Utf8ToWide(ident.deviceKey) + L"\r\n";

            BOOL ok = WinHttpSendRequest(
                request,
                headers.c_str(),
                static_cast<DWORD>(headers.size()),
                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()),
                0
            );

            if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
                DWORD err = GetLastError();
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                throw std::runtime_error("telemetry WinHTTP request failed err=" + std::to_string(err));
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

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);

            if (status < 200 || status >= 300) {
                throw std::runtime_error("telemetry HTTP status " + std::to_string(status));
            }
        }

'''

if marker not in text:
    raise SystemExit("log helper marker not found")

text = text.replace(marker, marker + helpers, 1)

old = """                signaling_ = std::make_unique<SignalingClient>(agentWsUrl);

                auto sendFn = [this](const std::string& payload) {
"""
new = """                signaling_ = std::make_unique<SignalingClient>(agentWsUrl);

                StartTelemetryLoop(ident);

                auto sendFn = [this](const std::string& payload) {
"""
if old not in text:
    raise SystemExit("signaling creation block not found")
text = text.replace(old, new, 1)

old = """            void StartInventoryLoop(AgentIdentity ident) {
                StopInventoryLoop();
                inventoryThread_ = std::thread([this, ident = std::move(ident)]() mutable {
                    // First snapshot is sent immediately from the WebSocket open callback.
                    // This loop sends follow-up live inventory every 60 seconds so
                    // the RMM portal stays up-to-date without waiting five minutes.
                    while (!stop_.load()) {
                        for (int i = 0; i < 60 && !stop_.load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                        if (stop_.load()) break;
                        if (HasActiveSessions()) {
                            LogI("scheduled full inventory skipped while remote session is active");
                            continue;
                        }
                        SendInventorySnapshotSafe(ident);
                    }
                    });
            }

"""
new = """            void StartInventoryLoop(AgentIdentity ident) {
                StopInventoryLoop();
                inventoryThread_ = std::thread([this, ident = std::move(ident)]() mutable {
                    // First snapshot is sent immediately from the WebSocket open callback.
                    // This loop sends follow-up live inventory every 60 seconds so
                    // the RMM portal stays up-to-date without waiting five minutes.
                    while (!stop_.load()) {
                        for (int i = 0; i < 60 && !stop_.load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                        if (stop_.load()) break;
                        if (HasActiveSessions()) {
                            LogI("scheduled full inventory skipped while remote session is active");
                            continue;
                        }
                        SendInventorySnapshotSafe(ident);
                    }
                    });
            }

            void StartTelemetryLoop(AgentIdentity ident) {
                std::thread([this, ident = std::move(ident)]() mutable {
                    CpuSample previousCpu{};
                    bool hasPreviousCpu = false;

                    BuildTelemetryPayload(previousCpu, hasPreviousCpu);

                    while (!stop_.load()) {
                        for (int i = 0; i < 10 && !stop_.load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }

                        if (stop_.load()) break;

                        try {
                            json payload = BuildTelemetryPayload(previousCpu, hasPreviousCpu);

                            HttpPostJsonWithAgentAuth(
                                "https://api.hi5central.com/api/v1/agent/devices/telemetry",
                                payload.dump(),
                                ident
                            );

                            LogI("telemetry posted cpu=" +
                                (payload["cpuPercent"].is_null() ? std::string("-") : std::to_string(payload["cpuPercent"].get<double>())) +
                                " ram=" +
                                (payload["memoryUsedPercent"].is_null() ? std::string("-") : std::to_string(payload["memoryUsedPercent"].get<double>())));
                        }
                        catch (const std::exception& ex) {
                            LogW(std::string("telemetry post failed: ") + ex.what());
                        }
                        catch (...) {
                            LogW("telemetry post failed: unknown error");
                        }
                    }
                    }).detach();
            }

"""
if old not in text:
    raise SystemExit("StartInventoryLoop block not found")
text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
