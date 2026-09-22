#include "service/service_main.h"
#include "agent_identity.h"
#include "agent_version.h"

#include "signaling_client.h"
#include "codec_policy.h"
#include "ipc/input_pipe.h"
#include "ipc/session_launcher.h"
#include "ipc/shmem_ring.h"
#include "ipc/named_pipe.h"
#include "util/log.h"
#include "session/session_bridge.h"
#include "ui/agent_presence_controller.h"
#include "ui/chat_controller.h"
#include "inventory/inventory_snapshot.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <psapi.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <winhttp.h>
#include <map>

#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <cstdio>
#include <unordered_map>
#include <functional>
#include <vector>

using json = nlohmann::json;

namespace hi5 {

    namespace {

        static SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
        static SERVICE_STATUS g_serviceStatus{};
        static std::string g_serviceName;
        static std::atomic<bool> g_stopRequested{ false };
        // SCM/WTS session lifecycle is the authoritative source for logoff/logon.
        // Polling WTSGetActiveConsoleSessionId/WTSQueryUserToken alone races Windows
        // while the old desktop is being destroyed.
        static std::atomic<uint64_t> g_sessionChangeSeq{ 0 };
        static std::atomic<DWORD> g_sessionChangeType{ 0 };
        static std::atomic<DWORD> g_sessionChangeSessionId{ 0xFFFFFFFF };

        static void LogI(const std::string& s) { LogInfo("[service] " + s); }
        static void LogW(const std::string& s) { LogWarn("[service] " + s); }
        static void LogE(const std::string& s) { LogError("[service] " + s); }

        static std::wstring Utf8ToWide(const std::string& value) {
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

        static std::string ReadMachineEnvironmentValue(const char* name) {
            HKEY key = nullptr;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                R"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)",
                0,
                KEY_READ,
                &key) != ERROR_SUCCESS) {
                return {};
            }

            char buffer[256]{};
            DWORD type = 0;
            DWORD size = sizeof(buffer);
            const LONG rc = RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
            RegCloseKey(key);

            if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
                return {};
            }

            return std::string(buffer);
        }

        static std::string ReadConfigValue(const char* name) {
            char buffer[256]{};
            DWORD len = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
            if (len > 0 && len < sizeof(buffer)) {
                return std::string(buffer, len);
            }
            return ReadMachineEnvironmentValue(name);
        }

        static int ReadConfigInt(const char* name, int fallback, int minValue, int maxValue) {
            std::string raw = ReadConfigValue(name);
            if (raw.empty()) {
                return fallback;
            }

            int value = fallback;
            try {
                value = std::stoi(raw);
            }
            catch (...) {
                value = fallback;
            }

            value = std::max(minValue, std::min(maxValue, value));
            return value;
        }

        static std::string ReadConfigString(const char* name, const std::string& fallback) {
            std::string raw = ReadConfigValue(name);
            return raw.empty() ? fallback : raw;
        }

        static std::vector<std::string> SplitLines(const std::string& text) {
            std::vector<std::string> lines;
            std::istringstream iss(text);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) lines.push_back(line);
            }
            return lines;
        }


        static std::string LocalComputerName() {
            char name[MAX_COMPUTERNAME_LENGTH + 1]{};
            DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
            if (GetComputerNameA(name, &len) && len > 0) {
                return std::string(name, len);
            }
            return "Remote device";
        }

        static std::filesystem::path ChatLogDir() {
            return std::filesystem::path(LR"(C:\ProgramData\Hi5Central\Agent\ChatLogs)");
        }

        static void PurgeOldChatLogs(int maxAgeDays = 90) {
            namespace fs = std::filesystem;
            const auto dir = ChatLogDir();
            std::error_code ec;
            fs::create_directories(dir, ec);
            const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * maxAgeDays);
            for (const auto& item : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!item.is_regular_file(ec)) continue;
                if (item.path().extension() != ".json") continue;
                const auto t = item.last_write_time(ec);
                if (!ec && t < cutoff) {
                    fs::remove(item.path(), ec);
                }
            }
        }

        static std::string SafeFilePart(std::string value) {
            for (char& c : value) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
                if (!ok) c = '_';
            }
            if (value.empty()) value = "unknown";
            return value;
        }

        static std::int64_t NowUnixMs() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        static std::string NowIsoUtc() {
            SYSTEMTIME st{};
            GetSystemTime(&st);
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                static_cast<unsigned>(st.wYear),
                static_cast<unsigned>(st.wMonth),
                static_cast<unsigned>(st.wDay),
                static_cast<unsigned>(st.wHour),
                static_cast<unsigned>(st.wMinute),
                static_cast<unsigned>(st.wSecond),
                static_cast<unsigned>(st.wMilliseconds));
            return std::string(buf);
        }

        static std::string ChatBodyFromJson(const json& msg) {
            for (const char* key : { "body", "message", "text", "content" }) {
                auto it = msg.find(key);
                if (it != msg.end() && it->is_string()) {
                    return it->get<std::string>();
                }
            }
            return {};
        }

        static void AppendChatTranscript(const std::string& sessionId,
            const std::string& sender,
            const std::string& displayName,
            const std::string& body) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const auto dir = ChatLogDir();
            fs::create_directories(dir, ec);
            if (ec) return;

            const auto file = dir / ("chat_" + SafeFilePart(sessionId) + ".json");
            json root = json::array();
            {
                std::ifstream in(file, std::ios::binary);
                if (in.good()) {
                    auto parsed = json::parse(in, nullptr, false);
                    if (parsed.is_array()) root = std::move(parsed);
                }
            }

            root.push_back({
                {"unix_ms", NowUnixMs()},
                {"session_id", sessionId},
                {"sender", sender},
                {"display_name", displayName},
                {"body", body}
                });

            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (out.good()) {
                out << root.dump(2);
            }
        }

        static DWORD ActiveConsoleSessionId() {
            return WTSGetActiveConsoleSessionId();
        }

        // A numeric console session does not necessarily have an interactive
        // user desktop. After sign-out Windows keeps Winlogon on the console,
        // but WTSQueryUserToken correctly reports ERROR_NO_TOKEN until a user
        // has logged in. Use this as the boundary between Winlogon capture and
        // the normal/default desktop.
        static bool InteractiveUserSessionReady(DWORD sessionId) {
            if (sessionId == 0xFFFFFFFF) return false;
            HANDLE token = nullptr;
            if (!WTSQueryUserToken(sessionId, &token) || !token) {
                return false;
            }
            CloseHandle(token);
            return true;
        }

        static std::string SessionIdToString(DWORD sessionId) {
            if (sessionId == 0xFFFFFFFF) return std::string("none");
            return std::to_string(sessionId);
        }

        enum class DesktopMode {
            Normal,
            Secure
        };

        enum class SessionMode {
            Console,
            Backstage
        };

        static SessionMode ParseSessionMode(const std::string& raw) {
            std::string mode = raw;
            std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
                });
            if (mode == "backstage" || mode == "background" || mode == "background_mode") {
                return SessionMode::Backstage;
            }
            return SessionMode::Console;
        }

        static const char* SessionModeName(SessionMode mode) {
            return mode == SessionMode::Backstage ? "backstage" : "console";
        }

        static std::vector<std::string> parseIceServers(const json& msg) {
            std::vector<std::string> out;
            if (!msg.contains("iceServers") || !msg["iceServers"].is_array()) {
                return out;
            }
            for (const auto& v : msg["iceServers"]) {
                if (v.is_string()) {
                    out.push_back(v.get<std::string>());
                }
            }
            return out;
        }

        static void SetServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0) {
            if (!g_statusHandle) return;

            g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
            g_serviceStatus.dwCurrentState = state;
            g_serviceStatus.dwWin32ExitCode = win32ExitCode;
            g_serviceStatus.dwWaitHint = waitHint;
            g_serviceStatus.dwControlsAccepted =
                (state == SERVICE_START_PENDING)
                ? 0
                : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE);

            SetServiceStatus(g_statusHandle, &g_serviceStatus);
        }

        static bool DomCodeToVk(const std::string& code, WORD& vk, bool& extended) {
            extended = false;

            if (code.size() == 4 && code.rfind("Key", 0) == 0) {
                const char c = code[3];
                if (c >= 'A' && c <= 'Z') {
                    vk = static_cast<WORD>(c);
                    return true;
                }
            }

            if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
                const char c = code[5];
                if (c >= '0' && c <= '9') {
                    vk = static_cast<WORD>(c);
                    return true;
                }
            }

            if (code == "Enter") { vk = VK_RETURN; return true; }
            if (code == "Escape") { vk = VK_ESCAPE; return true; }
            if (code == "Backspace") { vk = VK_BACK; return true; }
            if (code == "Tab") { vk = VK_TAB; return true; }
            if (code == "Space") { vk = VK_SPACE; return true; }

            if (code == "ShiftLeft") { vk = VK_LSHIFT; return true; }
            if (code == "ShiftRight") { vk = VK_RSHIFT; return true; }
            if (code == "ControlLeft") { vk = VK_LCONTROL; return true; }
            if (code == "ControlRight") { vk = VK_RCONTROL; extended = true; return true; }
            if (code == "AltLeft") { vk = VK_LMENU; return true; }
            if (code == "AltRight") { vk = VK_RMENU; extended = true; return true; }
            if (code == "MetaLeft") { vk = VK_LWIN; extended = true; return true; }
            if (code == "MetaRight") { vk = VK_RWIN; extended = true; return true; }

            if (code == "ArrowUp") { vk = VK_UP; extended = true; return true; }
            if (code == "ArrowDown") { vk = VK_DOWN; extended = true; return true; }
            if (code == "ArrowLeft") { vk = VK_LEFT; extended = true; return true; }
            if (code == "ArrowRight") { vk = VK_RIGHT; extended = true; return true; }

            if (code == "Delete") { vk = VK_DELETE; extended = true; return true; }
            if (code == "Insert") { vk = VK_INSERT; extended = true; return true; }
            if (code == "Home") { vk = VK_HOME; extended = true; return true; }
            if (code == "End") { vk = VK_END; extended = true; return true; }
            if (code == "PageUp") { vk = VK_PRIOR; extended = true; return true; }
            if (code == "PageDown") { vk = VK_NEXT; extended = true; return true; }

            if (code == "F1") { vk = VK_F1; return true; }
            if (code == "F2") { vk = VK_F2; return true; }
            if (code == "F3") { vk = VK_F3; return true; }
            if (code == "F4") { vk = VK_F4; return true; }
            if (code == "F5") { vk = VK_F5; return true; }
            if (code == "F6") { vk = VK_F6; return true; }
            if (code == "F7") { vk = VK_F7; return true; }
            if (code == "F8") { vk = VK_F8; return true; }
            if (code == "F9") { vk = VK_F9; return true; }
            if (code == "F10") { vk = VK_F10; return true; }
            if (code == "F11") { vk = VK_F11; return true; }
            if (code == "F12") { vk = VK_F12; return true; }

            return false;
        }


        static bool IsTextualKeyboardKey(const std::string& key) {
            if (key.empty()) return false;
            if (key == "Alt" || key == "AltGraph" || key == "CapsLock" ||
                key == "Control" || key == "Dead" || key == "Delete" ||
                key == "End" || key == "Enter" || key == "Escape" ||
                key == "Fn" || key == "FnLock" || key == "Home" ||
                key == "Hyper" || key == "Insert" || key == "Meta" ||
                key == "NumLock" || key == "OS" || key == "PageDown" ||
                key == "PageUp" || key == "Process" || key == "ScrollLock" ||
                key == "Shift" || key == "Super" || key == "Symbol" ||
                key == "SymbolLock" || key == "Tab" || key == "Unidentified" ||
                key == "ContextMenu" || key == "Pause" || key == "PrintScreen") {
                return false;
            }
            // UTF-8 printable characters may be 1-4 bytes. Browser named keys are longer.
            return key.size() <= 8;
        }


        static std::string CurrentExePath() {
            char buf[MAX_PATH]{};
            DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
            return n ? std::string(buf, buf + n) : std::string();
        }

        static std::string DirOfPath(const std::string& path) {
            const auto pos = path.find_last_of("\\/");
            if (pos == std::string::npos) return ".";
            return path.substr(0, pos);
        }

        static std::string SasHelperExePath() {
            const std::string exe = CurrentExePath();
            const std::string dir = DirOfPath(exe);
            return dir + "\\hi5central_sas_helper.exe";
        }

        static std::string QuoteArg(const std::string& value) {
            std::string out = "\"";
            for (char c : value) { if (c == '"') out += '\\'; out += c; }
            out += "\"";
            return out;
        }

        static std::wstring WideFromUtf8(const std::string& s) {
            if (s.empty()) return std::wstring();
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            if (n <= 1) return std::wstring();
            std::wstring w(static_cast<size_t>(n - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
            return w;
        }

        static bool EnableTokenPrivilege(HANDLE token, const wchar_t* privName) {
            TOKEN_PRIVILEGES tp{};
            if (!privName || !*privName) return false;
            if (!LookupPrivilegeValueW(nullptr, privName, &tp.Privileges[0].Luid)) return false;
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
            return GetLastError() == ERROR_SUCCESS;
        }

        static bool EnableTokenPrivilege(HANDLE token, const char* privName) {
            if (!privName || !*privName) return false;

            int needed = MultiByteToWideChar(CP_UTF8, 0, privName, -1, nullptr, 0);
            UINT codePage = CP_UTF8;
            if (needed <= 0) {
                codePage = CP_ACP;
                needed = MultiByteToWideChar(codePage, 0, privName, -1, nullptr, 0);
            }
            if (needed <= 0) return false;

            std::wstring wide;
            wide.resize(static_cast<size_t>(needed));
            int written = MultiByteToWideChar(codePage, 0, privName, -1, wide.data(), needed);
            if (written <= 0) return false;

            if (!wide.empty() && wide.back() == L'\0') {
                wide.pop_back();
            }

            return EnableTokenPrivilege(token, wide.c_str());
        }

        static std::filesystem::path CadStatusPath() {
            return std::filesystem::path(LR"(C:\ProgramData\Hi5Central\Agent\Logs\CadStatus.txt)");
        }

        static void CadLog(const std::string& line) {
            try {
                std::error_code ec;
                std::filesystem::create_directories(CadStatusPath().parent_path(), ec);
                std::ofstream out(CadStatusPath(), std::ios::binary | std::ios::app);
                if (out.good()) {
                    out << NowIsoUtc() << " " << line << "\n";
                }
            }
            catch (...) {
            }
            LogI("[cad] " + line);
        }

        static std::string LastErrorText(DWORD err) {
            if (err == 0) return "0";
            LPSTR buf = nullptr;
            DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                reinterpret_cast<LPSTR>(&buf), 0, nullptr);
            std::string out = std::to_string(err);
            if (len && buf) {
                out += " ";
                out.append(buf, len);
                while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
            }
            if (buf) LocalFree(buf);
            return out;
        }

        static std::string BoolText(bool v) { return v ? "true" : "false"; }

        static bool IsRunningAsLocalSystem() {
#ifdef _WIN32
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
            DWORD needed = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
            std::vector<BYTE> buf(needed);
            bool ok = false;
            if (needed && GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
                auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
                LPSTR sidText = nullptr;
                if (ConvertSidToStringSidA(tu->User.Sid, &sidText) && sidText) {
                    ok = std::string(sidText) == "S-1-5-18";
                    LocalFree(sidText);
                }
            }
            CloseHandle(token);
            return ok;
#else
            return false;
#endif
        }

        static DWORD ReadSoftwareSasGenerationValue(bool* exists = nullptr) {
#ifdef _WIN32
            if (exists) *exists = false;
            HKEY key = nullptr;
            LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System)", 0, KEY_READ, &key);
            if (rc != ERROR_SUCCESS) return 0;
            DWORD value = 0;
            DWORD type = 0;
            DWORD cb = sizeof(value);
            rc = RegQueryValueExW(key, L"SoftwareSASGeneration", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &cb);
            RegCloseKey(key);
            if (rc == ERROR_SUCCESS && type == REG_DWORD) {
                if (exists) *exists = true;
                return value;
            }
            return 0;
#else
            if (exists) *exists = false;
            return 0;
#endif
        }

        struct DisplayGeometrySnapshot {
            int primaryWidth = 0;
            int primaryHeight = 0;
            int virtualX = 0;
            int virtualY = 0;
            int virtualWidth = 0;
            int virtualHeight = 0;

            bool operator==(const DisplayGeometrySnapshot& other) const {
                return primaryWidth == other.primaryWidth && primaryHeight == other.primaryHeight &&
                    virtualX == other.virtualX && virtualY == other.virtualY &&
                    virtualWidth == other.virtualWidth && virtualHeight == other.virtualHeight;
            }
            bool operator!=(const DisplayGeometrySnapshot& other) const { return !(*this == other); }
        };

        static DisplayGeometrySnapshot ReadDisplayGeometrySnapshot() {
            DisplayGeometrySnapshot out{};
            out.primaryWidth = GetSystemMetrics(SM_CXSCREEN);
            out.primaryHeight = GetSystemMetrics(SM_CYSCREEN);
            out.virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            out.virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            out.virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            out.virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            return out;
        }

        static void LogDisplayGeometry(const std::string& sessionId, const std::string& phase, const DisplayGeometrySnapshot& value) {
            LogI("[display-state] phase=" + phase + " session=" + sessionId +
                " primary=" + std::to_string(value.primaryWidth) + "x" + std::to_string(value.primaryHeight) +
                " virtual=" + std::to_string(value.virtualWidth) + "x" + std::to_string(value.virtualHeight) +
                " origin=" + std::to_string(value.virtualX) + "," + std::to_string(value.virtualY));
        }

        static std::string WtsStateName(WTS_CONNECTSTATE_CLASS state) {
            switch (state) {
            case WTSActive: return "Active";
            case WTSConnected: return "Connected";
            case WTSConnectQuery: return "ConnectQuery";
            case WTSShadow: return "Shadow";
            case WTSDisconnected: return "Disconnected";
            case WTSIdle: return "Idle";
            case WTSListen: return "Listen";
            case WTSReset: return "Reset";
            case WTSDown: return "Down";
            case WTSInit: return "Init";
            default: return "Unknown";
            }
        }

        static void CadLogSessionDiagnostics(const std::string& sessionId) {
#ifdef _WIN32
            DWORD processSession = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &processSession);
            const DWORD active = ActiveConsoleSessionId();
            CadLog("diagnostics session=" + sessionId +
                " process_session=" + std::to_string(processSession) +
                " active_console_session=" + SessionIdToString(active) +
                " local_system=" + BoolText(IsRunningAsLocalSystem()));

            bool sasExists = false;
            DWORD sasValue = ReadSoftwareSasGenerationValue(&sasExists);
            CadLog("diagnostics SoftwareSASGeneration exists=" + BoolText(sasExists) +
                " value=" + std::to_string(sasValue));

            if (active != 0xFFFFFFFF) {
                LPWSTR raw = nullptr;
                DWORD bytes = 0;
                if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, active, WTSConnectState, &raw, &bytes) && raw && bytes >= sizeof(WTS_CONNECTSTATE_CLASS)) {
                    auto state = *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(raw);
                    CadLog("diagnostics active_session_state=" + WtsStateName(state));
                    WTSFreeMemory(raw);
                }
                else {
                    CadLog("diagnostics WTSConnectState failed err=" + LastErrorText(GetLastError()));
                }
                raw = nullptr; bytes = 0;
                if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, active, WTSUserName, &raw, &bytes) && raw) {
                    std::wstring w(raw);
                    std::string user(w.begin(), w.end());
                    CadLog("diagnostics active_session_user=" + user);
                    WTSFreeMemory(raw);
                }
            }
#endif
        }

        struct SoftwareSasGenerationBackup {
            bool changed = false;
            bool hadValue = false;
            DWORD oldValue = 0;
        };

        static bool EnsureTemporarySoftwareSasForServices(SoftwareSasGenerationBackup& backup) {
#ifdef _WIN32
            constexpr const wchar_t* kPath = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System)";
            constexpr const wchar_t* kName = L"SoftwareSASGeneration";

            backup = SoftwareSasGenerationBackup{};

            HKEY key = nullptr;
            DWORD disposition = 0;
            LONG rc = RegCreateKeyExW(
                HKEY_LOCAL_MACHINE,
                kPath,
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_QUERY_VALUE | KEY_SET_VALUE,
                nullptr,
                &key,
                &disposition);

            if (rc != ERROR_SUCCESS || !key) {
                CadLog("temporary SoftwareSASGeneration RegCreateKeyExW failed err=" + LastErrorText(static_cast<DWORD>(rc)));
                return false;
            }

            DWORD value = 0;
            DWORD type = 0;
            DWORD cb = sizeof(value);
            rc = RegQueryValueExW(key, kName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &cb);
            if (rc == ERROR_SUCCESS && type == REG_DWORD) {
                backup.hadValue = true;
                backup.oldValue = value;
                CadLog("temporary SoftwareSASGeneration previous exists=true value=" + std::to_string(value));
            }
            else {
                backup.hadValue = false;
                backup.oldValue = 0;
                CadLog("temporary SoftwareSASGeneration previous exists=false query_rc=" + std::to_string(rc));
            }

            if (backup.hadValue) {
                if ((backup.oldValue & 0x1u) != 0) {
                    CadLog("SoftwareSASGeneration already allows Services; leaving administrator policy unchanged");
                    RegCloseKey(key);
                    return true;
                }
                CadLog("SoftwareSASGeneration exists and does not allow Services; respecting explicit administrator policy");
                RegCloseKey(key);
                return false;
            }

            const DWORD newValue = 1u;
            rc = RegSetValueExW(key, kName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&newValue), sizeof(newValue));
            RegCloseKey(key);

            if (rc != ERROR_SUCCESS) {
                CadLog("temporary SoftwareSASGeneration set failed value=" + std::to_string(newValue) + " err=" + LastErrorText(static_cast<DWORD>(rc)));
                return false;
            }

            backup.changed = true;
            CadLog("temporary SoftwareSASGeneration set value=" + std::to_string(newValue) + " services_allowed=true");
            return true;
#else
            (void)backup;
            return false;
#endif
        }

        static void RestoreTemporarySoftwareSasGeneration(const SoftwareSasGenerationBackup& backup) {
#ifdef _WIN32
            if (!backup.changed) {
                CadLog("temporary SoftwareSASGeneration restore skipped changed=false");
                return;
            }

            constexpr const wchar_t* kPath = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System)";
            constexpr const wchar_t* kName = L"SoftwareSASGeneration";

            HKEY key = nullptr;
            LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPath, 0, KEY_SET_VALUE, &key);
            if (rc != ERROR_SUCCESS || !key) {
                CadLog("temporary SoftwareSASGeneration restore RegOpenKeyExW failed err=" + LastErrorText(static_cast<DWORD>(rc)));
                return;
            }

            if (backup.hadValue) {
                DWORD oldValue = backup.oldValue;
                rc = RegSetValueExW(key, kName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&oldValue), sizeof(oldValue));
                CadLog("temporary SoftwareSASGeneration restore previous value=" + std::to_string(oldValue) + " rc=" + std::to_string(rc));
            }
            else {
                rc = RegDeleteValueW(key, kName);
                if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
                CadLog("temporary SoftwareSASGeneration deleted temporary value rc=" + std::to_string(rc));
            }

            RegCloseKey(key);
#else
            (void)backup;
#endif
        }

        static bool InvokeSendSas(const std::string& sessionId, const std::string& pathLabel, BOOL asUser) {
#ifdef _WIN32
            using SendSasFn = void (WINAPI*)(BOOL);
            SetLastError(0);
            HMODULE sas = LoadLibraryW(L"sas.dll");
            if (!sas) {
                CadLog(pathLabel + " LoadLibrary(sas.dll) failed session=" + sessionId + " err=" + LastErrorText(GetLastError()));
                return false;
            }
            auto fn = reinterpret_cast<SendSasFn>(GetProcAddress(sas, "SendSAS"));
            if (!fn) {
                const DWORD err = GetLastError();
                FreeLibrary(sas);
                CadLog(pathLabel + " GetProcAddress(SendSAS) failed session=" + sessionId + " err=" + LastErrorText(err));
                return false;
            }

            CadLog(pathLabel + " calling SendSAS(" + std::string(asUser ? "TRUE" : "FALSE") + ") session=" + sessionId);
            fn(asUser);
            Sleep(150);
            CadLog(pathLabel + " SendSAS call returned session=" + sessionId);

            FreeLibrary(sas);
            return true;
#else
            (void)sessionId; (void)pathLabel; (void)asUser;
            return false;
#endif
        }

        static bool TrySendSecureAttentionSequenceFromService(const std::string& sessionId) {
#ifdef _WIN32
            CadLog("==== CAD request begin session=" + sessionId + " ====");
            CadLog("CAD_BUILD=PolicySafe-SoftwareSASGeneration-SendSAS active=true");
            CadLogSessionDiagnostics(sessionId);

            SoftwareSasGenerationBackup backup;
            const bool policyReady = EnsureTemporarySoftwareSasForServices(backup);
            CadLog("temporary SoftwareSASGeneration policy_ready=" + BoolText(policyReady));

            bool ok = false;
            if (policyReady) {
                ok = InvokeSendSas(sessionId, "service", FALSE);
            }
            else {
                CadLog("service SendSAS skipped because Services SAS permission is unavailable or explicitly blocked");
            }

            RestoreTemporarySoftwareSasGeneration(backup);

            CadLog("CAD request complete session=" + sessionId +
                " attempted=" + BoolText(policyReady) +
                " send_sas_path_returned=" + BoolText(ok));
            CadLog("NOTE: SoftwareSASGeneration is restored/deleted immediately after SendSAS. SendSAS returns void; verify by observing the secure screen.");
            CadLog("==== CAD request end session=" + sessionId + " ====");
            return ok;
#else
            (void)sessionId;
            return false;
#endif
        }


        static std::string ShortSessionPrefixForOverlay(const std::string& sessionId) {
            return sessionId.substr(0, std::min<size_t>(24, sessionId.size()));
        }

        static std::string ChatOverlayInPipeName(const std::string& sessionId) {
            return "\\\\.\\pipe\\Hi5ChatIn_" + ShortSessionPrefixForOverlay(sessionId);
        }

        static std::string ChatOverlayOutPipeName(const std::string& sessionId) {
            return "\\\\.\\pipe\\Hi5ChatOut_" + ShortSessionPrefixForOverlay(sessionId);
        }

        static std::string SessionBannerStopEventName(const std::string& sessionId) {
            return "Global\\Hi5BannerStop_" + SafeFilePart(ShortSessionPrefixForOverlay(sessionId));
        }

        static std::string SessionBannerChatEventName(const std::string& sessionId) {
            return "Global\\Hi5BannerChat_" + SafeFilePart(ShortSessionPrefixForOverlay(sessionId));
        }

        static std::string SessionBannerEndEventName(const std::string& sessionId) {
            return "Global\\Hi5BannerEnd_" + SafeFilePart(ShortSessionPrefixForOverlay(sessionId));
        }

        static std::string SessionChatStopEventName(const std::string& sessionId) {
            return "Global\\Hi5ChatOverlayStop_" + SafeFilePart(ShortSessionPrefixForOverlay(sessionId));
        }

        static HANDLE CreateOrOpenManualResetEventA(const std::string& name, bool initiallySignaled = false) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = FALSE;
            // Allow the LocalSystem service and the interactive user-session helper to share
            // the same Global\ event. Without this DACL, a banner/chat helper running as
            // the logged-on user may fail to open the event created by LocalSystem.
            if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;WD)",
                SDDL_REVISION_1,
                &sd,
                nullptr)) {
                sa.lpSecurityDescriptor = sd;
            }

            HANDLE h = CreateEventA(sa.lpSecurityDescriptor ? &sa : nullptr, TRUE, initiallySignaled ? TRUE : FALSE, name.c_str());
            if (sd) LocalFree(sd);
            if (!h) {
                LogE("[ui-cleanup] CreateEvent failed name=" + name + " err=" + std::to_string(GetLastError()));
            }
            return h;
        }

        static std::wstring ToWidePath(const std::string& s) {
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring w(std::max(0, n - 1), L'\0');
            if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
            return w;
        }

        static bool RunHiddenProcessAndWait(const std::wstring& commandLine, DWORD timeoutMs = 7000) {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            std::wstring mutableCmd = commandLine;
            BOOL ok = CreateProcessW(nullptr,
                mutableCmd.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi);
            if (!ok) {
                LogW("[ui-cleanup] CreateProcess cleanup failed err=" + std::to_string(GetLastError()));
                return false;
            }
            CloseHandle(pi.hThread);
            DWORD waitRc = WaitForSingleObject(pi.hProcess, timeoutMs);
            if (waitRc == WAIT_TIMEOUT) {
                LogW("[ui-cleanup] cleanup command timed out, terminating cleanup process");
                TerminateProcess(pi.hProcess, 124);
                WaitForSingleObject(pi.hProcess, 1000);
            }
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            return exitCode == 0 || exitCode == STILL_ACTIVE;
        }

        static void CleanupOrphanUiHelperProcesses() {
            // Last-resort cleanup for helper processes launched by previous builds or for cases
            // where the control server/viewer closes without delivering a clean session-ended
            // message. This intentionally targets only helper modes, not the service itself.
            const std::wstring ps =
                L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
                L"$ErrorActionPreference='SilentlyContinue'; "
                L"Get-CimInstance Win32_Process | "
                L"Where-Object { ($_.Name -eq 'Hi5CentralUser.exe' -or $_.Name -eq 'Hi5CentralAgentService.exe' -or $_.Name -eq 'Hi5CentralAgent.exe' -or $_.Name -eq 'native_vp8_stream.exe') -and $_.CommandLine -match '--mode\\\\s+(banner|chat-overlay|native-chat)' } | "
                L"ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
                L"\"";
            LogI("[ui-cleanup] killing orphan banner/chat-overlay helper processes");
            RunHiddenProcessAndWait(ps);
        }


        static std::string EscapePowerShellSingleQuoted(const std::string& value) {
            std::string out;
            out.reserve(value.size() + 8);
            for (char ch : value) {
                if (ch == '\'') out += "''";
                else out += ch;
            }
            return out;
        }

        static void CleanupOrphanStreamerProcesses(const std::string& sessionId = std::string()) {
            // Last-resort cleanup for session-scoped helpers. Capture workers remain
            // Hi5CentralRemoteHost.exe --mode streamer; WebRTC/codecs/audio now live in the
            // dedicated Hi5CentralMediaHost.exe. Neither process may outlive its session.
            std::wstring ps =
                L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
                L"$ErrorActionPreference='SilentlyContinue'; "
                L"$procs = Get-CimInstance Win32_Process | "
                L"Where-Object { $_.Name -eq 'Hi5CentralMediaHost.exe' -or "
                L"(($_.Name -eq 'Hi5CentralRemoteHost.exe' -or $_.Name -eq 'Hi5CentralAgentService.exe' -or $_.Name -eq 'Hi5CentralAgent.exe' -or $_.Name -eq 'native_vp8_stream.exe') -and $_.CommandLine -match '--mode\\\\s+streamer|--mode=streamer') }; ";

            if (!sessionId.empty()) {
                const std::wstring wsid = ToWidePath(EscapePowerShellSingleQuoted(sessionId));
                ps += L"$sid='" + wsid + L"'; $procs = $procs | Where-Object { $_.CommandLine -like ('*' + $sid + '*') }; ";
            }

            ps +=
                L"$procs | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
                L"\"";

            if (sessionId.empty()) {
                LogI("[streamer-cleanup] killing orphan streamer helper processes");
            }
            else {
                LogI("[streamer-cleanup] killing streamer helper processes for session=" + sessionId);
            }
            RunHiddenProcessAndWait(ps, 10000);
        }

        static HANDLE CreateSessionJobObject(const std::string& sessionId) {
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (!job) {
                LogW("[session-job] CreateJobObject failed session=" + sessionId + " err=" + std::to_string(GetLastError()));
                return nullptr;
            }

            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                LogW("[session-job] SetInformationJobObject failed session=" + sessionId + " err=" + std::to_string(GetLastError()));
                CloseHandle(job);
                return nullptr;
            }

            LogI("[session-job] created session=" + sessionId);
            return job;
        }

        static bool AssignProcessToSessionJob(HANDLE job, HANDLE process, const std::string& sessionId, const char* role) {
            if (!job || !process) return false;
            if (AssignProcessToJobObject(job, process)) {
                LogI("[session-job] assigned role=" + std::string(role ? role : "helper") + " session=" + sessionId +
                    " pid=" + std::to_string(GetProcessId(process)));
                return true;
            }
            LogW("[session-job] assign failed role=" + std::string(role ? role : "helper") + " session=" + sessionId +
                " pid=" + std::to_string(GetProcessId(process)) + " err=" + std::to_string(GetLastError()));
            return false;
        }

        static void TrimIdleWorkingSet() {
            if (EmptyWorkingSet(GetCurrentProcess())) {
                LogI("[memory] trimmed service working set after last remote session");
            }
            else {
                LogW("[memory] EmptyWorkingSet failed err=" + std::to_string(GetLastError()));
            }
        }

        struct ProcessMemorySnapshot {
            SIZE_T workingSetBytes = 0;
            SIZE_T privateBytes = 0;
            SIZE_T peakWorkingSetBytes = 0;
        };

        static bool QueryProcessMemory(HANDLE process, ProcessMemorySnapshot& out) {
            if (!process) return false;
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            if (!GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
                return false;
            }
            out.workingSetBytes = counters.WorkingSetSize;
            out.privateBytes = counters.PrivateUsage;
            out.peakWorkingSetBytes = counters.PeakWorkingSetSize;
            return true;
        }

        static void LogProcessMemorySnapshot(const std::string& sessionId, const char* role, HANDLE process) {
            ProcessMemorySnapshot memory{};
            if (!QueryProcessMemory(process, memory)) return;
            constexpr SIZE_T kMiB = 1024ull * 1024ull;
            LogI("[memory] session=" + sessionId +
                " role=" + std::string(role ? role : "unknown") +
                " pid=" + std::to_string(GetProcessId(process)) +
                " working_set_mb=" + std::to_string(memory.workingSetBytes / kMiB) +
                " private_mb=" + std::to_string(memory.privateBytes / kMiB) +
                " peak_working_set_mb=" + std::to_string(memory.peakWorkingSetBytes / kMiB) +
                " working_set_bytes=" + std::to_string(static_cast<unsigned long long>(memory.workingSetBytes)) +
                " private_bytes=" + std::to_string(static_cast<unsigned long long>(memory.privateBytes)));
        }

        static std::string SiblingExecutablePath(const char* fileName) {
            const std::string current = CurrentExePath();
            if (current.empty() || !fileName || !*fileName) return current;
            std::filesystem::path candidate = std::filesystem::path(current).parent_path() / fileName;
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) return candidate.string();
            return current; // rollback compatibility for older/single-binary installs
        }

        static std::string UserHostExePath() {
            return SiblingExecutablePath("Hi5CentralUser.exe");
        }

        static HANDLE LaunchUserHostFeature(const std::string& featureArgs) {
            const std::string exe = UserHostExePath();
            if (exe.empty()) return nullptr;
            const DWORD windowsSession = WTSGetActiveConsoleSessionId();
            if (windowsSession == 0xFFFFFFFF) return nullptr;

            const std::string hostArgs = "--mode user-host --windows-session " + QuoteArg(std::to_string(windowsSession));
            HANDLE hostProbe = LaunchInInteractiveSession(exe, hostArgs);
            if (hostProbe) {
                WaitForSingleObject(hostProbe, 250);
                CloseHandle(hostProbe);
            }
            Sleep(75);
            return LaunchInInteractiveSession(exe, featureArgs);
        }

        static void StopResidentUserHosts() {
            const std::wstring ps =
                L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
                L"$ErrorActionPreference='SilentlyContinue'; "
                L"Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'Hi5CentralUser.exe' -and $_.CommandLine -match '--mode\\s+user-host' } | "
                L"ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
                L"\"";
            RunHiddenProcessAndWait(ps, 5000);
        }

        static std::string RemoteHostExePath() {
            return SiblingExecutablePath("Hi5CentralRemoteHost.exe");
        }

        static std::string MediaHostExePath() {
            return SiblingExecutablePath("Hi5CentralMediaHost.exe");
        }

        static std::string PatchHostExePath() {
            const std::string current = CurrentExePath();
            if (current.empty()) return {};
            const std::filesystem::path candidate =
                std::filesystem::path(current).parent_path() / "Hi5CentralPatchHost.exe";
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec)) return {};
            return candidate.string();
        }

        static HANDLE LaunchServiceChildProcess(const std::string& exePath, const std::string& args) {
            std::wstring command = L"\"" + WideFromUtf8(exePath) + L"\" " + WideFromUtf8(args);
            std::wstring workDir = WideFromUtf8(DirOfPath(exePath));
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
            if (!ok) return nullptr;
            CloseHandle(pi.hThread);
            return pi.hProcess;
        }

        static std::string PatchSafeId(const std::string& value) {
            std::string out;
            out.reserve(value.size());
            for (const unsigned char ch : value) {
                if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') out.push_back(static_cast<char>(ch));
            }
            if (out.empty()) out = "patch";
            if (out.size() > 96) out.resize(96);
            return out;
        }

        static bool ApplyPatchFileAcl(const std::filesystem::path& path) {
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;FA;;;SY)(A;;FA;;;BA)",
                SDDL_REVISION_1,
                &descriptor,
                nullptr)) {
                return false;
            }
            const BOOL ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, descriptor);
            LocalFree(descriptor);
            return ok == TRUE;
        }

        static bool ApplyPatchDirectoryAcl(const std::filesystem::path& path) {
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                SDDL_REVISION_1,
                &descriptor,
                nullptr)) {
                return false;
            }
            const BOOL ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, descriptor);
            LocalFree(descriptor);
            return ok == TRUE;
        }

        static bool WriteProtectedPatchManifest(const std::filesystem::path& path, const json& manifest) {
            const std::string plain = manifest.dump();
            DATA_BLOB input{};
            input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain.data()));
            input.cbData = static_cast<DWORD>(plain.size());

            DATA_BLOB output{};
            const DWORD flags = CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN;
            if (!CryptProtectData(&input, L"Hi5Central Patch Manifest", nullptr, nullptr, nullptr, flags, &output)) {
                return false;
            }

            bool written = false;
            try {
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                if (stream) {
                    stream.write(reinterpret_cast<const char*>(output.pbData), static_cast<std::streamsize>(output.cbData));
                    written = stream.good();
                }
            } catch (...) {
                written = false;
            }
            LocalFree(output.pbData);
            if (!written) return false;
            return ApplyPatchFileAcl(path);
        }

        static json RunPatchHostSoftwareJob(
            const AgentIdentity& ident,
            const std::string& jobId,
            const json& payload,
            std::string& errorMessage) {

            const std::string patchHost = PatchHostExePath();
            if (patchHost.empty()) {
                errorMessage = "Hi5CentralPatchHost.exe is not installed.";
                return json::object();
            }

            namespace fs = std::filesystem;
            const fs::path root = fs::path(LR"(C:\ProgramData\Hi5Central\Agent\PatchHost)");
            std::error_code ec;
            fs::create_directories(root, ec);
            if (ec) {
                errorMessage = "Unable to create PatchHost working directory.";
                return json::object();
            }
            if (!ApplyPatchDirectoryAcl(root)) {
                errorMessage = "Unable to secure PatchHost working directory.";
                return json::object();
            }

            const std::string safeId = PatchSafeId(jobId);
            const fs::path manifestPath = root / Utf8ToWide(safeId + ".manifest.dpapi");
            const fs::path resultPath = root / Utf8ToWide(safeId + ".result.json");
            fs::remove(manifestPath, ec);
            ec.clear();
            fs::remove(resultPath, ec);

            json manifest = payload.is_object() ? payload : json::object();
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            manifest["protocolVersion"] = 1;
            manifest["action"] = payload.value("action", std::string("software.install"));
            manifest["jobId"] = jobId;
            manifest["deviceId"] = ident.deviceId;
            manifest["issuedUnixMs"] = nowMs;
            manifest["expiresUnixMs"] = nowMs + (10LL * 60LL * 1000LL);

            if (!WriteProtectedPatchManifest(manifestPath, manifest)) {
                errorMessage = "Unable to protect PatchHost manifest.";
                fs::remove(manifestPath, ec);
                return json::object();
            }

            const std::string args =
                "--execute-manifest " + QuoteArg(manifestPath.string()) +
                " --output " + QuoteArg(resultPath.string());

            HANDLE process = LaunchServiceChildProcess(patchHost, args);
            if (!process) {
                errorMessage = "Unable to launch Hi5CentralPatchHost.exe.";
                fs::remove(manifestPath, ec);
                return json::object();
            }

            const DWORD wait = WaitForSingleObject(process, 35 * 60 * 1000);
            DWORD exitCode = 1;
            if (wait == WAIT_TIMEOUT) {
                TerminateProcess(process, ERROR_TIMEOUT);
                exitCode = ERROR_TIMEOUT;
                errorMessage = "PatchHost execution timed out.";
            } else if (!GetExitCodeProcess(process, &exitCode)) {
                exitCode = GetLastError();
                errorMessage = "Unable to read PatchHost exit code.";
            }
            CloseHandle(process);

            std::string resultText;
            try {
                std::ifstream stream(resultPath, std::ios::binary);
                if (stream) {
                    std::ostringstream buffer;
                    buffer << stream.rdbuf();
                    resultText = buffer.str();
                }
            } catch (...) {}

            fs::remove(manifestPath, ec);
            ec.clear();
            fs::remove(resultPath, ec);

            if (resultText.empty()) {
                if (errorMessage.empty()) errorMessage = "PatchHost did not return a result.";
                return json{
                    {"success", false},
                    {"patchHostExitCode", static_cast<int>(exitCode)}
                };
            }

            json result = json::parse(resultText, nullptr, false);
            if (result.is_discarded() || !result.is_object()) {
                errorMessage = "PatchHost returned invalid JSON.";
                return json{
                    {"success", false},
                    {"patchHostExitCode", static_cast<int>(exitCode)}
                };
            }
            result["patchHostExitCode"] = static_cast<int>(exitCode);
            if (!result.value("success", false) && errorMessage.empty()) {
                errorMessage = result.value("error", std::string("PatchHost reported failure."));
            }
            return result;
        }

        static std::string ChatOverlayExePath() {
            return UserHostExePath();
        }

        struct ChatOverlayState {
            std::string sessionId;
            std::string inPipeName;   // service -> overlay
            std::string outPipeName;  // overlay -> service
            HANDLE process = nullptr;
            HANDLE inPipe = INVALID_HANDLE_VALUE;
            HANDLE outPipe = INVALID_HANDLE_VALUE;
            HANDLE stopEvent = nullptr;
            std::string stopEventName;
            std::atomic<bool> stop{ false };
            std::thread inServerThread;
            std::thread outServerThread;
            std::mutex mu;
            std::vector<std::string> pendingToOverlay;
            bool inConnected = false;
            bool overlayReady = false;
            bool userDismissed = false;
        };

        static std::mutex g_chatOverlayMu;
        static std::unordered_map<std::string, std::unique_ptr<ChatOverlayState>> g_chatOverlays;
        using ChatOverlayReplyCallback = std::function<void(const std::string& sessionId, const std::string& body)>;


        struct PresenceBannerState {
            std::string sessionId;
            HANDLE process = nullptr;
            HANDLE stopEvent = nullptr;
            HANDLE chatEvent = nullptr;
            HANDLE endEvent = nullptr;
            std::string stopEventName;
            std::string chatEventName;
            std::string endEventName;
        };

        static std::mutex g_presenceBannerMu;
        static std::unordered_map<std::string, PresenceBannerState> g_presenceBanners;

        static void StopPresenceBanner(const std::string& sessionId) {
            PresenceBannerState state;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(g_presenceBannerMu);
                auto it = g_presenceBanners.find(sessionId);
                if (it != g_presenceBanners.end()) {
                    state = it->second;
                    g_presenceBanners.erase(it);
                    found = true;
                }
            }

            if (!found) return;

            LogI("[presence] stopping banner session=" + sessionId);
            if (state.stopEvent) SetEvent(state.stopEvent);
            if (state.process) {
                // Presence UI must never pause video handoff for seconds. It normally
                // exits immediately on its stop event; give it a short grace period,
                // then terminate the UI-only helper and continue the live stream.
                const DWORD waitRc = WaitForSingleObject(state.process, 150);
                if (waitRc == WAIT_TIMEOUT) {
                    LogW("[presence] banner did not exit promptly, terminating session=" + sessionId);
                    TerminateProcess(state.process, 0);
                    WaitForSingleObject(state.process, 100);
                }
                CloseHandle(state.process);
            }
            if (state.stopEvent) CloseHandle(state.stopEvent);
            if (state.chatEvent) CloseHandle(state.chatEvent);
            if (state.endEvent) CloseHandle(state.endEvent);
        }

        static void StopPresenceBanners() {
            std::vector<std::string> ids;
            {
                std::lock_guard<std::mutex> lock(g_presenceBannerMu);
                for (const auto& kv : g_presenceBanners) ids.push_back(kv.first);
            }
            for (const auto& id : ids) StopPresenceBanner(id);
        }

        static void StartPresenceBanner(const std::string& sessionId, const std::string& technicianName, HANDLE sessionJob, bool notifyOnStart = true) {
            if (sessionId.empty()) return;

            StopPresenceBanner(sessionId);

            const DWORD consoleSession = ActiveConsoleSessionId();
            if (!InteractiveUserSessionReady(consoleSession)) {
                LogI("[presence] deferred until interactive user desktop session=" + sessionId +
                    " console=" + SessionIdToString(consoleSession));
                return;
            }

            const std::string exe = UserHostExePath();
            if (exe.empty()) {
                LogE("[presence] cannot resolve user host executable path");
                return;
            }

            const std::string safeTech = technicianName.empty() ? "Technician" : technicianName;
            const std::string stopEventName = SessionBannerStopEventName(sessionId);
            const std::string chatEventName = SessionBannerChatEventName(sessionId);
            const std::string endEventName = SessionBannerEndEventName(sessionId);
            HANDLE stopEvent = CreateOrOpenManualResetEventA(stopEventName, false);
            HANDLE chatEvent = CreateOrOpenManualResetEventA(chatEventName, false);
            HANDLE endEvent = CreateOrOpenManualResetEventA(endEventName, false);
            if (stopEvent) ResetEvent(stopEvent);
            if (chatEvent) ResetEvent(chatEvent);
            if (endEvent) ResetEvent(endEvent);
            const std::string args = "--mode banner --session " + QuoteArg(sessionId) +
                " --technician " + QuoteArg(safeTech) +
                " --stop-event " + QuoteArg(stopEventName) +
                " --chat-event " + QuoteArg(chatEventName) +
                " --end-event " + QuoteArg(endEventName) +
                " --notify " + std::string(notifyOnStart ? "1" : "0");

            LogI("[presence] launching edge support panel session=" + sessionId +
                " technician=" + safeTech +
                " notify=" + std::string(notifyOnStart ? "true" : "false"));
            HANDLE proc = LaunchUserHostFeature(args);
            if (!proc) {
                LogE("[presence] banner launch failed session=" + sessionId);
                if (stopEvent) CloseHandle(stopEvent);
                if (chatEvent) CloseHandle(chatEvent);
                if (endEvent) CloseHandle(endEvent);
                return;
            }
            WaitForSingleObject(proc, 1000);
            CloseHandle(proc);

            PresenceBannerState state;
            state.sessionId = sessionId;
            state.process = nullptr;
            state.stopEvent = stopEvent;
            state.chatEvent = chatEvent;
            state.endEvent = endEvent;
            state.stopEventName = stopEventName;
            state.chatEventName = chatEventName;
            state.endEventName = endEventName;
            {
                std::lock_guard<std::mutex> lock(g_presenceBannerMu);
                g_presenceBanners[sessionId] = state;
            }
        }

        static bool ConsumePresenceBannerAction(const std::string& sessionId, bool chatAction) {
            std::lock_guard<std::mutex> lock(g_presenceBannerMu);
            auto it = g_presenceBanners.find(sessionId);
            if (it == g_presenceBanners.end()) return false;
            HANDLE eventHandle = chatAction ? it->second.chatEvent : it->second.endEvent;
            if (!eventHandle) return false;
            if (WaitForSingleObject(eventHandle, 0) != WAIT_OBJECT_0) return false;
            ResetEvent(eventHandle);
            return true;
        }


        static bool WriteLineToPipe(HANDLE pipe, const std::string& line) {
            if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;
            std::string data = line;
            if (data.empty() || data.back() != '\n') data.push_back('\n');
            DWORD written = 0;
            BOOL ok = WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            return ok && written == data.size();
        }

        static HANDLE CreateMessagePipeServer(const std::string& pipeName, DWORD openMode) {
            const std::wstring wName = ToWidePath(pipeName);
            PSECURITY_DESCRIPTOR sd = nullptr;
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = FALSE;
            if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;WD)",
                SDDL_REVISION_1, &sd, nullptr)) {
                sa.lpSecurityDescriptor = sd;
            }
            HANDLE pipe = CreateNamedPipeW(
                wName.c_str(), openMode,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                1, 65536, 65536, 0, sa.lpSecurityDescriptor ? &sa : nullptr);
            if (sd) LocalFree(sd);
            return pipe;
        }

        static void FlushQueuedChatToOverlay(ChatOverlayState* state) {
            if (!state) return;
            std::vector<std::string> copy;
            HANDLE pipe = INVALID_HANDLE_VALUE;
            {
                std::lock_guard<std::mutex> lock(state->mu);
                if (!state->inConnected || state->inPipe == INVALID_HANDLE_VALUE) return;
                pipe = state->inPipe;
                copy.swap(state->pendingToOverlay);
            }
            for (const auto& line : copy) {
                if (!WriteLineToPipe(pipe, line)) {
                    LogW("[chat-ui] write to overlay failed session=" + state->sessionId + " err=" + std::to_string(GetLastError()));
                    std::lock_guard<std::mutex> lock(state->mu);
                    state->pendingToOverlay.push_back(line);
                    break;
                }
            }
        }

        static void ChatOverlayInServerLoop(ChatOverlayState* state) {
            HANDLE pipe = CreateMessagePipeServer(state->inPipeName, PIPE_ACCESS_OUTBOUND);
            if (pipe == INVALID_HANDLE_VALUE) {
                LogE("[chat-ui] CreateNamedPipeW(in) failed session=" + state->sessionId + " err=" + std::to_string(GetLastError()));
                return;
            }

            {
                std::lock_guard<std::mutex> lock(state->mu);
                state->inPipe = pipe;
            }

            LogI("[chat-ui] waiting overlay input pipe session=" + state->sessionId + " pipe=" + state->inPipeName);
            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) {
                if (!state->stop.load()) LogE("[chat-ui] ConnectNamedPipe(in) failed session=" + state->sessionId + " err=" + std::to_string(GetLastError()));
                return;
            }

            {
                std::lock_guard<std::mutex> lock(state->mu);
                state->inConnected = true;
            }
            LogI("[chat-ui] overlay input pipe connected session=" + state->sessionId);
            FlushQueuedChatToOverlay(state);

            while (!state->stop.load()) {
                Sleep(100);
            }
        }

        static void ChatOverlayOutServerLoop(ChatOverlayState* state, ChatOverlayReplyCallback cb) {
            HANDLE pipe = CreateMessagePipeServer(state->outPipeName, PIPE_ACCESS_INBOUND);
            if (pipe == INVALID_HANDLE_VALUE) {
                LogE("[chat-ui] CreateNamedPipeW(out) failed session=" + state->sessionId + " err=" + std::to_string(GetLastError()));
                return;
            }

            {
                std::lock_guard<std::mutex> lock(state->mu);
                state->outPipe = pipe;
            }

            LogI("[chat-ui] waiting overlay output pipe session=" + state->sessionId + " pipe=" + state->outPipeName);
            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) {
                if (!state->stop.load()) LogE("[chat-ui] ConnectNamedPipe(out) failed session=" + state->sessionId + " err=" + std::to_string(GetLastError()));
                return;
            }
            LogI("[chat-ui] overlay output pipe connected session=" + state->sessionId);

            std::string pending;
            char buf[8192];
            while (!state->stop.load()) {
                DWORD got = 0;
                BOOL ok = ReadFile(pipe, buf, sizeof(buf), &got, nullptr);
                if (!ok || got == 0) {
                    DWORD err = GetLastError();
                    if (!state->stop.load()) LogW("[chat-ui] output pipe read stopped session=" + state->sessionId + " err=" + std::to_string(err));
                    break;
                }
                pending.append(buf, buf + got);
                for (;;) {
                    auto pos = pending.find('\n');
                    if (pos == std::string::npos) break;
                    std::string line = pending.substr(0, pos);
                    pending.erase(0, pos + 1);
                    auto j = json::parse(line, nullptr, false);
                    if (j.is_discarded()) continue;
                    const std::string type = j.value("type", "");
                    if (type == "overlay_ready") {
                        {
                            std::lock_guard<std::mutex> lock(state->mu);
                            state->overlayReady = true;
                        }
                        LogI("[chat-ui] overlay ready session=" + state->sessionId);
                        FlushQueuedChatToOverlay(state);
                    }
                    else if (type == "chat_message" && j.value("sender", "") == "user") {
                        const std::string body = j.value("body", std::string());
                        if (!body.empty() && cb) cb(state->sessionId, body);
                    }
                    else if (type == "dismissed" || type == "closed") {
                        {
                            std::lock_guard<std::mutex> lock(state->mu);
                            state->userDismissed = true;
                        }
                        LogI("[chat-ui] overlay dismissed by user session=" + state->sessionId);
                    }
                }
            }
        }

        static ChatOverlayState* EnsureChatOverlay(const std::string& sessionId, HANDLE sessionJob, ChatOverlayReplyCallback cb) {
            std::lock_guard<std::mutex> lock(g_chatOverlayMu);
            auto found = g_chatOverlays.find(sessionId);
            if (found != g_chatOverlays.end() && found->second) {
                return found->second.get();
            }

            auto state = std::make_unique<ChatOverlayState>();
            state->sessionId = sessionId;
            state->inPipeName = ChatOverlayInPipeName(sessionId);
            state->outPipeName = ChatOverlayOutPipeName(sessionId);
            state->stopEventName = SessionChatStopEventName(sessionId);
            state->stopEvent = CreateOrOpenManualResetEventA(state->stopEventName, false);
            if (state->stopEvent) ResetEvent(state->stopEvent);

            const std::string overlayExe = ChatOverlayExePath();
            std::error_code ec;
            if (!std::filesystem::exists(overlayExe, ec)) {
                LogE("[chat-ui] native_vp8_stream.exe missing path=" + overlayExe);
                return nullptr;
            }

            auto* raw = state.get();
            raw->inServerThread = std::thread(ChatOverlayInServerLoop, raw);
            raw->outServerThread = std::thread(ChatOverlayOutServerLoop, raw, cb);

            const std::string args = "--mode native-chat --session " + QuoteArg(sessionId) +
                " --pipe-in " + QuoteArg(state->inPipeName) +
                " --pipe-out " + QuoteArg(state->outPipeName) +
                " --stop-event " + QuoteArg(state->stopEventName);
            LogI("[chat-ui] launching native chat helper session=" + sessionId +
                " in=" + state->inPipeName + " out=" + state->outPipeName);
            raw->process = LaunchUserHostFeature(args);
            if (!raw->process) {
                LogE("[chat-ui] launch failed session=" + sessionId);
                raw->stop.store(true);
                if (raw->stopEvent) SetEvent(raw->stopEvent);
                if (raw->inPipe != INVALID_HANDLE_VALUE) CancelIoEx(raw->inPipe, nullptr);
                if (raw->outPipe != INVALID_HANDLE_VALUE) CancelIoEx(raw->outPipe, nullptr);
                if (raw->inServerThread.joinable()) raw->inServerThread.join();
                if (raw->outServerThread.joinable()) raw->outServerThread.join();
                if (raw->stopEvent) {
                    CloseHandle(raw->stopEvent);
                    raw->stopEvent = nullptr;
                }
                return nullptr;
            }

            WaitForSingleObject(raw->process, 1000);
            CloseHandle(raw->process);
            raw->process = nullptr;
            g_chatOverlays[sessionId] = std::move(state);
            return raw;
        }

        static bool SendChatToOverlay(const std::string& sessionId,
            HANDLE sessionJob,
            const std::string& displayName,
            const std::string& body,
            ChatOverlayReplyCallback cb) {
            if (body.empty()) return false;
            ChatOverlayState* state = EnsureChatOverlay(sessionId, sessionJob, std::move(cb));
            if (!state) return false;
            json payload = {
                {"type", "chat_message"},
                {"session_id", sessionId},
                {"sender", "tech"},
                {"display_name", displayName.empty() ? "Technician" : displayName},
                {"body", body},
                {"unix_ms", NowUnixMs()}
            };

            {
                std::lock_guard<std::mutex> lock(state->mu);
                if (state->userDismissed) {
                    LogI("[chat-ui] transcript updated while remote chat remains dismissed session=" + sessionId);
                    return true;
                }
                state->pendingToOverlay.push_back(payload.dump());
            }
            FlushQueuedChatToOverlay(state);
            return true;
        }

        static void StopChatOverlay(const std::string& sessionId) {
            std::unique_ptr<ChatOverlayState> state;
            {
                std::lock_guard<std::mutex> lock(g_chatOverlayMu);
                auto it = g_chatOverlays.find(sessionId);
                if (it == g_chatOverlays.end()) return;
                state = std::move(it->second);
                g_chatOverlays.erase(it);
            }

            LogI("[chat-ui] stopping overlay session=" + sessionId);
            state->stop.store(true);
            if (state->stopEvent) SetEvent(state->stopEvent);
            if (state->inPipe != INVALID_HANDLE_VALUE) {
                WriteLineToPipe(state->inPipe, json{ {"type", "close"}, {"session_id", sessionId} }.dump());
                FlushFileBuffers(state->inPipe);
                DisconnectNamedPipe(state->inPipe);
                CancelIoEx(state->inPipe, nullptr);
                CloseHandle(state->inPipe);
                state->inPipe = INVALID_HANDLE_VALUE;
            }
            if (state->outPipe != INVALID_HANDLE_VALUE) {
                DisconnectNamedPipe(state->outPipe);
                CancelIoEx(state->outPipe, nullptr);
                CloseHandle(state->outPipe);
                state->outPipe = INVALID_HANDLE_VALUE;
            }
            if (state->inServerThread.joinable()) state->inServerThread.join();
            if (state->outServerThread.joinable()) state->outServerThread.join();
            if (state->process) {
                DWORD wait = WaitForSingleObject(state->process, 1500);
                if (wait == WAIT_TIMEOUT) {
                    LogW("[chat-ui] overlay did not exit cleanly, terminating session=" + sessionId);
                    TerminateProcess(state->process, 0);
                    WaitForSingleObject(state->process, 1000);
                }
                CloseHandle(state->process);
                state->process = nullptr;
            }
            if (state->stopEvent) {
                CloseHandle(state->stopEvent);
                state->stopEvent = nullptr;
            }
        }

        static void StopChatOverlays() {
            std::vector<std::string> ids;
            {
                std::lock_guard<std::mutex> lock(g_chatOverlayMu);
                for (auto& kv : g_chatOverlays) ids.push_back(kv.first);
            }
            for (const auto& id : ids) StopChatOverlay(id);
        }
        struct SessionContext {
            std::string sessionId;
            std::vector<std::string> iceServers;
            std::string technicianName = "Technician";

            ShmemRing mediaShmem;
            std::unique_ptr<NamedPipeServer> mediaEventPipeServer;
            NamedPipeClient mediaControlPipe;
            std::mutex mediaControlMu;
            std::atomic<bool> mediaReady{ false };

            ShmemRing normalShmem;
            ShmemRing secureShmem;

            InputPipeWriter normalInputPipe;
            InputPipeWriter secureInputPipe;

            std::string normalShmemName;
            std::string secureShmemName;
            std::string normalInputPipeName;
            std::string secureInputPipeName;
            std::string normalStopEventName;
            std::string secureStopEventName;
            std::string mediaShmemName;
            std::string mediaControlPipeName;
            std::string mediaEventPipeName;
            std::string mediaStopEventName;
            std::string chatPipeName;

            HANDLE normalStopEvent = nullptr;
            HANDLE secureStopEvent = nullptr;
            HANDLE mediaStopEvent = nullptr;
            HANDLE sessionJob = nullptr;

            HANDLE normalStreamerProcess = nullptr;
            HANDLE secureStreamerProcess = nullptr;
            HANDLE backstageHostProcess = nullptr;
            HANDLE mediaHostProcess = nullptr;
            DWORD gpuHandleSourcePid = 0;
            std::unordered_map<uint64_t, uint64_t> gpuHandleMap;
            std::atomic<bool> gpuTransportFallbackRequested{ false };
            bool disableGpuTransport = false;
            DWORD normalStreamerSessionId = 0xFFFFFFFF;
            DWORD secureStreamerSessionId = 0xFFFFFFFF;
            bool backstageMode = false;
            SessionMode sessionMode = SessionMode::Console;
            std::unique_ptr<NamedPipeServer> chatPipeServer;

            std::thread framePumpThread;
            std::atomic<bool> framePumpStop{ false };

            int displayIndex = 0;
            int fps = 30;
            DisplayGeometrySnapshot displayGeometryAtStart{};
            DisplayGeometrySnapshot lastDisplayGeometry{};
            int lastCaptureMonitorWidth = 0;
            int lastCaptureMonitorHeight = 0;
            DesktopMode activeMode = DesktopMode::Normal;

            bool uacRequested = false;
            bool unifiedDesktopStreamer = true;
            bool unifiedSecureReady = false;
            bool secureLaunchInProgress = false;
            bool secureReady = false;
            bool secureStateAnnounced = false;
            bool handoffStateAnnounced = false;
            bool chatOpen = false;
            bool localInputBlocked = false;
            std::chrono::steady_clock::time_point lastNormalFrameAt{};
            std::chrono::steady_clock::time_point lastSecureFrameAt{};
            std::chrono::steady_clock::time_point uacDetectedAt{};
            std::chrono::steady_clock::time_point desktopReturnAt{};
            uint64_t uacDetectedTickNs = 0;
            uint64_t desktopReturnTickNs = 0;
            std::chrono::steady_clock::time_point lastNormalLaunchAttempt{};
            std::chrono::steady_clock::time_point lastSecureLaunchAttempt{};
            int secureHostFailureCount = 0;
            std::chrono::steady_clock::time_point secureHostFailureWindowStart{};
            std::chrono::steady_clock::time_point secureHostRestartBlockedUntil{};
            std::chrono::steady_clock::time_point lastConsoleSessionPoll{};
            std::chrono::steady_clock::time_point lastConsoleSwitchDetected{};
            std::chrono::steady_clock::time_point normalRetireRequestedAt{};
            std::chrono::steady_clock::time_point secureRetireRequestedAt{};
            uint64_t consoleNormalReadyAfterTickNs = 0;

            DWORD activeConsoleSessionId = 0xFFFFFFFF;
            DWORD lastSeenConsoleSessionId = 0xFFFFFFFF;
            DWORD pendingConsoleSessionId = 0xFFFFFFFF;
            DWORD bannerConsoleSessionId = 0xFFFFFFFF;
            bool consoleSwitchInProgress = false;
            bool consoleHandoffActive = false;
            bool normalDesktopUnavailable = false;
            bool normalRecoveryArmed = false;
            bool logoffLatched = false;
            uint64_t lastSessionChangeSeq = 0;
            // Stable Winlogon/login-screen state. While true the secure worker
            // exclusively owns video + input and no normal desktop helper is
            // launched until WTS exposes an interactive user token.
            bool loginDesktopMode = false;
            bool interactiveUserReady = false;
            bool bannerRebindPending = false;
            bool secureRetiring = false;
            uint64_t consoleGeneration = 0;
            std::atomic<bool> secureFallbackOwnsInput{ false };
        };

        static bool SecureHostRestartBlocked(SessionContext& ctx, std::chrono::steady_clock::time_point now) {
            if (ctx.secureHostRestartBlockedUntil.time_since_epoch().count() == 0) return false;
            if (now < ctx.secureHostRestartBlockedUntil) return true;
            LogI("[secure-circuit] cooldown expired session=" + ctx.sessionId);
            ctx.secureHostFailureCount = 0;
            ctx.secureHostFailureWindowStart = {};
            ctx.secureHostRestartBlockedUntil = {};
            return false;
        }

        static void RecordSecureHostFailure(SessionContext& ctx, std::chrono::steady_clock::time_point now, const std::string& reason) {
            const int windowMs = ReadConfigInt("HI5_SECURE_HOST_FAILURE_WINDOW_MS", 10000, 1000, 60000);
            const int failureLimit = ReadConfigInt("HI5_SECURE_HOST_FAILURE_LIMIT", 3, 2, 10);
            const int cooldownMs = ReadConfigInt("HI5_SECURE_HOST_FAILURE_COOLDOWN_MS", 30000, 1000, 300000);
            if (ctx.secureHostFailureWindowStart.time_since_epoch().count() == 0 ||
                now - ctx.secureHostFailureWindowStart > std::chrono::milliseconds(windowMs)) {
                ctx.secureHostFailureWindowStart = now;
                ctx.secureHostFailureCount = 0;
            }
            ++ctx.secureHostFailureCount;
            LogW("[secure-circuit] failure session=" + ctx.sessionId +
                " count=" + std::to_string(ctx.secureHostFailureCount) +
                " limit=" + std::to_string(failureLimit) + " reason=" + reason);
            if (ctx.secureHostFailureCount >= failureLimit) {
                ctx.secureHostRestartBlockedUntil = now + std::chrono::milliseconds(cooldownMs);
                LogW("[secure-circuit] OPEN session=" + ctx.sessionId +
                    " cooldown_ms=" + std::to_string(cooldownMs));
            }
        }

        static void ResetSecureHostFailureCircuit(SessionContext& ctx) {
            if (ctx.secureHostFailureCount == 0 && ctx.secureHostRestartBlockedUntil.time_since_epoch().count() == 0) return;
            LogI("[secure-circuit] reset after healthy secure frame session=" + ctx.sessionId);
            ctx.secureHostFailureCount = 0;
            ctx.secureHostFailureWindowStart = {};
            ctx.secureHostRestartBlockedUntil = {};
        }


                struct TerminalSession {
            std::string sessionId;
            HPCON pseudoConsole = nullptr;
            HANDLE process = nullptr;
            HANDLE thread = nullptr;
            HANDLE stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr;
            std::atomic<bool> running{ false };
            std::thread reader;
        };

class Worker {
        public:
            explicit Worker(std::string serviceName)
                : serviceName_(std::move(serviceName))
                , sessionBridge_()
                , presenceController_(sessionBridge_)
                , chatController_(sessionBridge_) {
                sessionBridge_.SetEventCallback([this](SessionEventType ev) {
                    HandleBridgeEvent(ev);
                    });
            }

            ~Worker() {
                Stop();
            }

            void Run() {
                LogI("worker start");
                PurgeOldChatLogs(90);

                constexpr int width = 0;
                constexpr int height = 0;
                const auto codecDecision = hi5::SelectCodecPolicy();

const int fps = ReadConfigInt(
    "HI5_STREAM_FPS",
    codecDecision.fps,
    5,
    60
);

const int bitrateKbps = ReadConfigInt(
    "HI5_STREAM_BITRATE_KBPS",
    codecDecision.bitrateKbps,
    1000,
    50000
);

LogI(
    "codec policy requested=" + codecDecision.requestedMode +
    " selected=" + codecDecision.selectedCodec +
    " encoder=" + codecDecision.encoder +
    " hardware=" + std::string(codecDecision.hardware ? "true" : "false") +
    " stable=" + std::string(codecDecision.stable ? "true" : "false") +
    " reason=" + codecDecision.reason
);

LogI(
    "stream config codec=" + codecDecision.selectedCodec +
    " encoder=" + codecDecision.encoder +
    " fps=" + std::to_string(fps) +
    " bitrate_kbps=" + std::to_string(bitrateKbps) +
    " note=codec probing deferred to session-scoped media host"
);

                const std::string defaultAgentWsBase = "wss://rmm.hi5central.com/agent/ws";
                const std::wstring configDir = L"C:\\ProgramData\\Hi5Central\\Agent";

                AgentIdentity ident = loadAgentIdentityFromDir(configDir, defaultAgentWsBase);

                LogI("identity loaded device_id=" + ident.deviceId);
                LogI("agent ws base=" + ident.agentWsBaseUrl);

                const std::string agentWsUrl =
                    ident.agentWsBaseUrl +
                    "?device_id=" + ident.deviceId +
                    "&device_key=" + ident.deviceKey;

                signaling_ = std::make_unique<SignalingClient>(agentWsUrl);

                StartTelemetryLoop(ident);
                StartJobLoop(ident);
                StartTrayLoop(ident);

                auto sendFn = [this](const std::string& payload) {
                    if (signaling_) {
                        signaling_->send(payload);
                    }
                    };

                signaling_->onOpen([this, ident]() {
                    signalingConnected_.store(true);
                    signalingReconnectRequested_.store(false);
                    LogI("websocket connected");
                    if (signaling_) {
                        signaling_->send(json{{"type", "hello"}, {"agent_version", hi5::kAgentVersion}}.dump());
                        LogI("hello sent");
                        SendInventorySnapshotSafe(ident);
                    }
                    });

                signaling_->onMessage([this, sendFn, width, height, fps, bitrateKbps, ident](const std::string& text) {
                    sessionBridge_.HandleIncomingJson(text);

                    const auto msg = json::parse(text, nullptr, false);
                    if (msg.is_discarded()) {
                        LogW("invalid json from signaling");
                        FlushBridgeOutgoing();
                        return;
                    }

                    const std::string type = msg.value("type", "");

                    if (type == "agent_action") {
                        HandleAgentAction(msg, ident);
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "job_execute" && msg.contains("job") && msg["job"].is_object()) {
                        const json job = msg["job"];
                        LogI("job dispatched over websocket id=" + job.value("id", std::string()) +
                            " type=" + job.value("job_type", std::string()));
                        std::thread([this, ident, job]() {
                            ExecuteClaimedJob(ident, job);
                        }).detach();
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "terminal_start" || type == "terminal_input" || type == "terminal_stop" || type == "terminal_resize") {
                        LogI("routing terminal websocket message type=" + type);
                        HandleTerminalMessage(msg);
                        return;
                    }

                    if (type == "files_list" || type == "files_cancel" || type == "files_download_request" || type == "files_download_cancel" || type == "files_rename_request" || type == "files_delete_request" || type == "files_mkdir_request" || type == "files_upload_start" || type == "files_upload_chunk" || type == "files_upload_complete" || type == "files_upload_cancel") {
                        LogI("routing live files websocket message type=" + type);
                        HandleLiveFilesMessage(msg);
                        return;
                    }

                if (type == "refresh_inventory" || type == "inventory_refresh") {
                        LogI("refresh_inventory requested by control server");
                        SendInventorySnapshotSafe(ident);
                        FlushBridgeOutgoing();
                        return;
                    }

                    std::string sessionId = msg.value("session_id", msg.value("sessionId", std::string()));
                    if (sessionId.empty()) {
                        sessionId = sessionBridge_.GetActiveSession();
                    }

                    if (type == "start_webrtc") {
                        const SessionMode sessionMode = ParseSessionMode(msg.value("mode", std::string("console")));
                        const std::string technicianName = msg.value("technician_name", msg.value("technician", std::string("Technician")));
                        LogI("start_webrtc session=" + sessionId + " mode=" + SessionModeName(sessionMode));
                        sessionBridge_.SetActiveSession(sessionId);

                        auto iceServers = parseIceServers(msg);
                        StartStreamerSession(sessionId, iceServers, sendFn, width, height, fps, bitrateKbps, sessionMode, technicianName);
                        if (!HasSession(sessionId)) {
                            LogSupportEvent("Remote session failed to start for " + technicianName);
                            sessionBridge_.ClearActiveSession();
                            FlushBridgeOutgoing();
                            return;
                        }

                        if (sessionMode == SessionMode::Console) {
                            presenceController_.ShowConnected(technicianName, true);
                            StartPresenceBanner(sessionId, technicianName, SessionJobHandle(sessionId));
                        }
                        else {
                            presenceController_.Hide();
                            StopPresenceBanner(sessionId);
                        }

                        LogSupportEvent("Remote session started by " + technicianName);
                        LogSupportEvent("Connecting - WebRTC");
                        // Keep the portal responsive without running expensive WMI/PowerShell
                        // collectors during live remote control. The WebSocket/session state
                        // is already live; full inventory can still be requested manually.
                        LogI("remote session active; full scheduled inventory throttled during stream session=" + sessionId);
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "webrtc_answer" || type == "ice_candidate" || type == "answer") {
                        std::lock_guard<std::mutex> lock(sessionsMu_);
                        auto it = sessions_.find(sessionId);
                        if (it != sessions_.end()) {
                            SendMediaControl(*it->second, json{ {"type", "signal"}, {"payload", text} });
                        }
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "input_event") {
                        SessionContext* ctx = nullptr;
                        {
                            std::lock_guard<std::mutex> lock(sessionsMu_);
                            auto it = sessions_.find(sessionId);
                            if (it != sessions_.end()) ctx = it->second.get();
                        }
                        if (ctx) DispatchInputToPipe(*ctx, msg);
                        else LogW("input_event received but no active session session=" + sessionId);
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "service_shortcut" || type == "system_shortcut" || type == "shortcut" ||
                        type == "service_command") {
                        SessionContext* ctx = nullptr;
                        {
                            std::lock_guard<std::mutex> lock(sessionsMu_);
                            auto it = sessions_.find(sessionId);
                            if (it != sessions_.end()) ctx = it->second.get();
                        }
                        if (ctx) {
                            DispatchInputToPipe(*ctx, msg);
                        }
                        else {
                            LogW("service shortcut received but no active session session=" + sessionId + " type=" + type);
                        }
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "switch_monitor") {
                        const int requested = msg.value("monitor_index", 0);
                        HandleSwitchMonitor(sessionId, requested);
                        return;
                    }

                    if (type == "chat_open") {
                        // Viewer-side Chat button should only open the technician panel.
                        // The remote user chat opens only when a real technician message arrives.
                        LogI("chat_open ignored for remote overlay session=" + sessionId);
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "chat_close") {
                        const bool closeRemote = msg.value("close_remote", false);
                        const std::string reason = msg.value("reason", std::string());
                        if (closeRemote || reason == "viewer_disconnect") {
                            LogI("chat_close requested remote overlay stop session=" + sessionId);
                            StopChatOverlay(sessionId);
                        }
                        else {
                            LogI("chat_close ignored for remote overlay session=" + sessionId);
                        }
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "chat_message") {
                        const std::string body = ChatBodyFromJson(msg);
                        const std::string displayName = msg.value("display_name", msg.value("sender_name", std::string("Technician")));
                        if (body.empty()) {
                            LogW("chat_message from viewer had no body/message/text content session=" + sessionId);
                            FlushBridgeOutgoing();
                            return;
                        }

                        AppendChatTranscript(sessionId, "tech", displayName, body);

                        const bool sent = SendChatToOverlay(sessionId, SessionJobHandle(sessionId), displayName, body,
                            [this](const std::string& sid, const std::string& replyBody) {
                                AppendChatTranscript(sid, "user", "Remote user", replyBody);
                                LogI("chat message from remote user session=" + sid + " bytes=" + std::to_string(replyBody.size()));
                                if (signaling_) {
                                    json out = {
                                        {"type", "chat_message"},
                                        {"session_id", sid},
                                        {"sender", "user"},
                                        {"display_name", "Remote user"},
                                        {"body", replyBody},
                                        {"unix_ms", NowUnixMs()}
                                    };
                                    signaling_->send(out.dump());
                                }
                            });

                        LogI("chat message queued to WebView overlay session=" + sessionId +
                            " sent=" + std::string(sent ? "1" : "0") +
                            " bytes=" + std::to_string(body.size()));
                        FlushBridgeOutgoing();
                        return;
                    }
                    if (type == "remote_file_list_request") {
                        HandleRemoteFileListRequest(sessionId, msg.value("path", std::string("/")));
                        return;
                    }

                    if (type == "remote_file_download_request") {
                        HandleRemoteFileDownloadRequest(sessionId, msg);
                        return;
                    }

                    if (type == "remote_file_upload_request") {
                        HandleRemoteFileUploadRequest(sessionId, msg);
                        return;
                    }

                    if (type == "remote_file_upload_start") {
                        HandleRemoteFileUploadStart(sessionId, msg);
                        return;
                    }

                    if (type == "remote_file_upload_chunk") {
                        HandleRemoteFileUploadChunk(sessionId, msg);
                        return;
                    }

                    if (type == "remote_file_upload_complete_request") {
                        HandleRemoteFileUploadComplete(sessionId, msg);
                        return;
                    }

                    if (type == "remote_file_delete_request") {
                        HandleRemoteFileDeleteRequest(sessionId, msg);
                        return;
                    }

                    if (type == "remote_file_mkdir_request") {
                        HandleRemoteFileMkdirRequest(sessionId, msg);
                        return;
                    }

                    if (type == "remote_file_rename_request") {
                        HandleRemoteFileRenameRequest(sessionId, msg);
                        return;
                    }

                    if (type == "backstage_start") {
                        LogI("backstage_start session=" + sessionId);
                        HandleBackstageStart(sessionId);
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "backstage_stop" || type == "console_start") {
                        LogI("backstage_stop/console_start session=" + sessionId);
                        HandleBackstageStop(sessionId);
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "viewer_disconnected" || type == "viewer_closed" || type == "viewer_left" ||
                        type == "session_ended" || type == "session_closed" || type == "session_stopped" ||
                        type == "stop_webrtc" || type == "webrtc_stopped" || type == "remote_session_ended" ||
                        type == "remote_control_stopped" || type == "end_session") {
                        LogI("remote session cleanup message type=" + type + " session=" + sessionId);
                        presenceController_.Hide();
                        sessionBridge_.ClearActiveSession();
                        if (!sessionId.empty()) {
                            StopSession(sessionId);
                            CleanupOrphanStreamerProcesses(sessionId);
                        }
                        else {
                            LogW("remote session cleanup message had empty session_id; stopping all sessions and helpers");
                            for (;;) {
                                std::string nextId;
                                {
                                    std::lock_guard<std::mutex> lock(sessionsMu_);
                                    if (sessions_.empty()) break;
                                    nextId = sessions_.begin()->first;
                                }
                                StopSession(nextId);
                            }
                            CleanupOrphanStreamerProcesses();
                        }
                        // Be deliberately aggressive here: after any viewer/session cleanup event
                        // there should be no user-facing helper or streamer child left running.
                        StopChatOverlays();
                        StopPresenceBanners();
                        CleanupOrphanUiHelperProcesses();
                        FlushBridgeOutgoing();
                        return;
                    }

                    FlushBridgeOutgoing();
                    });

                signaling_->onClosed([this]() {
                    signalingConnected_.store(false);
                    LogW("websocket closed; stopping active remote sessions and scheduling reconnect");
                    for (;;) {
                        std::string nextId;
                        {
                            std::lock_guard<std::mutex> lock(sessionsMu_);
                            if (sessions_.empty()) break;
                            nextId = sessions_.begin()->first;
                        }
                        StopSession(nextId);
                    }
                    CleanupOrphanStreamerProcesses();
                    if (!stop_.load()) {
                        signalingReconnectRequested_.store(true);
                    }
                    });

                signalingReconnectRequested_.store(false);
                signalingConnected_.store(false);
                signaling_->connect();
                StartInventoryLoop(ident);
                StartPatchDiscoveryLoop(ident);

                LogI("[patchhost] standalone PatchHost architecture active; legacy in-process patch worker removed");

                auto nextUiCleanupSweep = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                auto nextSignalingReconnect = std::chrono::steady_clock::now();
                int signalingReconnectBackoffSeconds = 2;
                while (!stop_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    const auto now = std::chrono::steady_clock::now();

                    if (signalingConnected_.load()) {
                        signalingReconnectBackoffSeconds = 2;
                    }
                    else if (signalingReconnectRequested_.load() && now >= nextSignalingReconnect) {
                        signalingReconnectRequested_.store(false);
                        LogI("websocket reconnect attempt backoff_seconds=" +
                            std::to_string(signalingReconnectBackoffSeconds));
                        try {
                            if (signaling_) signaling_->connect();
                        }
                        catch (const std::exception& e) {
                            LogW(std::string("websocket reconnect attempt failed: ") + e.what());
                            signalingReconnectRequested_.store(true);
                        }
                        nextSignalingReconnect = now + std::chrono::seconds(signalingReconnectBackoffSeconds);
                        signalingReconnectBackoffSeconds = std::min(signalingReconnectBackoffSeconds * 2, 60);
                    }

                    if (now >= nextUiCleanupSweep) {
                        nextUiCleanupSweep = now + std::chrono::seconds(5);
                        bool hasSessions = false;
                        {
                            std::lock_guard<std::mutex> lock(sessionsMu_);
                            hasSessions = !sessions_.empty();
                        }
                        if (!hasSessions) {
                            StopChatOverlays();
                            StopPresenceBanners();
                            CleanupOrphanUiHelperProcesses();
                            CleanupOrphanStreamerProcesses();
                            presenceController_.Hide();
                        }
                    }
                }

                Stop();
                LogI("worker stopped");
            }

            void RequestStop() {
                LogI("stop requested");
                stop_.store(true);
            }

            void Stop() {
                stop_.store(true);

                for (;;) {
                    std::string nextId;
                    {
                        std::lock_guard<std::mutex> lock(sessionsMu_);
                        if (sessions_.empty()) break;
                        nextId = sessions_.begin()->first;
                    }
                    StopSession(nextId);
                }

                StopInventoryLoop();
                StopTrayLoop();
                StopChatOverlays();
                StopPresenceBanners();
                CleanupOrphanUiHelperProcesses();
                StopResidentUserHosts();
                CleanupOrphanStreamerProcesses();
                presenceController_.Hide();
                sessionBridge_.ClearActiveSession();
                signaling_.reset();
            }


        private:
            void SendActionProgress(const std::string& actionId, const std::string& status, int progress, const std::string& message) {
                if (!signaling_ || actionId.empty()) return;
                json payload = {
                    {"type", "action_progress"},
                    {"action_id", actionId},
                    {"status", status},
                    {"progress", std::max(0, std::min(100, progress))},
                    {"message", message}
                };
                signaling_->send(payload.dump());
            }

            void SendActionResult(const std::string& actionId,
                const std::string& status,
                int progress,
                const std::string& message,
                const json& result,
                const std::string& errorMessage = std::string()) {
                if (!signaling_ || actionId.empty()) return;
                json payload = {
                    {"type", "action_result"},
                    {"action_id", actionId},
                    {"status", status},
                    {"progress", std::max(0, std::min(100, progress))},
                    {"message", message},
                    {"result", result}
                };
                if (!errorMessage.empty()) {
                    payload["error_message"] = errorMessage;
                }
                signaling_->send(payload.dump());
            }

            struct CommandResult {
                DWORD exitCode = 1;
                std::string output;
                std::string error;
                long long durationMs = 0;
            };

            CommandResult RunPowerShellCommand(const std::string& actionId, const std::string& command, int timeoutSeconds) {
                CommandResult result{};
                if (command.empty()) {
                    result.error = "Command is empty";
                    return result;
                }

                namespace fs = std::filesystem;
                const auto start = std::chrono::steady_clock::now();

                fs::path actionDir = fs::path(LR"(C:\ProgramData\Hi5Central\Agent\Actions)");
                std::error_code ec;
                fs::create_directories(actionDir, ec);

                std::string safeId = actionId;
                safeId.erase(std::remove_if(safeId.begin(), safeId.end(), [](unsigned char ch) {
                    return !(std::isalnum(ch) || ch == '-' || ch == '_');
                    }), safeId.end());
                if (safeId.empty()) safeId = "action";

                fs::path scriptPath = actionDir / (ToWidePath(safeId) + L".ps1");
                {
                    std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
                    if (!f) {
                        result.error = "Failed to create PowerShell script file";
                        return result;
                    }
                    f << "$ErrorActionPreference = 'Continue'\r\n";
                    f << "try {\r\n";
                    f << command << "\r\n";
                    f << "} catch { Write-Error $_; exit 1 }\r\n";
                }

                SECURITY_ATTRIBUTES sa{};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;

                HANDLE readPipe = nullptr;
                HANDLE writePipe = nullptr;
                if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
                    result.error = "CreatePipe failed";
                    return result;
                }
                SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = writePipe;
                si.hStdError = writePipe;
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

                PROCESS_INFORMATION pi{};
                std::wstring cmdLine = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath.wstring() + L"\"";

                BOOL ok = CreateProcessW(
                    nullptr,
                    cmdLine.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &si,
                    &pi);

                CloseHandle(writePipe);

                if (!ok) {
                    CloseHandle(readPipe);
                    fs::remove(scriptPath, ec);
                    result.error = "CreateProcessW failed for powershell.exe";
                    return result;
                }

                const DWORD timeoutMs = static_cast<DWORD>(std::max(5, timeoutSeconds) * 1000);
                const DWORD started = GetTickCount();
                constexpr size_t kMaxOutput = 256 * 1024;
                char buffer[4096];

                for (;;) {
                    DWORD available = 0;
                    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                        DWORD bytesRead = 0;
                        const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer) - 1));
                        if (!ReadFile(readPipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
                            break;
                        }
                        if (result.output.size() < kMaxOutput) {
                            const size_t remaining = kMaxOutput - result.output.size();
                            result.output.append(buffer, buffer + std::min<size_t>(bytesRead, remaining));
                        }
                    }

                    DWORD wait = WaitForSingleObject(pi.hProcess, 50);
                    if (wait == WAIT_OBJECT_0) {
                        break;
                    }

                    if (GetTickCount() - started > timeoutMs) {
                        TerminateProcess(pi.hProcess, 124);
                        result.error = "Command timed out";
                        break;
                    }
                }

                for (;;) {
                    DWORD bytesRead = 0;
                    if (!ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &bytesRead, nullptr) || bytesRead == 0) {
                        break;
                    }
                    if (result.output.size() < kMaxOutput) {
                        const size_t remaining = kMaxOutput - result.output.size();
                        result.output.append(buffer, buffer + std::min<size_t>(bytesRead, remaining));
                    }
                }

                DWORD exitCode = 1;
                if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
                    result.exitCode = exitCode;
                }

                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                CloseHandle(readPipe);
                fs::remove(scriptPath, ec);

                const auto end = std::chrono::steady_clock::now();
                result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                return result;
            }


            std::string ActiveInteractiveUserSid() {
                const DWORD sessionId = WTSGetActiveConsoleSessionId();
                if (sessionId == 0xFFFFFFFF) return {};
                HANDLE token = nullptr;
                if (!WTSQueryUserToken(sessionId, &token)) return {};

                DWORD needed = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
                if (!needed) {
                    CloseHandle(token);
                    return {};
                }
                std::vector<BYTE> buffer(needed);
                if (!GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
                    CloseHandle(token);
                    return {};
                }
                CloseHandle(token);

                auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
                LPWSTR sidText = nullptr;
                if (!ConvertSidToStringSidW(user->User.Sid, &sidText) || !sidText) return {};
                const std::string sid = WideToUtf8(sidText);
                LocalFree(sidText);
                return sid;
            }

            CommandResult RunPowerShellCommandInteractiveUser(const std::string& actionId, const std::string& command, int timeoutSeconds) {
                CommandResult result{};
                if (command.empty()) {
                    result.error = "Command is empty";
                    return result;
                }

                namespace fs = std::filesystem;
                const auto start = std::chrono::steady_clock::now();
                fs::path actionDir = fs::path(LR"(C:\ProgramData\Hi5Central\Agent\Actions)");
                std::error_code ec;
                fs::create_directories(actionDir, ec);

                std::string safeId = actionId;
                safeId.erase(std::remove_if(safeId.begin(), safeId.end(), [](unsigned char ch) {
                    return !(std::isalnum(ch) || ch == '-' || ch == '_');
                    }), safeId.end());
                if (safeId.empty()) safeId = "interactive-action";
                safeId += "-user";

                fs::path scriptPath = actionDir / (ToWidePath(safeId) + L".ps1");
                fs::path outputDir = fs::path(LR"(C:\Users\Public\Documents)");
                fs::create_directories(outputDir, ec);
                fs::path outputPath = outputDir / (std::wstring(L"Hi5Central-") + ToWidePath(safeId) + L".out");
                {
                    std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
                    if (!f) {
                        result.error = "Failed to create interactive PowerShell script file";
                        return result;
                    }
                    f << "$ErrorActionPreference = 'Continue'\r\n";
                    f << "try {\r\n";
                    f << command << "\r\n";
                    f << "} catch { Write-Error $_; exit 1 }\r\n";
                }

                const std::string cmdExe = R"(C:\Windows\System32\cmd.exe)";
                const std::string scriptUtf8 = WideToUtf8(scriptPath.wstring());
                const std::string outputUtf8 = WideToUtf8(outputPath.wstring());
                const std::string cmdLine =
                    "/d /s /c \"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \\\"" +
                    scriptUtf8 + "\\\" > \\\"" + outputUtf8 + "\\\" 2>&1\"";

                HANDLE process = hi5::LaunchInInteractiveSession(cmdExe, cmdLine);
                if (!process) {
                    fs::remove(scriptPath, ec);
                    result.error = "Failed to launch PowerShell in the active interactive user session";
                    return result;
                }

                const DWORD timeoutMs = static_cast<DWORD>(std::max(5, timeoutSeconds) * 1000);
                const DWORD wait = WaitForSingleObject(process, timeoutMs);
                if (wait == WAIT_TIMEOUT) {
                    TerminateProcess(process, 124);
                    result.error = "Interactive user command timed out";
                }

                DWORD exitCode = 1;
                if (GetExitCodeProcess(process, &exitCode)) result.exitCode = exitCode;
                CloseHandle(process);

                std::ifstream output(outputPath, std::ios::binary);
                if (output) {
                    constexpr size_t kMaxOutput = 256 * 1024;
                    std::string contents((std::istreambuf_iterator<char>(output)), std::istreambuf_iterator<char>());
                    if (contents.size() > kMaxOutput) contents.resize(kMaxOutput);
                    result.output = std::move(contents);
                }

                fs::remove(scriptPath, ec);
                fs::remove(outputPath, ec);
                const auto end = std::chrono::steady_clock::now();
                result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                return result;
            }

            json ParseJsonObjectFromOutput(const std::string& output) {
                if (output.empty()) {
                    return json::object();
                }

                const auto firstObj = output.find('{');
                const auto firstArr = output.find('[');
                std::string::size_type first = std::string::npos;
                if (firstObj != std::string::npos && firstArr != std::string::npos) {
                    first = std::min(firstObj, firstArr);
                }
                else if (firstObj != std::string::npos) {
                    first = firstObj;
                }
                else {
                    first = firstArr;
                }

                if (first == std::string::npos) {
                    return json::object();
                }

                const auto lastObj = output.rfind('}');
                const auto lastArr = output.rfind(']');
                std::string::size_type last = std::string::npos;
                if (lastObj != std::string::npos && lastArr != std::string::npos) {
                    last = std::max(lastObj, lastArr);
                }
                else if (lastObj != std::string::npos) {
                    last = lastObj;
                }
                else {
                    last = lastArr;
                }

                if (last == std::string::npos || last <= first) {
                    return json::object();
                }

                const std::string candidate = output.substr(first, last - first + 1);
                auto parsed = json::parse(candidate, nullptr, false);
                if (parsed.is_discarded()) {
                    return json::object();
                }
                return parsed;
            }

            json BuildCommandActionResult(const std::string& command, const CommandResult& cr) {
                json parsed = ParseJsonObjectFromOutput(cr.output);
                json result = {
                    {"exit_code", static_cast<int>(cr.exitCode)},
                    {"duration_ms", cr.durationMs},
                    {"output", cr.output}
                };
                if (!command.empty()) {
                    result["command"] = command;
                }
                if (!parsed.empty()) {
                    result["parsed"] = parsed;
                    if (parsed.is_object()) {
                        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                            result[it.key()] = it.value();
                        }
                    }
                }
                if (!cr.error.empty()) {
                    result["error"] = cr.error;
                }
                return result;
            }

            std::string WindowsUpdateCommonPowerShell() {
                return R"HI5PS(
function Get-Hi5RebootRequired {
    $paths = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired',
        'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations'
    )
    foreach ($p in $paths) {
        try {
            if (Test-Path $p) { return $true }
        } catch {}
    }
    return $false
}
function Convert-Hi5UpdateResultCode([int]$code) {
    switch ($code) {
        0 { 'NotStarted' }
        1 { 'InProgress' }
        2 { 'Succeeded' }
        3 { 'SucceededWithErrors' }
        4 { 'Failed' }
        5 { 'Aborted' }
        default { "Unknown($code)" }
    }
}
function Convert-Hi5UpdateOperation([int]$code) {
    switch ($code) {
        1 { 'Installation' }
        2 { 'Uninstallation' }
        default { "Operation($code)" }
    }
}
function Get-Hi5UpdateInfo($u) {
    $kbs = @()
    try { foreach ($kb in $u.KBArticleIDs) { if ($kb) { $kbs += "KB$kb" } } } catch {}
    $cats = @()
    try { foreach ($c in $u.Categories) { if ($c.Name) { $cats += [string]$c.Name } } } catch {}
    $isDriver = $false
    foreach ($c in $cats) { if ($c -match 'Driver') { $isDriver = $true } }
    [pscustomobject]@{
        title = [string]$u.Title
        kb_articles = $kbs
        categories = $cats
        is_driver = $isDriver
        msrc_severity = $(try { [string]$u.MsrcSeverity } catch { '' })
        is_downloaded = $(try { [bool]$u.IsDownloaded } catch { $false })
        requires_reboot = $(try { [bool]$u.RebootRequired } catch { $false })
        eula_accepted = $(try { [bool]$u.EulaAccepted } catch { $false })
        size_bytes = $(try { [int64]$u.MaxDownloadSize } catch { 0 })
        support_url = $(try { [string]$u.SupportUrl } catch { '' })
    }
}
)HI5PS";
            }

            std::string BuildWindowsUpdateScanScript() {
                return WindowsUpdateCommonPowerShell() + R"HI5PS(
$ErrorActionPreference = 'Stop'
$session = New-Object -ComObject Microsoft.Update.Session
$searcher = $session.CreateUpdateSearcher()
$started = Get-Date
$result = $searcher.Search('IsInstalled=0 and IsHidden=0')
$items = @()
for ($i = 0; $i -lt $result.Updates.Count; $i++) {
    $items += Get-Hi5UpdateInfo $result.Updates.Item($i)
}
[pscustomobject]@{
    action = 'scan_windows_updates'
    status = 'ok'
    scanned_at = (Get-Date).ToUniversalTime().ToString('o')
    duration_seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
    count = $items.Count
    reboot_required = Get-Hi5RebootRequired
    updates = $items
} | ConvertTo-Json -Depth 10 -Compress
)HI5PS";
            }

            std::string BuildWindowsUpdateHistoryScript(const json& payload) {
                int limit = 50;
                try { limit = std::max(1, std::min(250, payload.value("limit", 50))); }
                catch (...) {}
                return WindowsUpdateCommonPowerShell() + "\n$historyLimit = " + std::to_string(limit) + R"HI5PS(
$ErrorActionPreference = 'Continue'
$session = New-Object -ComObject Microsoft.Update.Session
$searcher = $session.CreateUpdateSearcher()
$total = 0
try { $total = [int]$searcher.GetTotalHistoryCount() } catch {}
$take = [Math]::Min($historyLimit, $total)
$history = @()
if ($take -gt 0) {
    try {
        $rows = $searcher.QueryHistory(0, $take)
        foreach ($h in $rows) {
            $history += [pscustomobject]@{
                date = $(try { ([datetime]$h.Date).ToUniversalTime().ToString('o') } catch { '' })
                title = [string]$h.Title
                operation = Convert-Hi5UpdateOperation ([int]$h.Operation)
                result_code = [int]$h.ResultCode
                result = Convert-Hi5UpdateResultCode ([int]$h.ResultCode)
                hresult = ('0x{0:X8}' -f ([uint32]$h.HResult))
                client_application_id = $(try { [string]$h.ClientApplicationID } catch { '' })
                support_url = $(try { [string]$h.SupportUrl } catch { '' })
            }
        }
    } catch {}
}
$hotfixes = @()
try {
    $hotfixes = Get-HotFix | Sort-Object InstalledOn -Descending | Select-Object -First $historyLimit | ForEach-Object {
        [pscustomobject]@{
            hotfix_id = [string]$_.HotFixID
            description = [string]$_.Description
            installed_by = [string]$_.InstalledBy
            installed_on = $(try { ([datetime]$_.InstalledOn).ToUniversalTime().ToString('o') } catch { '' })
        }
    }
} catch {}
$events = @()
try {
    $events = Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WindowsUpdateClient'; Id=19,20,21,43} -MaxEvents $historyLimit -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{
            time_created = $(try { ([datetime]$_.TimeCreated).ToUniversalTime().ToString('o') } catch { '' })
            id = [int]$_.Id
            level = [string]$_.LevelDisplayName
            message = [string]$_.Message
        }
    }
} catch {}
[pscustomobject]@{
    action = 'get_update_history'
    status = 'ok'
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
    reboot_required = Get-Hi5RebootRequired
    history_count = $history.Count
    history = $history
    hotfixes = $hotfixes
    windows_update_events = $events
} | ConvertTo-Json -Depth 10 -Compress
)HI5PS";
            }

            std::string BuildWindowsUpdateInstallScript(const json& payload) {
                std::string payloadText = payload.dump();
                return WindowsUpdateCommonPowerShell() + R"HI5PS(
$ErrorActionPreference = 'Stop'
$payload = @'
)HI5PS" + payloadText + R"HI5PS(
'@ | ConvertFrom-Json
$installAll = $true
try { if ($null -ne $payload.install_all) { $installAll = [bool]$payload.install_all } } catch {}
$includeDrivers = $true
try { if ($null -ne $payload.include_drivers) { $includeDrivers = [bool]$payload.include_drivers } } catch {}
$kbFilter = @()
try { if ($payload.kb_articles) { foreach ($kb in $payload.kb_articles) { $kbFilter += ([string]$kb).ToUpperInvariant().Replace('KB','') } } } catch {}
$titleFilter = @()
try { if ($payload.titles) { foreach ($t in $payload.titles) { $titleFilter += [string]$t } } } catch {}
$hasFilter = (($kbFilter.Count -gt 0) -or ($titleFilter.Count -gt 0))
$session = New-Object -ComObject Microsoft.Update.Session
$searcher = $session.CreateUpdateSearcher()
$searchStarted = Get-Date
$search = $searcher.Search('IsInstalled=0 and IsHidden=0')
$selected = New-Object -ComObject Microsoft.Update.UpdateColl
$selectedInfo = @()
for ($i = 0; $i -lt $search.Updates.Count; $i++) {
    $u = $search.Updates.Item($i)
    $info = Get-Hi5UpdateInfo $u
    if (-not $includeDrivers -and $info.is_driver) { continue }
    $matches = $installAll
    if ($hasFilter) {
        $matches = $false
        foreach ($kb in $info.kb_articles) {
            if ($kbFilter -contains ([string]$kb).ToUpperInvariant().Replace('KB','')) { $matches = $true }
        }
        foreach ($t in $titleFilter) {
            if ($info.title -like "*$t*") { $matches = $true }
        }
    }
    if (-not $matches) { continue }
    try { if (-not $u.EulaAccepted) { $u.AcceptEula() } } catch {}
    [void]$selected.Add($u)
    $selectedInfo += $info
}
if ($selected.Count -eq 0) {
    [pscustomobject]@{
        action = 'install_windows_updates'
        status = 'ok'
        message = 'No matching updates to install'
        searched_count = [int]$search.Updates.Count
        selected_count = 0
        reboot_required = Get-Hi5RebootRequired
        updates = @()
    } | ConvertTo-Json -Depth 10 -Compress
    exit 0
}
$downloadResult = $null
$installResult = $null
$downloadStarted = Get-Date
$downloader = $session.CreateUpdateDownloader()
$downloader.Updates = $selected
$downloadResult = $downloader.Download()
$installStarted = Get-Date
$installer = $session.CreateUpdateInstaller()
$installer.Updates = $selected
try { $installer.ForceQuiet = $true } catch {}
try { $installer.AllowSourcePrompts = $false } catch {}
$installResult = $installer.Install()
$results = @()
for ($i = 0; $i -lt $selected.Count; $i++) {
    $u = $selected.Item($i)
    $ur = $installResult.GetUpdateResult($i)
    $info = Get-Hi5UpdateInfo $u
    $results += [pscustomobject]@{
        title = $info.title
        kb_articles = $info.kb_articles
        categories = $info.categories
        is_driver = $info.is_driver
        result_code = [int]$ur.ResultCode
        result = Convert-Hi5UpdateResultCode ([int]$ur.ResultCode)
        hresult = ('0x{0:X8}' -f ([uint32]$ur.HResult))
        reboot_required = $(try { [bool]$ur.RebootRequired } catch { $false })
    }
}
$overall = Convert-Hi5UpdateResultCode ([int]$installResult.ResultCode)
[pscustomobject]@{
    action = 'install_windows_updates'
    status = if ($installResult.ResultCode -eq 2 -or $installResult.ResultCode -eq 3) { 'ok' } else { 'failed' }
    searched_count = [int]$search.Updates.Count
    selected_count = [int]$selected.Count
    selected_updates = $selectedInfo
    download_result_code = [int]$downloadResult.ResultCode
    download_result = Convert-Hi5UpdateResultCode ([int]$downloadResult.ResultCode)
    install_result_code = [int]$installResult.ResultCode
    install_result = $overall
    reboot_required = (Get-Hi5RebootRequired -or [bool]$installResult.RebootRequired)
    searched_seconds = [math]::Round(($downloadStarted - $searchStarted).TotalSeconds, 2)
    downloaded_seconds = [math]::Round(($installStarted - $downloadStarted).TotalSeconds, 2)
    installed_at = (Get-Date).ToUniversalTime().ToString('o')
    results = $results
} | ConvertTo-Json -Depth 10 -Compress
if ($installResult.ResultCode -eq 4 -or $installResult.ResultCode -eq 5) { exit 1 }
)HI5PS";
            }



            std::string PsSingleQuote(const std::string& value) {
                std::string out = "'";
                for (char ch : value) {
                    if (ch == '\'') out += "''";
                    else out.push_back(ch);
                }
                out += "'";
                return out;
            }

            std::string PsStringArrayFromJson(const json& payload, const char* key) {
                std::string out = "@(";
                bool first = true;
                try {
                    if (payload.contains(key) && payload[key].is_array()) {
                        for (const auto& v : payload[key]) {
                            if (!v.is_string()) continue;
                            const std::string s = v.get<std::string>();
                            if (s.empty()) continue;
                            if (!first) out += ",";
                            out += PsSingleQuote(s);
                            first = false;
                        }
                    }
                }
                catch (...) {}
                out += ")";
                return out;
            }

            std::string WingetCommonPowerShell() {
                return R"HI5PS(
function Find-Hi5Winget {
    $cmd = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) { return $cmd.Source }

    $candidate = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
    if (Test-Path $candidate) { return $candidate }

    $windowsApps = Join-Path $env:ProgramFiles 'WindowsApps'
    if (Test-Path $windowsApps) {
        try {
            $matches = Get-ChildItem -Path $windowsApps -Filter winget.exe -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match 'Microsoft\.DesktopAppInstaller_' } |
                Sort-Object FullName -Descending
            foreach ($m in $matches) {
                if (Test-Path $m.FullName) { return $m.FullName }
            }
        } catch {}
    }

    $candidate = Join-Path $env:WINDIR 'System32\winget.exe'
    if (Test-Path $candidate) { return $candidate }
    return $null
}

function Invoke-Hi5Winget([string[]]$Hi5Args) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $text = ''
    $code = 1
    try {
        $text = (& $script:Hi5WingetPath @Hi5Args 2>&1 | Out-String)
        $code = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    } catch {
        $text = [string]$_
        $code = 1
    }
    $sw.Stop()
    [pscustomobject]@{
        args = $Hi5Args
        exit_code = $code
        duration_seconds = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        output = $text
    }
}

function Parse-Hi5WingetUpgradeText([string]$Text) {
    $items = @()
    if ([string]::IsNullOrWhiteSpace($Text)) { return $items }

    $lines = $Text -split "`r?`n" | ForEach-Object { $_.TrimEnd() } | Where-Object { $_.Trim().Length -gt 0 }
    foreach ($line in $lines) {
        $trim = $line.Trim()
        if ($trim -match '^(Name\s+Id\s+Version\s+Available\s+Source|[- ]{5,}|No installed package|No applicable update|The following packages|Name\s+Id\s+Installed)') { continue }
        if ($trim -match '^Found\s+.+\s+\[.+\]$') { continue }
        if ($trim -match '^\d+\s+upgrades?\s+available') { continue }
        if ($trim -match '^(Downloading|Installing|Successfully|Starting|Verifying|Done|Cancelled|Failed when|The installer)') { continue }

        $parts = [regex]::Split($trim, '\s{2,}') | Where-Object { $_ -ne '' }
        if ($parts.Count -ge 5) {
            $name = [string]$parts[0]
            $id = [string]$parts[1]
            $version = [string]$parts[2]
            $available = [string]$parts[3]
            $source = [string]$parts[4]
            if ($id -match '^[A-Za-z0-9][A-Za-z0-9_.+\-]+$') {
                $items += [pscustomobject]@{
                    name = $name
                    package_id = $id
                    id = $id
                    current_version = $version
                    available_version = $available
                    source = $source
                    manager = 'winget'
                }
            }
        }
    }
    return $items
}

function Get-Hi5WingetBaseArgs([string]$Source, [bool]$IncludeUnknown, [bool]$IncludePinned) {
    $args = @('--accept-source-agreements', '--disable-interactivity')
    if ($Source -and $Source -ne 'all') { $args += @('--source', $Source) }
    if ($IncludeUnknown) { $args += '--include-unknown' }
    if ($IncludePinned) { $args += '--include-pinned' }
    return $args
}

$script:Hi5WingetPath = Find-Hi5Winget
if (-not $script:Hi5WingetPath) {
    [pscustomobject]@{
        action = 'winget_unavailable'
        status = 'failed'
        error = 'winget.exe was not found. Install/update Microsoft App Installer or enable Windows Package Manager.'
        winget_available = $false
    } | ConvertTo-Json -Depth 8 -Compress
    exit 2
}
)HI5PS";
            }

            std::string BuildSoftwareUpdateScanScript(const json& payload) {
                const std::string source = payload.value("source", std::string("winget"));
                const bool includeUnknown = payload.value("include_unknown", true);
                const bool includePinned = payload.value("include_pinned", false);
                return WingetCommonPowerShell() +
                    "\n$hi5Source = " + PsSingleQuote(source) +
                    "\n$hi5IncludeUnknown = $" + std::string(includeUnknown ? "true" : "false") +
                    "\n$hi5IncludePinned = $" + std::string(includePinned ? "true" : "false") + R"HI5PS(
$ErrorActionPreference = 'Continue'
$started = Get-Date
$versionRun = Invoke-Hi5Winget -Hi5Args @('--version')
$sourceRun = Invoke-Hi5Winget -Hi5Args @('source','list','--accept-source-agreements','--disable-interactivity')
$updateSourceRun = Invoke-Hi5Winget -Hi5Args @('source','update','--accept-source-agreements','--disable-interactivity')
$upgradeArgs = @('upgrade') + (Get-Hi5WingetBaseArgs $hi5Source $hi5IncludeUnknown $hi5IncludePinned)
$upgradeRun = Invoke-Hi5Winget -Hi5Args $upgradeArgs
if ($upgradeRun.output -match 'usage:\s+winget' -and $upgradeRun.output -match 'The following commands are available') {
    throw 'WinGet upgrade was invoked without arguments. This indicates an internal argument binding failure.'
}
$updates = @(Parse-Hi5WingetUpgradeText $upgradeRun.output)
[pscustomobject]@{
    action = 'scan_software_updates'
    status = 'ok'
    manager = 'winget'
    winget_available = $true
    winget_path = $script:Hi5WingetPath
    winget_version = ($versionRun.output.Trim())
    source = $hi5Source
    include_unknown = $hi5IncludeUnknown
    include_pinned = $hi5IncludePinned
    scanned_at = (Get-Date).ToUniversalTime().ToString('o')
    duration_seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
    count = $updates.Count
    updates = $updates
    source_output = $sourceRun.output
    source_update_output = $updateSourceRun.output
    raw_output = $upgradeRun.output
    exit_code = $upgradeRun.exit_code
} | ConvertTo-Json -Depth 10 -Compress
)HI5PS";
            }

            std::string BuildSoftwareUpdateInstallScript(const json& payload) {
                const std::string source = payload.value("source", std::string("winget"));
                const bool includeUnknown = payload.value("include_unknown", true);
                const bool includePinned = payload.value("include_pinned", false);
                const bool installAll = payload.value("install_all", true);
                const std::string packageIds = PsStringArrayFromJson(payload, "package_ids");
                const std::string excludedIds = PsStringArrayFromJson(payload, "excluded_package_ids");
                return WingetCommonPowerShell() +
                    "\n$hi5Source = " + PsSingleQuote(source) +
                    "\n$hi5IncludeUnknown = $" + std::string(includeUnknown ? "true" : "false") +
                    "\n$hi5IncludePinned = $" + std::string(includePinned ? "true" : "false") +
                    "\n$hi5InstallAll = $" + std::string(installAll ? "true" : "false") +
                    "\n$hi5PackageIds = " + packageIds +
                    "\n$hi5ExcludedIds = " + excludedIds + R"HI5PS(
$ErrorActionPreference = 'Continue'
$started = Get-Date
$versionRun = Invoke-Hi5Winget -Hi5Args @('--version')
$null = Invoke-Hi5Winget -Hi5Args @('source','update','--accept-source-agreements','--disable-interactivity')
$base = Get-Hi5WingetBaseArgs $hi5Source $hi5IncludeUnknown $hi5IncludePinned
$results = @()

if ($hi5PackageIds.Count -gt 0) {
    foreach ($id in $hi5PackageIds) {
        if ($hi5ExcludedIds -contains $id) { continue }
        $upgradeArgs = @('upgrade','--id', $id, '--exact', '--silent', '--accept-package-agreements') + $base
        $run = Invoke-Hi5Winget -Hi5Args $upgradeArgs
        $results += [pscustomobject]@{
            package_id = $id
            id = $id
            manager = 'winget'
            source = $hi5Source
            exit_code = $run.exit_code
            duration_seconds = $run.duration_seconds
            status = if ($run.exit_code -eq 0) { 'ok' } else { 'failed' }
            output = $run.output
        }
    }
} elseif ($hi5InstallAll) {
    $upgradeArgs = @('upgrade','--all','--silent','--accept-package-agreements') + $base
    $run = Invoke-Hi5Winget -Hi5Args $upgradeArgs
    $results += [pscustomobject]@{
        package_id = '*'
        id = '*'
        manager = 'winget'
        source = $hi5Source
        exit_code = $run.exit_code
        duration_seconds = $run.duration_seconds
        status = if ($run.exit_code -eq 0) { 'ok' } else { 'failed' }
        output = $run.output
    }
}

$failed = @($results | Where-Object { $_.status -ne 'ok' }).Count
[pscustomobject]@{
    action = 'install_software_updates'
    status = if ($failed -eq 0) { 'ok' } else { 'failed' }
    manager = 'winget'
    winget_available = $true
    winget_path = $script:Hi5WingetPath
    winget_version = ($versionRun.output.Trim())
    source = $hi5Source
    include_unknown = $hi5IncludeUnknown
    include_pinned = $hi5IncludePinned
    install_all = $hi5InstallAll
    requested_package_ids = $hi5PackageIds
    excluded_package_ids = $hi5ExcludedIds
    installed_at = (Get-Date).ToUniversalTime().ToString('o')
    duration_seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
    attempted_count = $results.Count
    failed_count = $failed
    results = $results
} | ConvertTo-Json -Depth 10 -Compress
if ($failed -gt 0) { exit 1 }
)HI5PS";
            }

            void HandleAgentAction(json msg, AgentIdentity ident) {
                const std::string actionId = msg.value("action_id", std::string());
                const std::string actionType = msg.value("action_type", msg.value("kind", std::string()));
                json payload = msg.value("payload", json::object());

                if (actionId.empty()) {
                    LogW("agent_action missing action_id");
                    return;
                }

                std::thread([this, actionId, actionType, payload, ident]() mutable {
                    try {
                        SendActionProgress(actionId, "running", 5, "Action started");

                        if (actionType == "refresh_inventory" || actionType == "inventory_refresh") {
                            SendActionProgress(actionId, "running", 40, "Collecting inventory");
                            SendInventorySnapshotSafe(ident);
                            SendActionResult(actionId, "completed", 100, "Inventory refresh completed", json{ {"inventory_sent", true} });
                            return;
                        }

                        if (actionType == "run_powershell" || actionType == "command" || actionType == "run_command") {
                            const std::string command = payload.value("command", std::string("whoami"));
                            const int timeoutSeconds = std::max(5, std::min(3600, payload.value("timeout_seconds", 120)));
                            SendActionProgress(actionId, "running", 15, "Running PowerShell command");
                            CommandResult cr = RunPowerShellCommand(actionId, command, timeoutSeconds);
                            json result = BuildCommandActionResult(command, cr);
                            const bool ok = cr.error.empty() && cr.exitCode == 0;
                            SendActionResult(actionId, ok ? "completed" : "failed", 100, ok ? "Command completed" : "Command failed", result, cr.error);
                            return;
                        }

                        if (actionType == "scan_windows_updates" || actionType == "windows_update_scan") {
                            SendActionProgress(actionId, "running", 10, "Starting Windows Update scan");
                            const std::string script = BuildWindowsUpdateScanScript();
                            const int timeoutSeconds = std::max(60, std::min(3600, payload.value("timeout_seconds", 1800)));
                            SendActionProgress(actionId, "running", 35, "Searching for pending updates");
                            CommandResult cr = RunPowerShellCommand(actionId, script, timeoutSeconds);
                            json result = BuildCommandActionResult(std::string(), cr);
                            const int count = result.value("count", 0);
                            const bool ok = cr.error.empty() && cr.exitCode == 0;
                            std::string message = ok ? ("Windows Update scan completed: " + std::to_string(count) + " pending update(s)") : "Windows Update scan failed";
                            SendActionResult(actionId, ok ? "completed" : "failed", 100, message, result, cr.error);
                            return;
                        }

                        if (actionType == "get_update_history" || actionType == "windows_update_history") {
                            SendActionProgress(actionId, "running", 15, "Collecting Windows Update history");
                            const std::string script = BuildWindowsUpdateHistoryScript(payload);
                            const int timeoutSeconds = std::max(30, std::min(1800, payload.value("timeout_seconds", 300)));
                            CommandResult cr = RunPowerShellCommand(actionId, script, timeoutSeconds);
                            json result = BuildCommandActionResult(std::string(), cr);
                            const int count = result.value("history_count", 0);
                            const bool ok = cr.error.empty() && cr.exitCode == 0;
                            std::string message = ok ? ("Windows Update history collected: " + std::to_string(count) + " WUA item(s)") : "Windows Update history collection failed";
                            SendActionResult(actionId, ok ? "completed" : "failed", 100, message, result, cr.error);
                            return;
                        }

                        if (actionType == "install_windows_updates" || actionType == "windows_update_install") {
                            SendActionProgress(actionId, "running", 5, "Preparing Windows Update installation");
                            const std::string script = BuildWindowsUpdateInstallScript(payload);
                            const int timeoutSeconds = std::max(300, std::min(14400, payload.value("timeout_seconds", 7200)));
                            SendActionProgress(actionId, "running", 25, "Searching and selecting updates");
                            SendActionProgress(actionId, "running", 45, "Downloading/installing selected updates quietly");
                            CommandResult cr = RunPowerShellCommand(actionId, script, timeoutSeconds);
                            json result = BuildCommandActionResult(std::string(), cr);
                            const int selected = result.value("selected_count", 0);
                            const bool reboot = result.value("reboot_required", false);
                            const std::string installResult = result.value("install_result", std::string());
                            const bool ok = cr.error.empty() && cr.exitCode == 0;
                            std::string message;
                            if (ok) {
                                message = "Windows Update install completed: " + std::to_string(selected) + " update(s)";
                                if (!installResult.empty()) message += " (" + installResult + ")";
                                if (reboot) message += "; reboot required";
                            }
                            else {
                                message = "Windows Update install failed";
                            }
                            SendActionResult(actionId, ok ? "completed" : "failed", 100, message, result, cr.error);
                            return;
                        }

                        if (actionType == "scan_software_updates" || actionType == "software_update_scan" || actionType == "winget_scan") {
                            SendActionProgress(actionId, "running", 10, "Starting third-party software update scan");
                            const std::string script = BuildSoftwareUpdateScanScript(payload);
                            const int timeoutSeconds = std::max(60, std::min(3600, payload.value("timeout_seconds", 1800)));
                            SendActionProgress(actionId, "running", 35, "Refreshing WinGet source and checking available upgrades");
                            CommandResult cr = RunPowerShellCommand(actionId, script, timeoutSeconds);
                            json result = BuildCommandActionResult(std::string(), cr);
                            const int count = result.value("count", 0);
                            const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                            std::string message = ok ? ("Third-party software scan completed: " + std::to_string(count) + " available update(s)") : "Third-party software scan failed";
                            SendActionResult(actionId, ok ? "completed" : "failed", 100, message, result, cr.error);
                            return;
                        }

                        if (actionType == "install_software_updates" || actionType == "software_update_install" || actionType == "winget_upgrade") {
                            SendActionProgress(actionId, "running", 5, "Preparing third-party software updates");
                            const std::string script = BuildSoftwareUpdateInstallScript(payload);
                            const int timeoutSeconds = std::max(300, std::min(14400, payload.value("timeout_seconds", 7200)));
                            SendActionProgress(actionId, "running", 25, "Refreshing WinGet source");
                            SendActionProgress(actionId, "running", 45, "Installing third-party software updates silently");
                            CommandResult cr = RunPowerShellCommand(actionId, script, timeoutSeconds);
                            json result = BuildCommandActionResult(std::string(), cr);
                            const int attempted = result.value("attempted_count", 0);
                            const int failed = result.value("failed_count", 0);
                            const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                            std::string message = ok ? ("Third-party software update completed: " + std::to_string(attempted) + " attempted") : "Third-party software update failed";
                            if (failed > 0) message += "; " + std::to_string(failed) + " failed";
                            SendActionResult(actionId, ok ? "completed" : "failed", 100, message, result, cr.error);
                            return;
                        }

                        SendActionResult(actionId, "failed", 100, "Action type is not implemented by this agent yet",
                            json{ {"action_type", actionType} }, "Unsupported action type: " + actionType);
                    }
                    catch (const std::exception& ex) {
                        SendActionResult(actionId, "failed", 100, "Action failed", json::object(), ex.what());
                    }
                    catch (...) {
                        SendActionResult(actionId, "failed", 100, "Action failed", json::object(), "unknown error");
                    }
                    }).detach();
            }



            std::string PowerShellUtf8Preamble() {
                return R"HI5PS(
$ErrorActionPreference = 'Continue'
try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
} catch {}
)HI5PS";
            }

            std::string BuildServicesListScript() {
                return PowerShellUtf8Preamble() + R"HI5PS(
function ConvertTo-Hi5SafeString($Value) {
    if ($null -eq $Value) { return '' }
    $s = [string]$Value
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

# Get-Service is substantially faster than Win32_Service/CIM on many endpoints.
# Read static metadata directly from the service registry keys so the list opens
# quickly without waiting for the WMI provider to enumerate every service.
$items = Get-Service | Sort-Object DisplayName | ForEach-Object {
    $service = $_
    $regPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\' + $service.Name
    $reg = $null
    try { $reg = Get-ItemProperty -LiteralPath $regPath -ErrorAction Stop } catch {}

    $startMode = 'Unknown'
    if ($null -ne $reg) {
        switch ([int]$reg.Start) {
            2 { $startMode = if ([int]$reg.DelayedAutoStart -eq 1) { 'AutomaticDelayed' } else { 'Automatic' } }
            3 { $startMode = 'Manual' }
            4 { $startMode = 'Disabled' }
            default { $startMode = 'Unknown' }
        }
    }

    [pscustomobject]@{
        name = ConvertTo-Hi5SafeString $service.Name
        display_name = ConvertTo-Hi5SafeString $service.DisplayName
        state = ConvertTo-Hi5SafeString $service.Status
        status = ConvertTo-Hi5SafeString $service.Status
        start_mode = $startMode
        start_name = ConvertTo-Hi5SafeString $reg.ObjectName
        process_id = 0
        path_name = ConvertTo-Hi5SafeString $reg.ImagePath
        description = ConvertTo-Hi5SafeString $reg.Description
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

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
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


            std::string BuildProcessesListScript() {
                return PowerShellUtf8Preamble() + R"HI5PS(
function ConvertTo-Hi5SafeString($Value) {
    if ($null -eq $Value) { return '' }
    $s = [string]$Value
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

$items = Get-Process | Sort-Object ProcessName, Id | ForEach-Object {
    $path = ''
    try { $path = $_.Path } catch {}
    [pscustomobject]@{
        pid = [int]$_.Id
        name = ConvertTo-Hi5SafeString $_.ProcessName
        window_title = ConvertTo-Hi5SafeString $_.MainWindowTitle
        session_id = [int]$_.SessionId
        cpu_seconds = if ($null -ne $_.CPU) { [math]::Round([double]$_.CPU, 2) } else { $null }
        working_set_bytes = [int64]$_.WorkingSet64
        private_memory_bytes = [int64]$_.PrivateMemorySize64
        handle_count = [int]$_.HandleCount
        thread_count = [int]$_.Threads.Count
        path = ConvertTo-Hi5SafeString $path
    }
}

[pscustomobject]@{
    action = 'processes.list'
    status = 'ok'
    count = @($items).Count
    processes = @($items)
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

            std::string BuildProcessKillScript(const json& payload) {
                int pid = 0;
                try {
                    if (payload.contains("processId")) pid = payload.value("processId", 0);
                    if (pid <= 0 && payload.contains("pid")) pid = payload.value("pid", 0);
                } catch (...) {
                    pid = 0;
                }

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$pidToKill = )HI5PS") + std::to_string(pid) + R"HI5PS(

if ($pidToKill -le 4) {
    throw 'Refusing to terminate a protected/system PID'
}

$proc = Get-Process -Id $pidToKill -ErrorAction Stop
$name = [string]$proc.ProcessName

$blocked = @(
    'native_vp8_stream',
    'Hi5CentralAgent',
    'services',
    'csrss',
    'wininit',
    'winlogon',
    'lsass',
    'smss',
    'system'
)

if ($blocked -contains $name) {
    throw "Refusing to terminate protected process: $name"
}

Stop-Process -Id $pidToKill -Force -ErrorAction Stop
Start-Sleep -Milliseconds 700

$stillRunning = $null -ne (Get-Process -Id $pidToKill -ErrorAction SilentlyContinue)

[pscustomobject]@{
    action = 'process.kill'
    status = if ($stillRunning) { 'failed' } else { 'ok' }
    pid = $pidToKill
    name = $name
    still_running = $stillRunning
    completed_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 6 -Compress

if ($stillRunning) { exit 1 }
)HI5PS";
            }


            std::string BuildFilesListScript(const json& payload) {
                std::string targetPath = payload.value("path", std::string("C:\\"));
                if (targetPath.empty()) targetPath = "C:\\";
                const std::string psPath = PsSingleQuote(targetPath);

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
function ConvertTo-Hi5SafeString($Value) {
    if ($null -eq $Value) { return '' }
    $s = [string]$Value
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

$targetPath = )HI5PS") + psPath + R"HI5PS(

if ([string]::IsNullOrWhiteSpace($targetPath)) {
    $targetPath = 'C:\'
}

if (-not (Test-Path -LiteralPath $targetPath)) {
    throw "Path not found: $targetPath"
}

$item = Get-Item -LiteralPath $targetPath -Force -ErrorAction Stop
if (-not $item.PSIsContainer) {
    $targetPath = Split-Path -LiteralPath $targetPath -Parent
    if ([string]::IsNullOrWhiteSpace($targetPath)) { $targetPath = 'C:\' }
}

$current = Get-Item -LiteralPath $targetPath -Force -ErrorAction Stop
$parent = $null
try { $parent = Split-Path -LiteralPath $current.FullName -Parent } catch {}

$entries = Get-ChildItem -LiteralPath $current.FullName -Force -ErrorAction SilentlyContinue | Sort-Object @{Expression='PSIsContainer';Descending=$true}, Name | ForEach-Object {
    [pscustomobject]@{
        name = ConvertTo-Hi5SafeString $_.Name
        full_path = ConvertTo-Hi5SafeString $_.FullName
        type = if ($_.PSIsContainer) { 'folder' } else { 'file' }
        extension = ConvertTo-Hi5SafeString $_.Extension
        size_bytes = if ($_.PSIsContainer) { $null } else { [int64]$_.Length }
        created_at = $_.CreationTimeUtc.ToString('o')
        modified_at = $_.LastWriteTimeUtc.ToString('o')
        is_hidden = [bool](($_.Attributes -band [IO.FileAttributes]::Hidden) -ne 0)
        is_system = [bool](($_.Attributes -band [IO.FileAttributes]::System) -ne 0)
        attributes = ConvertTo-Hi5SafeString $_.Attributes
    }
}

$drives = Get-PSDrive -PSProvider FileSystem | Sort-Object Name | ForEach-Object {
    [pscustomobject]@{
        name = $_.Name
        root = $_.Root
        used_bytes = if ($null -ne $_.Used) { [int64]$_.Used } else { $null }
        free_bytes = if ($null -ne $_.Free) { [int64]$_.Free } else { $null }
    }
}

[pscustomobject]@{
    action = 'files.list'
    status = 'ok'
    path = ConvertTo-Hi5SafeString $current.FullName
    parent = ConvertTo-Hi5SafeString $parent
    count = @($entries).Count
    drives = @($drives)
    entries = @($entries)
    collected_at = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

            std::string BuildProcessRestartScript(const json& payload) {
                int pid = 0;
                try {
                    if (payload.contains("processId")) pid = payload.value("processId", 0);
                    if (pid <= 0 && payload.contains("pid")) pid = payload.value("pid", 0);
                } catch (...) { pid = 0; }

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$pidToRestart = )HI5PS") + std::to_string(pid) + R"HI5PS(
if ($pidToRestart -le 4) { throw 'Refusing to restart a protected/system PID' }

$blocked = @('native_vp8_stream','Hi5CentralAgent','services','csrss','wininit','winlogon','lsass','smss','system')
$process = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $pidToRestart) -ErrorAction Stop
$name = [string]$process.Name
if ($blocked -contains ([IO.Path]::GetFileNameWithoutExtension($name))) { throw "Refusing to restart protected process: $name" }
$commandLine = [string]$process.CommandLine
$executablePath = [string]$process.ExecutablePath
if ([string]::IsNullOrWhiteSpace($commandLine)) {
    if ([string]::IsNullOrWhiteSpace($executablePath)) { throw 'Process command line is unavailable; safe restart is not possible.' }
    $commandLine = ([char]34) + $executablePath + ([char]34)
}

Stop-Process -Id $pidToRestart -Force -ErrorAction Stop
Start-Sleep -Milliseconds 700
$created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $commandLine } -ErrorAction Stop
if ([int]$created.ReturnValue -ne 0) { throw ("Win32_Process.Create failed with code " + $created.ReturnValue) }

[pscustomobject]@{
    action='process.restart'
    status='ok'
    previous_pid=$pidToRestart
    new_pid=[int]$created.ProcessId
    name=$name
    completed_at=(Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 6 -Compress
)HI5PS";
            }

            std::string BuildServiceStartupScript(const json& payload) {
                const std::string serviceName = payload.value("serviceName", payload.value("service_name", std::string()));
                const std::string requested = payload.value("startType", payload.value("start_type", std::string()));
                const std::string psServiceName = PsSingleQuote(serviceName);
                const std::string psRequested = PsSingleQuote(requested);

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$serviceName = )HI5PS") + psServiceName + R"HI5PS(
$requested = )HI5PS" + psRequested + R"HI5PS(
if ([string]::IsNullOrWhiteSpace($serviceName)) { throw 'serviceName is required' }

$map = @{
    'automatic'='Automatic'
    'automaticdelayed'='Automatic'
    'automatic (delayed)'='Automatic'
    'manual'='Manual'
    'disabled'='Disabled'
}
$key = ([string]$requested).ToLowerInvariant().Replace('-','').Replace('_','')
if (-not $map.ContainsKey($key)) { throw 'Unsupported service startup type.' }
Set-Service -Name $serviceName -StartupType $map[$key] -ErrorAction Stop

$regPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\' + $serviceName
if ($key -eq 'automaticdelayed' -or $key -eq 'automatic (delayed)') {
    New-ItemProperty -Path $regPath -Name DelayedAutoStart -PropertyType DWord -Value 1 -Force | Out-Null
} elseif ($map[$key] -eq 'Automatic') {
    New-ItemProperty -Path $regPath -Name DelayedAutoStart -PropertyType DWord -Value 0 -Force | Out-Null
}

$svc = Get-CimInstance Win32_Service -Filter ("Name='" + $serviceName.Replace("'","''") + "'") -ErrorAction Stop
[pscustomobject]@{
    action='services.set_start_type'
    status='ok'
    service_name=$serviceName
    start_mode=[string]$svc.StartMode
    delayed_auto_start=$(try { [bool](Get-ItemPropertyValue -Path $regPath -Name DelayedAutoStart -ErrorAction Stop) } catch { $false })
    completed_at=(Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 6 -Compress
)HI5PS";
            }

            std::string BuildRegistryListScript(const json& payload) {
                const std::string path = payload.value("path", std::string("HKLM:\\"));
                const std::string psPath = PsSingleQuote(path);
                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$path = )HI5PS") + psPath + R"HI5PS(
if ([string]::IsNullOrWhiteSpace($path)) { $path = 'HKLM:\' }
if (-not (Test-Path -LiteralPath $path)) { throw "Registry path not found: $path" }

$item = Get-Item -LiteralPath $path -ErrorAction Stop
$subkeys = @(Get-ChildItem -LiteralPath $path -ErrorAction SilentlyContinue | Sort-Object PSChildName | ForEach-Object {
    [pscustomobject]@{ name=[string]$_.PSChildName; path=[string]$_.PSPath }
})
$values = @()
$properties = Get-ItemProperty -LiteralPath $path -ErrorAction Stop
foreach ($property in $properties.PSObject.Properties) {
    if ($property.Name -like 'PS*') { continue }
    $kind = ''
    try { $kind = [string]$item.GetValueKind($property.Name) } catch {}
    $display = ''
    try {
        if ($property.Value -is [array]) { $display = ($property.Value -join ', ') }
        else { $display = [string]$property.Value }
    } catch {}
    if ($display.Length -gt 4096) { $display = $display.Substring(0,4096) }
    $values += [pscustomobject]@{ name=[string]$property.Name; kind=$kind; value=$display }
}
[pscustomobject]@{
    action='registry.list'
    status='ok'
    path=[string]$item.PSPath
    provider_path=$path
    subkeys=$subkeys
    values=$values
    collected_at=(Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

            std::string BuildRegistryMutationScript(const std::string& jobType, const json& payload) {
                const std::string path = payload.value("path", std::string());
                const std::string name = payload.value("name", std::string());
                const std::string value = payload.value("value", std::string());
                const std::string kind = payload.value("kind", std::string("String"));
                const std::string psPath = PsSingleQuote(path);
                const std::string psName = PsSingleQuote(name);
                const std::string psValue = PsSingleQuote(value);
                const std::string psKind = PsSingleQuote(kind);
                const std::string psAction = PsSingleQuote(jobType);

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$path = )HI5PS") + psPath + R"HI5PS(
$name = )HI5PS" + psName + R"HI5PS(
$value = )HI5PS" + psValue + R"HI5PS(
$kind = )HI5PS" + psKind + R"HI5PS(
$action = )HI5PS" + psAction + R"HI5PS(
if ([string]::IsNullOrWhiteSpace($path)) { throw 'Registry path is required.' }

if ($action -eq 'registry.create_key') {
    if ([string]::IsNullOrWhiteSpace($name)) { throw 'Key name is required.' }
    New-Item -Path $path -Name $name -Force -ErrorAction Stop | Out-Null
} elseif ($action -eq 'registry.delete_key') {
    Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
} elseif ($action -eq 'registry.delete_value') {
    if ([string]::IsNullOrWhiteSpace($name)) { throw 'Value name is required.' }
    Remove-ItemProperty -LiteralPath $path -Name $name -Force -ErrorAction Stop
} elseif ($action -eq 'registry.set_value') {
    if ([string]::IsNullOrWhiteSpace($name)) { throw 'Value name is required.' }
    $propertyType = switch -Regex ($kind) {
        'DWord' { 'DWord'; break }
        'QWord' { 'QWord'; break }
        'ExpandString' { 'ExpandString'; break }
        'MultiString' { 'MultiString'; break }
        'Binary' { 'Binary'; break }
        default { 'String' }
    }
    $converted = $value
    if ($propertyType -eq 'DWord') { $converted = [uint32]$value }
    elseif ($propertyType -eq 'QWord') { $converted = [uint64]$value }
    elseif ($propertyType -eq 'MultiString') { $converted = @($value -split '\r?\n') }
    elseif ($propertyType -eq 'Binary') {
        $hex = ($value -replace '[^0-9A-Fa-f]','')
        if (($hex.Length % 2) -ne 0) { throw 'Binary registry data must contain complete hexadecimal byte pairs.' }
        $bytes = New-Object byte[] ($hex.Length / 2)
        for ($i=0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring($i*2,2),16) }
        $converted = $bytes
    }
    New-ItemProperty -LiteralPath $path -Name $name -PropertyType $propertyType -Value $converted -Force -ErrorAction Stop | Out-Null
} else {
    throw 'Unsupported registry action.'
}

[pscustomobject]@{
    action=$action
    status='ok'
    path=$path
    name=$name
    completed_at=(Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 6 -Compress
)HI5PS";
            }

            std::string BuildEventLogListScript(const json& payload) {
                const std::string logName = payload.value("logName", payload.value("log_name", std::string("System")));
                const std::string level = payload.value("level", std::string("All"));
                int maxEvents = 200;
                try { maxEvents = payload.value("maxEvents", payload.value("max_events", 200)); } catch (...) {}
                maxEvents = std::clamp(maxEvents, 25, 500);

                const std::string psLogName = PsSingleQuote(logName);
                const std::string psLevel = PsSingleQuote(level);

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$logName = )HI5PS") + psLogName + R"HI5PS(
$level = )HI5PS" + psLevel + R"HI5PS(
$maxEvents = )HI5PS" + std::to_string(maxEvents) + R"HI5PS(

$allowedLogs = @('System','Application','Security')
if ($allowedLogs -notcontains $logName) { throw 'Unsupported event log.' }

$filter = @{ LogName = $logName }
switch ($level) {
    'Critical' { $filter.Level = 1 }
    'Error' { $filter.Level = 2 }
    'Warning' { $filter.Level = 3 }
    'Information' { $filter.Level = 4 }
    'Verbose' { $filter.Level = 5 }
    default { }
}

$items = @(Get-WinEvent -FilterHashtable $filter -MaxEvents $maxEvents -ErrorAction Stop | ForEach-Object {
    $message = ''
    try { $message = [string]$_.Message } catch {}
    if ($message.Length -gt 6000) { $message = $message.Substring(0,6000) }
    [pscustomobject]@{
        record_id = [long]$_.RecordId
        event_id = [int]$_.Id
        provider = [string]$_.ProviderName
        level = [string]$_.LevelDisplayName
        time_created = $(if ($_.TimeCreated) { $_.TimeCreated.ToUniversalTime().ToString('o') } else { $null })
        computer = [string]$_.MachineName
        log_name = [string]$_.LogName
        message = $message
    }
})

[pscustomobject]@{
    action='events.list'
    status='ok'
    log_name=$logName
    level=$level
    count=$items.Count
    events=$items
    collected_at=(Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 8 -Compress
)HI5PS";
            }

            std::string BuildSoftwareUninstallScript(const json& payload) {
                const std::string appName = payload.value("name", std::string());
                const std::string registryKey = payload.value("registry_key", payload.value("registryKey", std::string()));
                const std::string scope = payload.value("scope", std::string());
                const std::string userProfile = payload.value("user_profile", payload.value("userProfile", std::string()));
                const std::string psName = PsSingleQuote(appName);
                const std::string psRegistryKey = PsSingleQuote(registryKey);
                const std::string psScope = PsSingleQuote(scope);
                const std::string psUserProfile = PsSingleQuote(userProfile);

                return PowerShellUtf8Preamble() + std::string(R"HI5PS(
$ErrorActionPreference = 'Stop'
$requestedName = )HI5PS") + psName + R"HI5PS(
$requestedRegistryKey = )HI5PS" + psRegistryKey + R"HI5PS(
$requestedScope = )HI5PS" + psScope + R"HI5PS(
$requestedUserProfile = )HI5PS" + psUserProfile + R"HI5PS(

function ConvertTo-Hi5SafeString($Value) {
    if ($null -eq $Value) { return '' }
    $s = [string]$Value
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

$roots = @(
    [pscustomobject]@{ scope='machine64'; path='HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' },
    [pscustomobject]@{ scope='machine32'; path='HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' }
)
$loadedHiveName = ''
if ($requestedScope -eq 'user') {
    $roots += [pscustomobject]@{ scope='user'; path='HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' }
} elseif ($requestedScope -like 'user:*') {
    $requestedSid = $requestedScope.Substring(5)
    $loadedPath = "Registry::HKEY_USERS\$requestedSid\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
    if (Test-Path -LiteralPath ("Registry::HKEY_USERS\$requestedSid")) {
        $roots += [pscustomobject]@{ scope=$requestedScope; path=$loadedPath }
    } elseif ($requestedUserProfile) {
        $ntUser = Join-Path $requestedUserProfile 'NTUSER.DAT'
        if (Test-Path -LiteralPath $ntUser) {
            $loadedHiveName = "Hi5CentralUninstall_$PID"
            & reg.exe load ("HKU\" + $loadedHiveName) $ntUser *> $null
            if ($LASTEXITCODE -eq 0) {
                $roots += [pscustomobject]@{
                    scope=$requestedScope
                    path=("Registry::HKEY_USERS\" + $loadedHiveName + "\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall")
                }
            } else {
                $loadedHiveName = ''
            }
        }
    }
}

function Close-Hi5UserHive {
    if ($loadedHiveName) {
        [GC]::Collect()
        [GC]::WaitForPendingFinalizers()
        & reg.exe unload ("HKU\" + $loadedHiveName) *> $null
        $script:loadedHiveName = ''
    }
}

function Find-Hi5Target {
    foreach ($root in $roots) {
        if ($requestedScope -and $root.scope -ne $requestedScope) { continue }
        if ($requestedRegistryKey) {
            $candidatePath = Join-Path $root.path $requestedRegistryKey
            if (Test-Path -LiteralPath $candidatePath) {
                $p = Get-ItemProperty -LiteralPath $candidatePath -ErrorAction SilentlyContinue
                if ($p -and $p.DisplayName) {
                    return [pscustomobject]@{
                        path=$candidatePath
                        scope=$root.scope
                        registry_key=$requestedRegistryKey
                        name=[string]$p.DisplayName
                        publisher=[string]$p.Publisher
                        version=[string]$p.DisplayVersion
                        uninstall_string=[string]$p.UninstallString
                        quiet_uninstall_string=[string]$p.QuietUninstallString
                    }
                }
            }
        }
    }

    if ($requestedName) {
        foreach ($root in $roots) {
            if ($requestedScope -and $root.scope -ne $requestedScope) { continue }
            if (-not (Test-Path -LiteralPath $root.path)) { continue }
            foreach ($child in Get-ChildItem -LiteralPath $root.path -ErrorAction SilentlyContinue) {
                $p = Get-ItemProperty -LiteralPath $child.PSPath -ErrorAction SilentlyContinue
                if ($p -and ([string]$p.DisplayName).Trim() -eq $requestedName.Trim()) {
                    return [pscustomobject]@{
                        path=$child.PSPath
                        scope=$root.scope
                        registry_key=$child.PSChildName
                        name=[string]$p.DisplayName
                        publisher=[string]$p.Publisher
                        version=[string]$p.DisplayVersion
                        uninstall_string=[string]$p.UninstallString
                        quiet_uninstall_string=[string]$p.QuietUninstallString
                    }
                }
            }
        }
    }
    return $null
}

function Test-Hi5ProtectedSoftware($target) {
    $combined = ("$($target.name) $($target.publisher)").ToLowerInvariant()
    if ($combined -match 'hi5central|chatpass') {
        return 'Hi5Central agent/component protection'
    }

    $knownSecurity = @(
        'microsoft defender','windows defender','crowdstrike','sentinelone','sophos',
        'bitdefender','eset','malwarebytes','webroot','cylance','carbon black',
        'symantec endpoint','trend micro','mcafee','trellix','forticlient','huntress',
        'cisco secure','cisco amp','cortex xdr','palo alto cortex','avast','avg antivirus',
        'kaspersky','f-secure','withsecure'
    )
    foreach ($term in $knownSecurity) {
        if ($combined.Contains($term)) { return "Security/AV protection: $term" }
    }

    try {
        $avProducts = Get-CimInstance -Namespace 'root/SecurityCenter2' -ClassName AntivirusProduct -ErrorAction SilentlyContinue
        foreach ($av in @($avProducts)) {
            $avName = ([string]$av.displayName).Trim()
            if (-not $avName) { continue }
            $a = $avName.ToLowerInvariant()
            if ($combined.Contains($a) -or $a.Contains(([string]$target.name).ToLowerInvariant())) {
                return "Registered antivirus product: $avName"
            }
        }
    } catch {}
    return ''
}

function Get-Hi5ProductCode($target) {
    if ([string]$target.registry_key -match '^\{[0-9A-Fa-f-]{36}\}$') { return [string]$target.registry_key }
    $match = [regex]::Match([string]$target.uninstall_string, '\{[0-9A-Fa-f-]{36}\}')
    if ($match.Success) { return $match.Value }
    return ''
}

function Test-Hi5StillInstalled {
    return $null -ne (Find-Hi5Target)
}

function Get-Hi5RebootRequired {
    $paths = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired'
    )
    foreach ($path in $paths) { try { if (Test-Path -LiteralPath $path) { return $true } } catch {} }
    try {
        $pending = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
        if ($pending) { return $true }
    } catch {}
    return $false
}

function Resolve-Hi5RegisteredCommand([string]$command) {
    if ([string]::IsNullOrWhiteSpace($command)) { return [pscustomobject]@{command=$command;path_rewritten=$false} }
    $t=$command.Trim(); $p=''; $a=''; $q=$false
    if ($t -match '^"([^"]+\.exe)"(.*)$') { $p=[string]$matches[1]; $a=[string]$matches[2]; $q=$true }
    elseif ($t -match '^([^\s"]+\.exe)(.*)$') { $p=[string]$matches[1]; $a=[string]$matches[2] }
    elseif ($t -match '^([A-Za-z]:\\.+?\.exe)(\s+[-/].*)$') { $p=[string]$matches[1]; $a=[string]$matches[2] }
    if ($p -and (Test-Path -LiteralPath $p)) {
        if (-not $q -and $p -match '\s') { return [pscustomobject]@{command=([char]34)+$p+([char]34)+$a;path_rewritten=$true} }
        return [pscustomobject]@{command=$command;path_rewritten=$false}
    }
    if ($p) {
        $s=Join-Path $env:WINDIR 'System32\config\systemprofile'; $w=Join-Path $env:WINDIR 'SysWOW64\config\systemprofile'
        if ($p.StartsWith($s,[System.StringComparison]::OrdinalIgnoreCase)) { $x=$w+$p.Substring($s.Length); if(Test-Path -LiteralPath $x){return [pscustomobject]@{command=([char]34)+$x+([char]34)+$a;path_rewritten=$true}} }
    }
    return [pscustomobject]@{command=$command;path_rewritten=$false}
}

function Add-Hi5Candidate([System.Collections.ArrayList]$list, [string]$strategy, [string]$command) {
    if ([string]::IsNullOrWhiteSpace($command)) { return }
    $resolved = Resolve-Hi5RegisteredCommand $command
    $command = [string]$resolved.command
    foreach ($item in $list) { if ($item.command -eq $command) { return } }
    [void]$list.Add([pscustomobject]@{
        strategy=$strategy
        command=$command
        path_rewritten=[bool]$resolved.path_rewritten
    })
}

function Test-Hi5UninstallActivity($candidate) {
    $names = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $command = [string]$candidate.command
    foreach ($match in [regex]::Matches($command, '(?i)([A-Za-z0-9_.+-]+)\.exe')) {
        $name = ([string]$match.Groups[1].Value).Trim()
        if ($name -and $name -notin @('cmd','powershell','pwsh')) { [void]$names.Add($name) }
    }
    if ([string]$candidate.strategy -eq 'msi_product_code') { [void]$names.Add('msiexec') }
    foreach ($name in $names) {
        try {
            if (Get-Process -Name $name -ErrorAction SilentlyContinue) { return $true }
        } catch {}
    }
    return $false
}

function Invoke-Hi5UninstallAttempt($candidate, [int]$index) {
    $root = Join-Path $env:ProgramData 'Hi5Central\Agent\Temp'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $cmdFile = Join-Path $root ("uninstall-{0}-{1}.cmd" -f $PID,$index)
    $outFile = "$cmdFile.out"
    $errFile = "$cmdFile.err"
    $nl = [Environment]::NewLine
    $commandBody = "@echo off" + $nl + $candidate.command + $nl + "exit /b %errorlevel%" + $nl
    Set-Content -LiteralPath $cmdFile -Value $commandBody -Encoding ASCII -Force

    $started = Get-Date
    $exitCode = $null
    $timedOut = $false
    $output = ''
    try {
        $quotedCmd = ([char]34) + $cmdFile + ([char]34)
        $process = Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/s','/c',$quotedCmd) -WindowStyle Hidden -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        if (-not $process.WaitForExit(180000)) {
            $timedOut = $true
            try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch {}
        } else {
            $exitCode = [int]$process.ExitCode
        }
    } catch {
        $output = $_.Exception.Message
    }

    try {
        $stdout = if (Test-Path -LiteralPath $outFile) { Get-Content -LiteralPath $outFile -Raw -ErrorAction SilentlyContinue } else { '' }
        $stderr = if (Test-Path -LiteralPath $errFile) { Get-Content -LiteralPath $errFile -Raw -ErrorAction SilentlyContinue } else { '' }
        $output = (($output + $nl + $stdout + $nl + $stderr).Trim())
        if ($output.Length -gt 1600) { $output = $output.Substring($output.Length - 1600) }
    } catch {}
    Remove-Item -LiteralPath $cmdFile,$outFile,$errFile -Force -ErrorAction SilentlyContinue

    $stillInstalled = $true
    $verificationWaitSeconds = 0
    $verificationExtended = $false
    $probeDelays = @(1,2,3)
    foreach ($delay in $probeDelays) {
        Start-Sleep -Seconds $delay
        $verificationWaitSeconds += $delay
        $stillInstalled = Test-Hi5StillInstalled
        if (-not $stillInstalled) { break }
    }

    if ($stillInstalled -and (Test-Hi5UninstallActivity $candidate)) {
        $verificationExtended = $true
        for ($probe = 0; $probe -lt 10; $probe++) {
            Start-Sleep -Seconds 3
            $verificationWaitSeconds += 3
            $stillInstalled = Test-Hi5StillInstalled
            if (-not $stillInstalled) { break }
            if (-not (Test-Hi5UninstallActivity $candidate)) { break }
        }
    }

    return [pscustomobject]@{
        strategy = $candidate.strategy
        exit_code = $exitCode
        timed_out = $timedOut
        still_installed = $stillInstalled
        verification_wait_seconds = $verificationWaitSeconds
        verification_extended = $verificationExtended
        path_rewritten = [bool]$candidate.path_rewritten
        duration_seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        output = ConvertTo-Hi5SafeString $output
    }
}

$target = Find-Hi5Target
if (-not $target) {
    [pscustomobject]@{
        action='software.uninstall'
        status='uninstalled'
        reason='already_not_present'
        requested_name=$requestedName
        attempts=@()
        reboot_required=(Get-Hi5RebootRequired)
        completed_at=(Get-Date).ToUniversalTime().ToString('o')
    } | ConvertTo-Json -Depth 10 -Compress
    Close-Hi5UserHive
    exit 0
}

$protectedReason = Test-Hi5ProtectedSoftware $target
if ($protectedReason) {
    [pscustomobject]@{
        action='software.uninstall'
        status='blocked'
        reason='protected_security_or_agent'
        detail=$protectedReason
        name=$target.name
        publisher=$target.publisher
        version=$target.version
        attempts=@()
        reboot_required=$false
        completed_at=(Get-Date).ToUniversalTime().ToString('o')
    } | ConvertTo-Json -Depth 10 -Compress
    Close-Hi5UserHive
    exit 5
}

$candidates = [System.Collections.ArrayList]::new()
if ($target.quiet_uninstall_string) {
    Add-Hi5Candidate $candidates 'vendor_quiet_uninstall' ([string]$target.quiet_uninstall_string)
}

$productCode = Get-Hi5ProductCode $target
if ($productCode) {
    $msiLog = Join-Path $env:ProgramData ("Hi5Central\Agent\Logs\uninstall-{0}.log" -f ($productCode -replace '[{}-]',''))
    $q = [char]34
    Add-Hi5Candidate $candidates 'msi_product_code' ("msiexec.exe /x $productCode /qn /norestart REBOOT=ReallySuppress /L*v " + $q + $msiLog + $q)
}

$uninstall = ([string]$target.uninstall_string).Trim()
$lower = $uninstall.ToLowerInvariant()
if ($uninstall) {
    if ($lower -match 'unins[0-9]*\.exe') {
        Add-Hi5Candidate $candidates 'inno_silent' ($uninstall + ' /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-')
    }
    if ($lower -match '(uninstall|uninst)\.exe') {
        Add-Hi5Candidate $candidates 'nsis_silent' ($uninstall + ' /S')
    }
    if ($lower -match 'update\.exe') {
        Add-Hi5Candidate $candidates 'squirrel_silent' ($uninstall + ' --uninstall -s')
    }
    if ($lower -match 'setup\.exe') {
        Add-Hi5Candidate $candidates 'installshield_silent' ($uninstall + ' /s /v"/qn REBOOT=ReallySuppress"')
    }
    if ($lower -match 'uninstall|remove') {
        Add-Hi5Candidate $candidates 'generic_quiet' ($uninstall + ' /quiet /norestart')
        Add-Hi5Candidate $candidates 'generic_silent' ($uninstall + ' /silent /norestart')
        Add-Hi5Candidate $candidates 'generic_capital_s' ($uninstall + ' /S')
    }
}

$winget = Get-Command winget.exe -ErrorAction SilentlyContinue
if ($winget -and $target.name) {
    $q = [char]34
    $safeName = ([string]$target.name).Replace([char]34,'')
    Add-Hi5Candidate $candidates 'winget_exact_name' ("winget.exe uninstall --name " + $q + $safeName + $q + " --exact --silent --disable-interactivity --accept-source-agreements")
}

$attempts = @()
$protectionSignal = $false
$success = $false
foreach ($candidate in @($candidates)) {
    $attempt = Invoke-Hi5UninstallAttempt $candidate ($attempts.Count + 1)
    $attempts += $attempt
    if (-not $attempt.still_installed) {
        $success = $true
        break
    }
    $combinedOutput = ([string]$attempt.output).ToLowerInvariant()
    if ($combinedOutput -match 'password|passphrase|tamper protection|self.?protection|access denied|credential|authorization required') {
        $protectionSignal = $true
        break
    }
}

if ($success) {
    [pscustomobject]@{
        action='software.uninstall'
        status='uninstalled'
        reason='verified_removed'
        name=$target.name
        publisher=$target.publisher
        version=$target.version
        attempts=$attempts
        reboot_required=(Get-Hi5RebootRequired)
        completed_at=(Get-Date).ToUniversalTime().ToString('o')
    } | ConvertTo-Json -Depth 10 -Compress
    Close-Hi5UserHive
    exit 0
}

$reason = if ($protectionSignal) { 'password_or_vendor_protection_required' } elseif ($candidates.Count -eq 0) { 'no_safe_silent_uninstaller_found' } elseif ($requestedScope -like 'user:*') { 'user_context_or_silent_uninstall_failed' } else { 'silent_uninstall_failed' }
[pscustomobject]@{
    action='software.uninstall'
    status='failed'
    reason=$reason
    name=$target.name
    publisher=$target.publisher
    version=$target.version
    attempts=$attempts
    reboot_required=(Get-Hi5RebootRequired)
    completed_at=(Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json -Depth 10 -Compress
Close-Hi5UserHive
exit 1
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

                    if (jobType == "processes.list") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildProcessesListScript(), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "process.kill") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildProcessKillScript(payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "process.restart") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildProcessRestartScript(payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "files.list") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildFilesListScript(payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
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

                    if (jobType == "services.set_start_type") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildServiceStartupScript(payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "registry.list") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildRegistryListScript(payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "registry.create_key" || jobType == "registry.delete_key" ||
                        jobType == "registry.set_value" || jobType == "registry.delete_value") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildRegistryMutationScript(jobType, payload), 120);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "events.list") {
                        CommandResult cr = RunPowerShellCommand(jobId, BuildEventLogListScript(payload), 180);
                        json result = BuildCommandActionResult(std::string(), cr);
                        const bool ok = cr.error.empty() && cr.exitCode == 0 && result.value("status", std::string("ok")) != "failed";
                        PostJobResult(ident, jobId, ok, result, cr.error);
                        return;
                    }

                    if (jobType == "patch.software.bulk") {
                        json items = payload.value("items", json::array());
                        json results = json::array();
                        int succeeded = 0;
                        int failed = 0;
                        if (!items.is_array() || items.empty() || items.size() > 50) {
                            PostJobResult(ident, jobId, false, json{
                                {"success", false},
                                {"error", "bulk_patch_items_invalid"},
                                {"attemptedCount", 0}
                            }, "Bulk software patch requires between 1 and 50 items.");
                            return;
                        }

                        size_t index = 0;
                        for (const auto& item : items) {
                            index += 1;
                            if (!item.is_object()) {
                                failed += 1;
                                results.push_back(json{
                                    {"success", false},
                                    {"error", "bulk_patch_item_invalid"},
                                    {"index", static_cast<int>(index)}
                                });
                                continue;
                            }
                            std::string patchError;
                            json patchPayload = item;
                            patchPayload["action"] = "software.install";
                            const std::string childJobId = jobId + "-" + std::to_string(index);
                            json itemResult = RunPatchHostSoftwareJob(ident, childJobId, patchPayload, patchError);
                            itemResult["index"] = static_cast<int>(index);
                            itemResult["catalogueId"] = item.value("catalogueId", std::string());
                            itemResult["packageId"] = item.value("packageId", std::string());
                            itemResult["applicationName"] = item.value("applicationName", std::string());
                            if (itemResult.value("success", false)) succeeded += 1;
                            else {
                                failed += 1;
                                if (!patchError.empty()) itemResult["agentError"] = patchError;
                            }
                            results.push_back(std::move(itemResult));
                        }

                        json capabilities = json::object();
                        for (const auto& itemResult : results) {
                            if (itemResult.contains("capabilities") && itemResult["capabilities"].is_object()) {
                                capabilities = itemResult["capabilities"];
                                break;
                            }
                        }
                        json result = {
                            {"success", failed == 0},
                            {"mode", payload.value("mode", std::string("selected_catalogue"))},
                            {"attemptedCount", static_cast<int>(items.size())},
                            {"succeededCount", succeeded},
                            {"failedCount", failed},
                            {"items", results},
                            {"capabilities", capabilities}
                        };
                        PostJobResult(ident, jobId, failed == 0, result, failed == 0 ? std::string() : "One or more software updates failed.");
                        try { SendInventorySnapshotSafe(ident); } catch (...) {}
                        try { SendPatchDiscoverySafe(ident); } catch (...) {}
                        return;
                    }

                    if (jobType == "patch.software" || jobType == "patch.vendor_artifact.inspect") {
                        std::string patchError;
                        json patchPayload = payload;
                        patchPayload["action"] = jobType == "patch.vendor_artifact.inspect"
                            ? "software.inspect"
                            : "software.install";
                        json result = RunPatchHostSoftwareJob(ident, jobId, patchPayload, patchError);
                        const bool ok = result.value("success", false);
                        PostJobResult(ident, jobId, ok, result, patchError);
                        if (ok && jobType == "patch.software") {
                            try { SendInventorySnapshotSafe(ident); } catch (...) {}
                            try { SendPatchDiscoverySafe(ident); } catch (...) {}
                        }
                        return;
                    }

                    if (jobType == "software.uninstall") {
                        const std::string script = BuildSoftwareUninstallScript(payload);
                        CommandResult cr = RunPowerShellCommand(jobId, script, 900);
                        json result = BuildCommandActionResult(std::string(), cr);
                        bool ok = cr.error.empty() && result.value("status", std::string()) == "uninstalled";

                        const std::string scope = payload.value("scope", std::string());
                        if (!ok && scope.rfind("user:", 0) == 0) {
                            const std::string targetSid = scope.substr(5);
                            const std::string activeSid = ActiveInteractiveUserSid();
                            if (!targetSid.empty() && !activeSid.empty() && targetSid == activeSid) {
                                CommandResult userCr = RunPowerShellCommandInteractiveUser(jobId, script, 900);
                                json userResult = BuildCommandActionResult(std::string(), userCr);
                                userResult["execution_context"] = "interactive_user_retry";
                                userResult["system_attempt"] = {
                                    {"status", result.value("status", std::string())},
                                    {"reason", result.value("reason", std::string())},
                                    {"exit_code", result.value("exit_code", -1)}
                                };
                                result = std::move(userResult);
                                cr = std::move(userCr);
                                ok = cr.error.empty() && result.value("status", std::string()) == "uninstalled";
                            }
                            else {
                                result["interactive_retry"] = "skipped";
                                result["interactive_retry_reason"] = activeSid.empty()
                                    ? "no_active_interactive_user"
                                    : "target_user_is_not_active";
                            }
                        }

                        PostJobResult(ident, jobId, ok, result, cr.error);
                        if (ok) {
                            try { SendInventorySnapshotSafe(ident); } catch (...) {}
                        }
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

            struct TrayActionBinding {
                std::string actionId;
                std::string eventName;
                HANDLE eventHandle = nullptr;
            };

            void StartTrayLoop(AgentIdentity ident) {
                if (trayThread_.joinable()) return;
                trayThread_ = std::thread([this, ident = std::move(ident)]() mutable {
                    std::vector<TrayActionBinding> bindings;
                    HANDLE helperProcess = nullptr;
                    HANDLE helperStopEvent = nullptr;
                    DWORD helperSessionId = 0xFFFFFFFF;
                    std::string activeSignature;
                    auto nextRefresh = std::chrono::steady_clock::now();

                    auto closeBindings = [&]() {
                        for (auto& binding : bindings) {
                            if (binding.eventHandle) {
                                CloseHandle(binding.eventHandle);
                                binding.eventHandle = nullptr;
                            }
                        }
                        bindings.clear();
                    };

                    auto stopHelper = [&]() {
                        if (helperStopEvent) SetEvent(helperStopEvent);
                        if (helperProcess) {
                            DWORD wait = WaitForSingleObject(helperProcess, 1600);
                            if (wait == WAIT_TIMEOUT) {
                                LogW("[tray] helper did not exit cleanly; terminating");
                                TerminateProcess(helperProcess, 0);
                                WaitForSingleObject(helperProcess, 800);
                            }
                            CloseHandle(helperProcess);
                            helperProcess = nullptr;
                        }
                        if (helperStopEvent) {
                            CloseHandle(helperStopEvent);
                            helperStopEvent = nullptr;
                        }
                        helperSessionId = 0xFFFFFFFF;
                    };

                    while (!stop_.load()) {
                        if (helperProcess && WaitForSingleObject(helperProcess, 0) != WAIT_TIMEOUT) {
                            LogI("[tray] helper exited; scheduling relaunch");
                            CloseHandle(helperProcess);
                            helperProcess = nullptr;
                            helperSessionId = 0xFFFFFFFF;
                            nextRefresh = std::chrono::steady_clock::now();
                        }

                        const auto now = std::chrono::steady_clock::now();
                        if (now >= nextRefresh) {
                            nextRefresh = now + std::chrono::seconds(30);
                            try {
                                const std::string response = HttpGetJsonWithAgentAuth(
                                    "https://api.hi5central.com/api/v1/agent/devices/tray-policy",
                                    ident
                                );
                                const auto root = json::parse(response, nullptr, false);
                                if (root.is_discarded() || !root.value("success", false) || !root.contains("policy") || !root["policy"].is_object()) {
                                    LogW("[tray] policy response invalid");
                                }
                                else {
                                    json policy = root["policy"];
                                    const bool enabled = policy.value("enabled", false);
                                    const DWORD activeSession = WTSGetActiveConsoleSessionId();
                                    const std::string signature = policy.dump();
                                    const bool sessionValid = activeSession != 0xFFFFFFFF;
                                    const bool helperAlive = helperSessionId == activeSession && !activeSignature.empty();
                                    const bool changed = signature != activeSignature || helperSessionId != activeSession;

                                    if (!enabled || !sessionValid) {
                                        if (helperProcess || !bindings.empty()) {
                                            LogI(enabled ? "[tray] no interactive session; removing tray helper" : "[tray] policy disabled; removing tray helper");
                                        }
                                        stopHelper();
                                        closeBindings();
                                        activeSignature = signature;
                                    }
                                    else if (changed || !helperAlive) {
                                        stopHelper();
                                        closeBindings();

                                        const std::string devicePart = SafeFilePart(ident.deviceId.substr(0, std::min<size_t>(24, ident.deviceId.size())));
                                        const std::string stopEventName = "Global\\Hi5TrayStop_" + devicePart;
                                        helperStopEvent = CreateOrOpenManualResetEventA(stopEventName, false);
                                        if (helperStopEvent) ResetEvent(helperStopEvent);

                                        json helperPolicy = policy;
                                        helperPolicy["title"] = "Hi5Central";
                                        json helperActions = json::array();
                                        if (policy.contains("actions") && policy["actions"].is_array()) {
                                            size_t index = 0;
                                            for (const auto& item : policy["actions"]) {
                                                if (!item.is_object() || index >= 20) continue;
                                                const std::string actionId = item.value("id", std::string());
                                                if (actionId.empty()) continue;
                                                const std::string eventName = "Global\\Hi5TrayAction_" + devicePart + "_" + SafeFilePart(actionId);
                                                HANDLE actionEvent = CreateOrOpenManualResetEventA(eventName, false);
                                                if (!actionEvent) continue;
                                                ResetEvent(actionEvent);

                                                TrayActionBinding binding;
                                                binding.actionId = actionId;
                                                binding.eventName = eventName;
                                                binding.eventHandle = actionEvent;
                                                bindings.push_back(binding);

                                                json helperAction = item;
                                                helperAction["eventName"] = eventName;
                                                helperActions.push_back(helperAction);
                                                ++index;
                                            }
                                        }
                                        helperPolicy["actions"] = helperActions;

                                        const std::string policyText = helperPolicy.dump();
                                        const std::string policyB64 = Base64EncodeBytes(
                                            reinterpret_cast<const unsigned char*>(policyText.data()),
                                            policyText.size()
                                        );
                                        const std::string exe = UserHostExePath();
                                        const std::string args = "--mode tray --session " + QuoteArg(std::to_string(activeSession)) +
                                            " --stop-event " + QuoteArg(stopEventName) +
                                            " --policy-b64 " + QuoteArg(policyB64);

                                        helperProcess = LaunchUserHostFeature(args);
                                        if (!helperProcess) {
                                            LogW("[tray] helper launch failed session=" + std::to_string(activeSession));
                                            if (helperStopEvent) { CloseHandle(helperStopEvent); helperStopEvent = nullptr; }
                                            closeBindings();
                                            nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                                        }
                                        else {
                                            WaitForSingleObject(helperProcess, 1000);
                                            CloseHandle(helperProcess);
                                            helperProcess = nullptr;
                                            helperSessionId = activeSession;
                                            activeSignature = signature;
                                            LogI("[tray] feature registered with resident user host session=" + std::to_string(activeSession) + " actions=" + std::to_string(bindings.size()));
                                        }
                                    }
                                }
                            }
                            catch (const std::exception& ex) {
                                LogW(std::string("[tray] policy refresh failed: ") + ex.what());
                            }
                            catch (...) {
                                LogW("[tray] policy refresh failed: unknown error");
                            }
                        }

                        if (!bindings.empty()) {
                            std::vector<HANDLE> handles;
                            handles.reserve(bindings.size());
                            for (const auto& binding : bindings) handles.push_back(binding.eventHandle);
                            DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 500);
                            if (wait >= WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + static_cast<DWORD>(handles.size())) {
                                const size_t index = static_cast<size_t>(wait - WAIT_OBJECT_0);
                                ResetEvent(bindings[index].eventHandle);
                                try {
                                    json request = { {"user", ActiveConsoleUser()} };
                                    HttpPostJsonWithAgentAuth(
                                        "https://api.hi5central.com/api/v1/agent/devices/tray-actions/" + bindings[index].actionId + "/run",
                                        request.dump(),
                                        ident
                                    );
                                    // The authenticated POST helper throws on transport/non-2xx failures.
                                    // Reaching here means the API accepted and queued the pinned automation.
                                    LogI("[tray] action queued id=" + bindings[index].actionId);
                                }
                                catch (const std::exception& ex) {
                                    LogW(std::string("[tray] action request failed: ") + ex.what());
                                }
                                catch (...) {
                                    LogW("[tray] action request failed: unknown error");
                                }
                            }
                        }
                        else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        }
                    }

                    stopHelper();
                    closeBindings();
                    LogI("[tray] loop stopped");
                });
            }

            void StopTrayLoop() {
                if (trayThread_.joinable()) trayThread_.join();
            }

            void StartJobLoop(AgentIdentity ident) {
                std::thread([this, ident = std::move(ident)]() mutable {
                    while (!stop_.load()) {
                        // Interactive Device Details actions are pushed immediately over
                        // the authenticated Agent WebSocket. Keep this five-second poll as
                        // the durable fallback for queued automation and reconnect cases.
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


            std::wstring TerminalExecutablePath(const std::string& shell, const std::string& arch) {
                wchar_t windowsDir[MAX_PATH]{};
                GetWindowsDirectoryW(windowsDir, MAX_PATH);

                std::wstring base = windowsDir;

                if (arch == "x86") {
                    base += L"\\SysWOW64\\";
                } else {
                    base += L"\\System32\\";
                }

                if (shell == "cmd") {
                    return base + L"cmd.exe";
                }

                return base + L"WindowsPowerShell\\v1.0\\powershell.exe";
            }

            std::wstring TerminalCommandLine(const std::string& shell, const std::string& arch) {
                std::wstring exe = TerminalExecutablePath(shell, arch);

                if (shell == "cmd") {
                    return L"\"" + exe + L"\"";
                }

                return L"\"" + exe + L"\" -NoLogo -NoExit -NoProfile -ExecutionPolicy Bypass";
            }

            std::string CleanTerminalOutputForBrowser(const std::string& input) {
                std::string out;
                out.reserve(input.size());

                for (size_t i = 0; i < input.size(); ++i) {
                    unsigned char c = static_cast<unsigned char>(input[i]);

                    // Strip ANSI/VT escape sequences.
                    if (c == 0x1B) {
                        ++i;
                        if (i < input.size() && input[i] == '[') {
                            while (i < input.size()) {
                                unsigned char x = static_cast<unsigned char>(input[i]);
                                if (x >= 0x40 && x <= 0x7E) break;
                                ++i;
                            }
                        } else if (i < input.size() && input[i] == ']') {
                            while (i < input.size()) {
                                if (input[i] == '\a') break;
                                if (input[i] == 0x1B && i + 1 < input.size() && input[i + 1] == '\\') {
                                    ++i;
                                    break;
                                }
                                ++i;
                            }
                        }
                        continue;
                    }

                    // Drop C1 CSI bytes and common control characters.
                    if (c == 0x9B) {
                        while (i < input.size()) {
                            unsigned char x = static_cast<unsigned char>(input[i]);
                            if (x >= 0x40 && x <= 0x7E) break;
                            ++i;
                        }
                        continue;
                    }

                    if (c == '\r') continue;
                    if (c < 32 && c != '\n' && c != '\t' && c != '\b') continue;

                    out.push_back(static_cast<char>(c));
                }

                return out;
            }

            void SendTerminalMessage(const std::string& type, const std::string& sessionId, const std::string& dataOrError) {
                const bool isOutput = type == "terminal_output";
                const std::string cleaned = dataOrError;

                json msg = {
                    {"type", type},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"data", isOutput ? cleaned : ""},
                    {"error", type == "terminal_error" ? cleaned : ""}
                };

                try {
                    if (signaling_) {
                        LogI("terminal websocket send type=" + type + " session=" + sessionId + " bytes=" + std::to_string(cleaned.size()));
                        signaling_->send(msg.dump());
                    }
                } catch (...) {
                    LogW("terminal websocket send failed type=" + type + " session=" + sessionId);
                }
            }

            static bool CreatePipePair(HANDLE* readPipe, HANDLE* writePipe, bool inheritRead, bool inheritWrite) {
                SECURITY_ATTRIBUTES sa{};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;
                sa.lpSecurityDescriptor = nullptr;

                if (!CreatePipe(readPipe, writePipe, &sa, 0)) {
                    return false;
                }

                if (!inheritRead) {
                    SetHandleInformation(*readPipe, HANDLE_FLAG_INHERIT, 0);
                }

                if (!inheritWrite) {
                    SetHandleInformation(*writePipe, HANDLE_FLAG_INHERIT, 0);
                }

                return true;
            }

            void StopTerminalSession(const std::string& sessionId) {
                std::shared_ptr<TerminalSession> session;

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    auto it = terminals_.find(sessionId);
                    if (it == terminals_.end()) return;
                    session = it->second;
                    terminals_.erase(it);
                }

                session->running.store(false);

                if (session->stdinWrite) {
                    CloseHandle(session->stdinWrite);
                    session->stdinWrite = nullptr;
                }

                if (session->process) {
                    TerminateProcess(session->process, 0);
                    CloseHandle(session->process);
                    session->process = nullptr;
                }

                if (session->thread) {
                    CloseHandle(session->thread);
                    session->thread = nullptr;
                }

                if (session->pseudoConsole) {
                    ClosePseudoConsole(session->pseudoConsole);
                    session->pseudoConsole = nullptr;
                }

                if (session->stdoutRead) {
                    CloseHandle(session->stdoutRead);
                    session->stdoutRead = nullptr;
                }

                SendTerminalMessage("terminal_closed", sessionId, "");
            }

            bool StartConPtyProcess(
                const std::string& sessionId,
                const std::string& shell,
                const std::string& arch,
                short cols,
                short rows,
                std::shared_ptr<TerminalSession>& sessionOut,
                std::string& errorOut
            ) {
                HANDLE inputRead = nullptr;
                HANDLE inputWrite = nullptr;
                HANDLE outputRead = nullptr;
                HANDLE outputWrite = nullptr;

                if (!CreatePipePair(&inputRead, &inputWrite, true, false)) {
                    errorOut = "CreatePipe input failed err=" + std::to_string(GetLastError());
                    return false;
                }

                if (!CreatePipePair(&outputRead, &outputWrite, false, true)) {
                    CloseHandle(inputRead);
                    CloseHandle(inputWrite);
                    errorOut = "CreatePipe output failed err=" + std::to_string(GetLastError());
                    return false;
                }

                COORD size{};
                size.X = cols > 0 ? cols : 120;
                size.Y = rows > 0 ? rows : 32;

                HPCON hpc = nullptr;
                HRESULT hr = CreatePseudoConsole(size, inputRead, outputWrite, 0, &hpc);

                CloseHandle(inputRead);
                CloseHandle(outputWrite);

                if (FAILED(hr)) {
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    char buf[64]{};
                    sprintf_s(buf, "0x%08lx", static_cast<unsigned long>(hr));
                    errorOut = std::string("CreatePseudoConsole failed hr=") + buf;
                    return false;
                }

                STARTUPINFOEXW si{};
                si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

                SIZE_T attrListSize = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);

                std::vector<char> attrListBuffer(attrListSize);
                si.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrListBuffer.data());

                if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrListSize)) {
                    ClosePseudoConsole(hpc);
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    errorOut = "InitializeProcThreadAttributeList failed err=" + std::to_string(GetLastError());
                    return false;
                }

                if (!UpdateProcThreadAttribute(
                    si.lpAttributeList,
                    0,
                    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                    hpc,
                    sizeof(HPCON),
                    nullptr,
                    nullptr
                )) {
                    DeleteProcThreadAttributeList(si.lpAttributeList);
                    ClosePseudoConsole(hpc);
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    errorOut = "UpdateProcThreadAttribute failed err=" + std::to_string(GetLastError());
                    return false;
                }

                PROCESS_INFORMATION pi{};
                std::wstring cmdLine = TerminalCommandLine(shell, arch);

                BOOL ok = CreateProcessW(
                    nullptr,
                    cmdLine.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                    nullptr,
                    nullptr,
                    &si.StartupInfo,
                    &pi
                );

                DeleteProcThreadAttributeList(si.lpAttributeList);

                if (!ok) {
                    DWORD err = GetLastError();
                    ClosePseudoConsole(hpc);
                    CloseHandle(inputWrite);
                    CloseHandle(outputRead);
                    errorOut = "CreateProcess terminal failed err=" + std::to_string(err);
                    return false;
                }

                auto session = std::make_shared<TerminalSession>();
                session->sessionId = sessionId;
                session->pseudoConsole = hpc;
                session->process = pi.hProcess;
                session->thread = pi.hThread;
                session->stdinWrite = inputWrite;
                session->stdoutRead = outputRead;
                session->running.store(true);

                sessionOut = session;
                return true;
            }

            void StartTerminalSession(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string shell = msg.value("shell", std::string("powershell"));
                const std::string runAs = msg.value("runAs", msg.value("run_as", std::string("admin")));
                const std::string arch = msg.value("arch", std::string("x64"));
                const short cols = static_cast<short>(msg.value("cols", 120));
                const short rows = static_cast<short>(msg.value("rows", 32));

                LogI("terminal start requested session=" + sessionId + " shell=" + shell + " runAs=" + runAs + " arch=" + arch);

                if (sessionId.empty()) return;

                if (runAs == "user") {
                    SendTerminalMessage("terminal_error", sessionId, "Signed-in user terminal mode is coming next. Admin/service mode is available now.");
                    return;
                }

                StopTerminalSession(sessionId);

                std::shared_ptr<TerminalSession> session;
                std::string error;

                if (!StartConPtyProcess(sessionId, shell, arch, cols, rows, session, error)) {
                    LogW("terminal start failed session=" + sessionId + " error=" + error);
                    SendTerminalMessage("terminal_error", sessionId, error);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    terminals_[sessionId] = session;
                }

                LogI("terminal started session=" + sessionId);

                session->reader = std::thread([this, session]() {
                    char buffer[8192];

                    while (session->running.load()) {
                        DWORD read = 0;
                        BOOL ok = ReadFile(session->stdoutRead, buffer, sizeof(buffer), &read, nullptr);

                        if (!ok || read == 0) {
                            break;
                        }

                        LogI("terminal output read session=" + session->sessionId + " bytes=" + std::to_string(read));
                        SendTerminalMessage("terminal_output", session->sessionId, std::string(buffer, read));
                    }

                    session->running.store(false);
                    SendTerminalMessage("terminal_closed", session->sessionId, "");
                    LogI("terminal reader exited session=" + session->sessionId);
                });

                session->reader.detach();
            }

            void SendTerminalInput(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string data = msg.value("data", std::string());

                if (sessionId.empty()) return;

                std::shared_ptr<TerminalSession> session;

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    auto it = terminals_.find(sessionId);
                    if (it == terminals_.end()) {
                        LogW("terminal input received but session not found session=" + sessionId);
                        SendTerminalMessage("terminal_error", sessionId, "Terminal session not found");
                        return;
                    }
                    session = it->second;
                }

                if (!session->stdinWrite) return;

                DWORD written = 0;
                BOOL ok = WriteFile(session->stdinWrite, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);

                if (!ok) {
                    DWORD err = GetLastError();
                    LogW("terminal input write failed session=" + sessionId + " err=" + std::to_string(err));
                    SendTerminalMessage("terminal_error", sessionId, "Terminal input failed err=" + std::to_string(err));
                    return;
                }

                LogI("terminal input written session=" + sessionId + " bytes=" + std::to_string(written));
            }

            void ResizeTerminalSession(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const short cols = static_cast<short>(msg.value("cols", 120));
                const short rows = static_cast<short>(msg.value("rows", 32));

                std::shared_ptr<TerminalSession> session;

                {
                    std::lock_guard<std::mutex> lock(terminalsMutex_);
                    auto it = terminals_.find(sessionId);
                    if (it == terminals_.end()) return;
                    session = it->second;
                }

                if (!session->pseudoConsole) return;

                COORD size{};
                size.X = cols > 0 ? cols : 120;
                size.Y = rows > 0 ? rows : 32;
                ResizePseudoConsole(session->pseudoConsole, size);
            }



            std::string Base64EncodeBytes(const unsigned char* data, size_t len) {
                static const char* table =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

                std::string out;
                out.reserve(((len + 2) / 3) * 4);

                for (size_t i = 0; i < len; i += 3) {
                    const unsigned int b0 = data[i];
                    const unsigned int b1 = (i + 1 < len) ? data[i + 1] : 0;
                    const unsigned int b2 = (i + 2 < len) ? data[i + 2] : 0;

                    const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;

                    out.push_back(table[(triple >> 18) & 0x3F]);
                    out.push_back(table[(triple >> 12) & 0x3F]);
                    out.push_back((i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=');
                    out.push_back((i + 2 < len) ? table[triple & 0x3F] : '=');
                }

                return out;
            }

            void SendFilesMessage(
                const std::string& type,
                const std::string& sessionId,
                const std::string& path,
                const json& result,
                const std::string& errorMessage = std::string()
            ) {
                json msg = {
                    {"type", type},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"path", path},
                    {"result", result}
                };

                if (type == "files_result") {
                    if (result.contains("entries") && result["entries"].is_array()) {
                        msg["entries"] = result["entries"];
                    } else if (result.contains("files") && result["files"].is_array()) {
                        msg["entries"] = result["files"];
                    } else if (result.contains("items") && result["items"].is_array()) {
                        msg["entries"] = result["items"];
                    } else {
                        msg["entries"] = json::array();
                    }

                    if (result.contains("drives")) {
                        msg["drives"] = result["drives"];
                    }

                    if (result.contains("parent")) {
                        msg["parent"] = result["parent"];
                    }

                    if (result.contains("count")) {
                        msg["count"] = result["count"];
                    }
                }

                if (type == "files_error") {
                    msg["error"] = errorMessage.empty() ? "File listing failed" : errorMessage;
                }

                try {
                    if (signaling_) {
                        LogI("files websocket send type=" + type +
                             " session=" + sessionId +
                             " path=" + path +
                             " entries=" + std::to_string(msg.value("entries", json::array()).size()));
                        signaling_->send(msg.dump());
                    }
                } catch (...) {
                    LogW("files websocket send failed type=" + type + " session=" + sessionId);
                }
            }



            std::vector<unsigned char> Base64DecodeBytes(const std::string& input) {
                static const int table[256] = {
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
                    -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
                    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
                };

                std::vector<unsigned char> out;
                int val = 0;
                int valb = -8;

                for (unsigned char c : input) {
                    int d = table[c];
                    if (d == -1) continue;
                    if (d == -2) break;

                    val = (val << 6) + d;
                    valb += 6;

                    if (valb >= 0) {
                        out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
                        valb -= 8;
                    }
                }

                return out;
            }

            void SendFileDownloadJson(const json& msg) {
                try {
                    if (signaling_) {
                        signaling_->send(msg.dump());
                    }
                } catch (...) {
                    LogW("files download websocket send failed");
                }
            }

            void HandleLiveFileDownloadRequest(const json& msg) {
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string transferId = msg.value("transferId", msg.value("transfer_id", std::string()));
                const std::string path = msg.value("path", std::string());

                if (sessionId.empty() || transferId.empty() || path.empty()) {
                    LogW("files download request missing session/transfer/path");
                    return;
                }

                std::thread([this, sessionId, transferId, path]() {
                    const unsigned long long maxBytes = 10ull * 1024ull * 1024ull * 1024ull;
                    const DWORD chunkSize = 64 * 1024;

                    try {
                        DWORD attrs = GetFileAttributesA(path.c_str());
                        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "File not found or path is a folder"}
                            });
                            return;
                        }

                        HANDLE file = CreateFileA(
                            path.c_str(),
                            GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr
                        );

                        if (file == INVALID_HANDLE_VALUE) {
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "Could not open file. Error " + std::to_string(GetLastError())}
                            });
                            return;
                        }

                        LARGE_INTEGER size{};
                        if (!GetFileSizeEx(file, &size)) {
                            CloseHandle(file);
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "Could not read file size"}
                            });
                            return;
                        }

                        const unsigned long long totalBytes = static_cast<unsigned long long>(size.QuadPart);
                        if (totalBytes > maxBytes) {
                            CloseHandle(file);
                            SendFileDownloadJson({
                                {"type", "files_error"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"error", "File is larger than the current 10 GB download limit"}
                            });
                            return;
                        }

                        std::string filename = path;
                        size_t slash = filename.find_last_of("\\/");
                        if (slash != std::string::npos) {
                            filename = filename.substr(slash + 1);
                        }
                        if (filename.empty()) filename = "download.bin";

                        SendFileDownloadJson({
                            {"type", "files_download_start"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"filename", filename},
                            {"size", totalBytes},
                            {"size_bytes", totalBytes}
                        });

                        std::vector<unsigned char> buffer(chunkSize);
                        unsigned long long offset = 0;
                        uint32_t index = 0;

                        for (;;) {
                            DWORD read = 0;
                            BOOL ok = ReadFile(file, buffer.data(), chunkSize, &read, nullptr);

                            if (!ok) {
                                const DWORD err = GetLastError();
                                CloseHandle(file);
                                SendFileDownloadJson({
                                    {"type", "files_error"},
                                    {"sessionId", sessionId},
                                    {"session_id", sessionId},
                                    {"transferId", transferId},
                                    {"transfer_id", transferId},
                                    {"path", path},
                                    {"error", "File read failed. Error " + std::to_string(err)}
                                });
                                return;
                            }

                            if (read == 0) break;

                            const std::string encoded = Base64EncodeBytes(buffer.data(), static_cast<size_t>(read));

                            SendFileDownloadJson({
                                {"type", "files_download_chunk"},
                                {"sessionId", sessionId},
                                {"session_id", sessionId},
                                {"transferId", transferId},
                                {"transfer_id", transferId},
                                {"path", path},
                                {"index", index},
                                {"offset", offset},
                                {"bytes", read},
                                {"data", encoded}
                            });

                            offset += read;
                            index += 1;
                        }

                        CloseHandle(file);

                        SendFileDownloadJson({
                            {"type", "files_download_complete"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"filename", filename},
                            {"size", totalBytes},
                            {"size_bytes", totalBytes},
                            {"chunks", index}
                        });

                        LogI("files download completed session=" + sessionId +
                             " path=" + path +
                             " bytes=" + std::to_string(totalBytes) +
                             " chunks=" + std::to_string(index));
                        LogSupportEvent("File downloaded: " + filename);
                    } catch (const std::exception& ex) {
                        SendFileDownloadJson({
                            {"type", "files_error"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"error", ex.what()}
                        });
                    } catch (...) {
                        SendFileDownloadJson({
                            {"type", "files_error"},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"path", path},
                            {"error", "Unknown file download error"}
                        });
                    }
                }).detach();
            }


            void SendFilesActionResult(
                const std::string& sessionId,
                const std::string& action,
                bool success,
                const std::string& refreshPath,
                const std::string& message,
                const std::string& error = std::string()
            ) {
                json out = {
                    {"type", "files_action_result"},
                    {"sessionId", sessionId},
                    {"session_id", sessionId},
                    {"action", action},
                    {"success", success},
                    {"refreshPath", refreshPath},
                    {"refresh_path", refreshPath},
                    {"message", message},
                    {"error", error}
                };

                try {
                    if (signaling_) {
                        signaling_->send(out.dump());
                    }
                } catch (...) {
                    LogW("files action result websocket send failed action=" + action + " session=" + sessionId);
                }
            }

            void HandleLiveFileActionRequest(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string pathValue = msg.value("path", std::string());
                const std::string nameValue = msg.value("name", std::string());
                std::string refreshPath = msg.value("currentPath", msg.value("current_path", std::string()));

                if (sessionId.empty()) return;

                if (refreshPath.empty()) {
                    refreshPath = pathValue.empty() ? "C:\\" : pathValue;
                }

                if (type == "files_mkdir_request") {
                    if (nameValue.empty()) {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Folder name is required");
                        return;
                    }

                    if (nameValue.find("\\") != std::string::npos ||
                        nameValue.find("/") != std::string::npos ||
                        nameValue.find(":") != std::string::npos ||
                        nameValue == "." ||
                        nameValue == "..") {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Invalid folder name");
                        return;
                    }

                    std::string basePath = refreshPath.empty() ? pathValue : refreshPath;
                    if (basePath.empty()) {
                        basePath = "C:\\";
                    }

                    if (!basePath.empty() && basePath.back() != '\\' && basePath.back() != '/') {
                        basePath += "\\";
                    }

                    const std::string fullPath = basePath + nameValue;

                    LogI("live files mkdir requested session=" + sessionId + " path=" + fullPath);

                    BOOL created = CreateDirectoryA(fullPath.c_str(), nullptr);
                    DWORD err = created ? ERROR_SUCCESS : GetLastError();

                    if (!created && err == ERROR_ALREADY_EXISTS) {
                        DWORD attrs = GetFileAttributesA(fullPath.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                            SendFilesActionResult(sessionId, type, true, refreshPath, "Folder already exists");
                            return;
                        }
                    }

                    if (!created && err != ERROR_SUCCESS) {
                        LogW("live files mkdir failed session=" + sessionId +
                             " path=" + fullPath +
                             " err=" + std::to_string(err));

                        SendFilesActionResult(
                            sessionId,
                            type,
                            false,
                            refreshPath,
                            "",
                            "Create folder failed. Windows error " + std::to_string(err)
                        );
                        return;
                    }

                    LogI("live files mkdir completed session=" + sessionId + " path=" + fullPath);

                    SendFilesActionResult(
                        sessionId,
                        type,
                        true,
                        refreshPath,
                        "Folder created"
                    );

                    return;
                }

                if (type == "files_rename_request") {
                    if (pathValue.empty() || nameValue.empty()) {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Path and new name are required");
                        return;
                    }

                    const std::string ps =
                        PowerShellUtf8Preamble() +
                        "$path = " + PsSingleQuote(pathValue) + "\n" +
                        "$name = " + PsSingleQuote(nameValue) + "\n" +
                        "Rename-Item -LiteralPath $path -NewName $name -Force\n" +
                        "[pscustomobject]@{ status='ok'; action='rename'; path=$path; name=$name } | ConvertTo-Json -Compress\n";

                    std::thread([this, sessionId, type, refreshPath, ps]() {
                        CommandResult cr = RunPowerShellCommand("live-file-action-" + sessionId, ps, 60);
                        const bool ok = cr.error.empty() && cr.exitCode == 0;
                        SendFilesActionResult(
                            sessionId,
                            type,
                            ok,
                            refreshPath,
                            ok ? "Renamed" : "",
                            ok ? "" : (cr.error.empty() ? "Rename failed" : cr.error)
                        );
                    }).detach();

                    return;
                }

                if (type == "files_delete_request") {
                    if (pathValue.empty()) {
                        SendFilesActionResult(sessionId, type, false, refreshPath, "", "Path is required");
                        return;
                    }

                    const std::string ps =
                        PowerShellUtf8Preamble() +
                        "$path = " + PsSingleQuote(pathValue) + "\n" +
                        "Remove-Item -LiteralPath $path -Force -Recurse\n" +
                        "[pscustomobject]@{ status='ok'; action='delete'; path=$path } | ConvertTo-Json -Compress\n";

                    std::thread([this, sessionId, type, refreshPath, ps]() {
                        CommandResult cr = RunPowerShellCommand("live-file-action-" + sessionId, ps, 60);
                        const bool ok = cr.error.empty() && cr.exitCode == 0;
                        SendFilesActionResult(
                            sessionId,
                            type,
                            ok,
                            refreshPath,
                            ok ? "Deleted" : "",
                            ok ? "" : (cr.error.empty() ? "Delete failed" : cr.error)
                        );
                    }).detach();

                    return;
                }

                SendFilesActionResult(sessionId, type, false, refreshPath, "", "Unknown file action");
            }


            void SendFileUploadJson(const json& msg) {
                try {
                    if (signaling_) signaling_->send(msg.dump());
                } catch (...) {
                    LogW("files upload websocket send failed");
                }
            }

            void HandleLiveFileUploadMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                const std::string transferId = msg.value("transferId", msg.value("transfer_id", std::string()));
                const std::string directory = msg.value("directory", msg.value("path", std::string("C:\\")));
                const std::string filename = msg.value("filename", std::string());

                struct UploadState {
                    HANDLE handle = INVALID_HANDLE_VALUE;
                    std::string tempPath;
                    std::string finalPath;
                    std::string directory;
                    std::string filename;
                    unsigned long long received = 0;
                    unsigned long long expected = 0;
                };

                static std::mutex uploadMutex;
                static std::unordered_map<std::string, UploadState> uploads;

                if (sessionId.empty() || transferId.empty()) return;

                if (type == "files_upload_start") {
                    if (filename.empty()) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Filename is required"}
                        });
                        return;
                    }

                    if (filename.find("\\") != std::string::npos || filename.find("/") != std::string::npos || filename.find(":") != std::string::npos) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Invalid filename"}
                        });
                        return;
                    }

                    std::string base = directory.empty() ? "C:\\" : directory;
                    if (!base.empty() && base.back() != '\\' && base.back() != '/') base += "\\";

                    const std::string finalPath = base + filename;
                    const std::string tempPath = finalPath + ".hi5upload-" + transferId + ".tmp";

                    HANDLE handle = CreateFileA(
                        tempPath.c_str(),
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    );

                    if (handle == INVALID_HANDLE_VALUE) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Could not create upload temp file. Windows error " + std::to_string(GetLastError())}
                        });
                        return;
                    }

                    UploadState state;
                    state.handle = handle;
                    state.tempPath = tempPath;
                    state.finalPath = finalPath;
                    state.directory = directory;
                    state.filename = filename;
                    state.expected = static_cast<unsigned long long>(msg.value("size_bytes", msg.value("size", 0ull)));

                    {
                        std::lock_guard<std::mutex> lock(uploadMutex);
                        uploads[transferId] = state;
                    }

                    LogI("files upload started session=" + sessionId + " path=" + finalPath);

                    SendFileUploadJson({
                        {"type", "files_upload_progress"},
                        {"sessionId", sessionId},
                        {"session_id", sessionId},
                        {"transferId", transferId},
                        {"transfer_id", transferId},
                        {"filename", filename},
                        {"receivedBytes", 0},
                        {"received_bytes", 0},
                        {"size", state.expected},
                        {"size_bytes", state.expected}
                    });

                    return;
                }

                if (type == "files_upload_chunk") {
                    std::string data = msg.value("data", std::string());
                    std::vector<unsigned char> bytes = Base64DecodeBytes(data);

                    std::lock_guard<std::mutex> lock(uploadMutex);
                    auto it = uploads.find(transferId);
                    if (it == uploads.end() || it->second.handle == INVALID_HANDLE_VALUE) return;

                    DWORD written = 0;
                    BOOL ok = WriteFile(
                        it->second.handle,
                        bytes.data(),
                        static_cast<DWORD>(bytes.size()),
                        &written,
                        nullptr
                    );

                    if (!ok || written != bytes.size()) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"error", "Upload write failed. Windows error " + std::to_string(GetLastError())}
                        });
                        CloseHandle(it->second.handle);
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                        return;
                    }

                    it->second.received += written;

                    SendFileUploadJson({
                        {"type", "files_upload_progress"},
                        {"sessionId", sessionId},
                        {"session_id", sessionId},
                        {"transferId", transferId},
                        {"transfer_id", transferId},
                        {"filename", it->second.filename},
                        {"index", msg.value("index", 0)},
                        {"receivedBytes", it->second.received},
                        {"received_bytes", it->second.received},
                        {"size", it->second.expected},
                        {"size_bytes", it->second.expected}
                    });

                    return;
                }

                if (type == "files_upload_complete") {
                    std::lock_guard<std::mutex> lock(uploadMutex);
                    auto it = uploads.find(transferId);
                    if (it == uploads.end()) return;

                    LogI("files upload complete requested session=" + sessionId +
                         " file=" + it->second.filename +
                         " received=" + std::to_string(it->second.received) +
                         " expected=" + std::to_string(it->second.expected));

                    if (it->second.expected > 0 && it->second.received != it->second.expected) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"refreshPath", it->second.directory},
                            {"refresh_path", it->second.directory},
                            {"error", "Upload incomplete. Received " + std::to_string(it->second.received) + " of " + std::to_string(it->second.expected) + " bytes"}
                        });
                        CloseHandle(it->second.handle);
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                        return;
                    }

                    FlushFileBuffers(it->second.handle);
                    CloseHandle(it->second.handle);
                    it->second.handle = INVALID_HANDLE_VALUE;

                    DeleteFileA(it->second.finalPath.c_str());

                    BOOL moved = MoveFileExA(
                        it->second.tempPath.c_str(),
                        it->second.finalPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED
                    );
                    if (!moved) {
                        SendFileUploadJson({
                            {"type", "files_upload_result"},
                            {"success", false},
                            {"sessionId", sessionId},
                            {"session_id", sessionId},
                            {"transferId", transferId},
                            {"transfer_id", transferId},
                            {"refreshPath", it->second.directory},
                            {"refresh_path", it->second.directory},
                            {"error", "Could not finalise uploaded file. Windows error " + std::to_string(GetLastError())}
                        });
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                        return;
                    }

                    LogI("files upload completed session=" + sessionId + " path=" + it->second.finalPath + " bytes=" + std::to_string(it->second.received));
                    LogSupportEvent("File uploaded: " + it->second.filename);

                    SendFileUploadJson({
                        {"type", "files_upload_result"},
                        {"success", true},
                        {"sessionId", sessionId},
                        {"session_id", sessionId},
                        {"transferId", transferId},
                        {"transfer_id", transferId},
                        {"filename", it->second.filename},
                        {"refreshPath", it->second.directory},
                        {"refresh_path", it->second.directory},
                        {"message", "Upload complete"}
                    });

                    uploads.erase(it);
                    return;
                }

                if (type == "files_upload_cancel") {
                    std::lock_guard<std::mutex> lock(uploadMutex);
                    auto it = uploads.find(transferId);
                    if (it != uploads.end()) {
                        if (it->second.handle != INVALID_HANDLE_VALUE) CloseHandle(it->second.handle);
                        DeleteFileA(it->second.tempPath.c_str());
                        uploads.erase(it);
                    }
                    return;
                }
            }

            void HandleLiveFilesMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                std::string targetPath = msg.value("path", msg.value("filePath", std::string("C:\\")));

                if (sessionId.empty()) {
                    LogW("live files message missing session id type=" + type);
                    return;
                }

                if (targetPath.empty()) {
                    targetPath = "C:\\";
                }

                if (type == "files_cancel" || type == "files_download_cancel") {
                    LogI("live files cancel requested session=" + sessionId);
                    return;
                }

                if (type == "files_download_request") {
                    HandleLiveFileDownloadRequest(msg);
                    return;
                }

                if (type == "files_upload_start" || type == "files_upload_chunk" || type == "files_upload_complete" || type == "files_upload_cancel") {
                    HandleLiveFileUploadMessage(msg);
                    return;
                }

                if (type == "files_rename_request" || type == "files_delete_request" || type == "files_mkdir_request") {
                    HandleLiveFileActionRequest(msg);
                    return;
                }

                if (type != "files_list") {
                    LogW("unknown live files message type=" + type + " session=" + sessionId);
                    return;
                }

                LogI("live files list requested session=" + sessionId + " path=" + targetPath);

                std::thread([this, sessionId, targetPath]() {
                    try {
                        json payload = {
                            {"path", targetPath}
                        };

                        CommandResult cr = RunPowerShellCommand(
                            std::string("live-files-") + sessionId,
                            BuildFilesListScript(payload),
                            60
                        );

                        json result = BuildCommandActionResult(std::string(), cr);

                        const bool ok =
                            cr.error.empty() &&
                            cr.exitCode == 0 &&
                            result.value("status", std::string("ok")) != "failed";

                        if (!ok) {
                            std::string error = cr.error;
                            if (error.empty() && result.contains("error")) {
                                error = result.value("error", std::string());
                            }
                            if (error.empty()) {
                                error = "File listing failed";
                            }

                            LogW("live files list failed session=" + sessionId +
                                 " path=" + targetPath +
                                 " error=" + error);

                            SendFilesMessage("files_error", sessionId, targetPath, json::object(), error);
                            return;
                        }

                        const std::string resultPath = result.value("path", targetPath);

                        LogI("live files list completed session=" + sessionId +
                             " path=" + resultPath +
                             " entries=" + std::to_string(result.value("entries", json::array()).size()));

                        SendFilesMessage("files_result", sessionId, resultPath, result);
                    } catch (const std::exception& ex) {
                        LogW(std::string("live files exception session=") + sessionId + " error=" + ex.what());
                        SendFilesMessage("files_error", sessionId, targetPath, json::object(), ex.what());
                    } catch (...) {
                        LogW("live files unknown exception session=" + sessionId);
                        SendFilesMessage("files_error", sessionId, targetPath, json::object(), "Unknown file listing error");
                    }
                }).detach();
            }

            void HandleTerminalMessage(const json& msg) {
                const std::string type = msg.value("type", std::string());
                const std::string sessionIdForLog = msg.value("sessionId", msg.value("session_id", std::string()));

                LogI("terminal message received type=" + type + " session=" + sessionIdForLog);

                if (type == "terminal_start") {
                    StartTerminalSession(msg);
                    return;
                }

                if (type == "terminal_input") {
                    SendTerminalInput(msg);
                    return;
                }

                if (type == "terminal_stop") {
                    const std::string sessionId = msg.value("sessionId", msg.value("session_id", std::string()));
                    StopTerminalSession(sessionId);
                    return;
                }

                if (type == "terminal_resize") {
                    ResizeTerminalSession(msg);
                    return;
                }
            }

            void SendInventorySnapshotSafe(const AgentIdentity& ident) {
                if (!signaling_) return;
                try {
                    auto snapshot = hi5::BuildInventorySnapshot(ident);
                    signaling_->send(snapshot.dump());
                    LogI("inventory_snapshot sent collected_at=" + snapshot.value("collected_at", std::string()));
                }
                catch (const std::exception& ex) {
                    LogW(std::string("inventory_snapshot failed: ") + ex.what());
                }
                catch (...) {
                    LogW("inventory_snapshot failed: unknown error");
                }
            }

            void StartInventoryLoop(AgentIdentity ident) {
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

            void SendPatchDiscoverySafe(const AgentIdentity& ident) {
                const std::string patchHost = PatchHostExePath();
                if (patchHost.empty()) {
                    LogI("[patchhost] executable not installed; discovery skipped");
                    return;
                }

                namespace fs = std::filesystem;
                const fs::path root = fs::path(LR"(C:\ProgramData\Hi5Central\Agent\PatchHost)");
                std::error_code ec;
                fs::create_directories(root, ec);
                if (ec || !ApplyPatchDirectoryAcl(root)) {
                    LogW("[patchhost] unable to secure working directory; discovery skipped");
                    return;
                }
                const fs::path outputPath = root / L"discovery.json";
                fs::remove(outputPath, ec);

                const std::string args = "--discover-software --output " + QuoteArg(outputPath.string());
                HANDLE process = LaunchServiceChildProcess(patchHost, args);
                if (!process) {
                    LogW("[patchhost] failed to launch discovery");
                    return;
                }

                const DWORD wait = WaitForSingleObject(process, 5 * 60 * 1000);
                DWORD exitCode = 1;
                if (wait == WAIT_TIMEOUT) {
                    TerminateProcess(process, ERROR_TIMEOUT);
                    exitCode = ERROR_TIMEOUT;
                    LogW("[patchhost] discovery timed out and was terminated");
                } else {
                    GetExitCodeProcess(process, &exitCode);
                }
                CloseHandle(process);

                std::ifstream stream(outputPath, std::ios::binary);
                if (!stream) {
                    LogW("[patchhost] discovery output missing exit_code=" + std::to_string(exitCode));
                    return;
                }

                std::ostringstream buffer;
                buffer << stream.rdbuf();
                json result = json::parse(buffer.str(), nullptr, false);
                fs::remove(outputPath, ec);
                if (result.is_discarded() || !result.is_object()) {
                    LogW("[patchhost] discovery output was invalid JSON");
                    return;
                }

                json payload = {
                    {"capabilities", result.value("capabilities", json::object())},
                    {"packages", result.value("packages", json::array())},
                    {"winget", result.value("winget", json::object())},
                    {"success", result.value("success", false)}
                };
                if (result.contains("error")) payload["error"] = result["error"];
                if (result.contains("detail")) payload["detail"] = result["detail"];

                try {
                    HttpPostJsonWithAgentAuth(
                        "https://api.hi5central.com/api/v1/agent/devices/patch-discovery",
                        payload.dump(),
                        ident
                    );
                    LogI("[patchhost] discovery posted packages=" +
                        std::to_string(payload["packages"].size()) +
                        " exit_code=" + std::to_string(exitCode));
                }
                catch (const std::exception& ex) {
                    LogW(std::string("[patchhost] discovery post failed: ") + ex.what());
                }
            }

            void StartPatchDiscoveryLoop(AgentIdentity ident) {
                std::thread([this, ident = std::move(ident)]() mutable {
                    const int intervalSeconds = ReadConfigInt(
                        "HI5_PATCH_DISCOVERY_SECONDS",
                        6 * 60 * 60,
                        15 * 60,
                        24 * 60 * 60
                    );

                    // Publish PatchHost capabilities promptly after service start so an
                    // Agent upgrade cannot leave the control plane showing stale PatchHost
                    // capabilities for the old build. Full recurring discovery remains on
                    // the normal long interval below.
                    for (int i = 0; i < 5 && !stop_.load(); ++i) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }

                    while (!stop_.load()) {
                        if (!HasActiveSessions()) {
                            SendPatchDiscoverySafe(ident);
                        } else {
                            LogI("[patchhost] discovery deferred while remote session is active");
                        }

                        for (int i = 0; i < intervalSeconds && !stop_.load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                    }
                }).detach();
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

            void StopInventoryLoop() {
                if (inventoryThread_.joinable()) {
                    inventoryThread_.join();
                }
            }

            bool HasActiveSessions() {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                return !sessions_.empty();
            }

            HANDLE SessionJobHandle(const std::string& sessionId) {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                auto it = sessions_.find(sessionId);
                return it == sessions_.end() ? nullptr : it->second->sessionJob;
            }

            bool HasSession(const std::string& sessionId) {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                return sessions_.find(sessionId) != sessions_.end();
            }

            void FlushBridgeOutgoing() {
                if (!signaling_) return;
                for (;;) {
                    auto next = sessionBridge_.PopOutgoingJson();
                    if (!next.has_value()) break;
                    signaling_->send(*next);
                }
            }

            void HandleBridgeEvent(SessionEventType ev) {
                switch (ev) {
                case SessionEventType::ChatMessageSent:
                case SessionEventType::ChatOpenRequested:
                case SessionEventType::ChatCloseRequested:
                case SessionEventType::FileListRequested:
                    FlushBridgeOutgoing();
                    break;
                default:
                    break;
                }
            }

            InputPipeWriter& ActiveInputPipe(SessionContext& ctx) {
                // Dynamic-desktop normally keeps input on the normal pipe. If the
                // visible frame is coming from a fallback Winlogon/login helper,
                // move keyboard/mouse to that helper too.
                if (ctx.secureFallbackOwnsInput.load(std::memory_order_acquire)) {
                    return ctx.secureInputPipe;
                }
                if (ctx.unifiedDesktopStreamer) return ctx.normalInputPipe;
                return (ctx.activeMode == DesktopMode::Secure)
                    ? ctx.secureInputPipe
                    : ctx.normalInputPipe;
            }

            bool SendChatOpen(InputPipeWriter& pipe) {
                InputCmd cmd{};
                cmd.type = InputCmdType::ChatOpen;
                return pipe.Write(cmd);
            }

            bool SendChatClose(InputPipeWriter& pipe) {
                InputCmd cmd{};
                cmd.type = InputCmdType::ChatClose;
                return pipe.Write(cmd);
            }

            bool SendChatBody(InputPipeWriter& pipe, const std::string& body) {
                if (body.empty()) return false;
                uint32_t offset = 0;
                if (!pipe.WriteClipboard(body.data(), static_cast<uint32_t>(body.size()), offset)) {
                    return false;
                }
                InputCmd cmd{};
                cmd.type = InputCmdType::ChatMessage;
                cmd.chatMessage.offsetInClip = offset;
                cmd.chatMessage.length = static_cast<uint32_t>(body.size());
                return pipe.Write(cmd);
            }

            void SyncChatStateToContext(SessionContext& ctx) {
                // Do not replay previous chat commands into a newly launched streamer. Replaying
                // ChatOpen/ChatMessage during restarts or secure-desktop handoff caused the native
                // chat window to reopen and close repeatedly. New technician messages are sent live
                // to ActiveInputPipe(ctx) only.
                (void)ctx;
            }

            void DispatchInputToPipe(SessionContext& ctx, const json& msg) {
                const std::string kind = msg.value("kind", "");
                const std::string type = msg.value("type", "");

                // Backstage mode commands can arrive over the WebRTC input data channel.
                // This bypasses the control-server relay, which may not forward new message types.
                if (kind == "backstage_start" || type == "backstage_start") {
                    LogI("backstage_start via datachannel session=" + ctx.sessionId);
                    HandleBackstageStart(ctx.sessionId);
                    return;
                }

                if (kind == "backstage_stop" || kind == "console_start" ||
                    type == "backstage_stop" || type == "console_start") {
                    LogI("backstage_stop/console_start via datachannel session=" + ctx.sessionId);
                    HandleBackstageStop(ctx.sessionId);
                    return;
                }

                if (kind == "local_input_block" || kind == "block_local_input" ||
                    type == "local_input_block" || type == "block_local_input") {
                    const bool blocked = msg.value("blocked", true);
                    InputCmd cmd{};
                    cmd.type = InputCmdType::LocalInputBlock;
                    cmd.localInput.blocked = blocked ? 1 : 0;
                    const bool normalQueued = ctx.normalInputPipe.Write(cmd);
                    const bool secureQueued = ctx.secureInputPipe.Write(cmd);
                    ctx.localInputBlocked = blocked;
                    LogI("local user input state requested session=" + ctx.sessionId +
                        " blocked=" + std::string(blocked ? "true" : "false") +
                        " normal_queued=" + std::string(normalQueued ? "true" : "false") +
                        " secure_queued=" + std::string(secureQueued ? "true" : "false"));
                    return;
                }

                auto& pipe = ActiveInputPipe(ctx);

                auto writeTextCommand = [&](const std::string& text) -> bool {
                    if (text.empty()) return false;
                    uint32_t offset = 0;
                    if (!pipe.WriteClipboard(text.data(), static_cast<uint32_t>(text.size()), offset)) {
                        LogW("input text command too large or clipboard buffer unavailable session=" + ctx.sessionId);
                        return false;
                    }
                    InputCmd cmd{};
                    cmd.type = InputCmdType::PasteText;
                    cmd.clipboard.offsetInClip = offset;
                    cmd.clipboard.length = static_cast<uint32_t>(text.size());
                    return pipe.Write(cmd);
                    };

                auto writeShortcutCommand = [&](ShortcutAction action) -> bool {
                    InputCmd cmd{};
                    cmd.type = InputCmdType::Shortcut;
                    cmd.shortcut.action = static_cast<uint16_t>(action);
                    return pipe.Write(cmd);
                    };
                if (kind == "text_input" || kind == "text" || kind == "insert_text" || kind == "key_text") {
                    const std::string text = msg.value("text", msg.value("key", std::string()));
                    writeTextCommand(text);
                    return;
                }

                if (kind == "key_press") {
                    const std::string key = msg.value("key", "");
                    std::string code = msg.value("code", key);

                    if (code == "Esc") code = "Escape";
                    if (code == "Del") code = "Delete";

                    WORD vk = 0;
                    bool extended = false;

                    if (!DomCodeToVk(code, vk, extended)) {
                        LogW("unknown key_press session=" + ctx.sessionId + " key=" + key + " code=" + code);
                        return;
                    }

                    InputCmd down{};
                    down.type = InputCmdType::KeyEvent;
                    down.key.vk = vk;
                    down.key.scanCode = static_cast<uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
                    down.key.down = 1;
                    down.key.isExtended = extended ? 1 : 0;
                    pipe.Write(down);

                    InputCmd up = down;
                    up.key.down = 0;
                    pipe.Write(up);
                    return;
                }

                if (kind == "shortcut" || type == "shortcut" ||
                    kind == "system_shortcut" || type == "system_shortcut" ||
                    kind == "service_shortcut" || type == "service_shortcut" ||
                    kind == "service_command" || type == "service_command") {
                    const std::string action = msg.value("action", msg.value("shortcut", std::string()));
                    if (action == "ctrl_alt_del" || action == "cad" || action == "sas" ||
                        action == "ctrl_alt_del_service" || action == "secure_attention") {
                        // Dedicated service-side SAS command. This is intentionally handled here
                        // before the input pipe fallback so the LocalSystem service gets the first
                        // chance to trigger SAS. The streamer fallback remains useful for nested
                        // VM sessions and non-SAS Ctrl+Alt+Del handling.
                        bool sasPolicyExists = false;
                        const DWORD sasPolicyValue = ReadSoftwareSasGenerationValue(&sasPolicyExists);
                        const bool sasSent = TrySendSecureAttentionSequenceFromService(ctx.sessionId);
                        LogI("ctrl_alt_del service command requested session=" + ctx.sessionId +
                            " service_sas_attempted=" + std::string(sasSent ? "1" : "0") +
                            " kind=" + kind + " type=" + type);
                        if (signaling_) {
                            const bool policyBlocked = sasPolicyExists && ((sasPolicyValue & 0x1u) == 0);
                            json result = {
                                {"type", "shortcut_result"},
                                {"session_id", ctx.sessionId},
                                {"action", "ctrl_alt_del"},
                                {"ok", sasSent},
                                {"code", sasSent ? "sent" : (policyBlocked ? "policy_blocked" : "sas_failed")},
                                {"message", sasSent ? "Ctrl+Alt+Del sent" :
                                    (policyBlocked ? "Secure attention sequence is disabled by device policy." :
                                        "Windows could not send Ctrl+Alt+Del using the secure attention API.")}
                            };
                            signaling_->send(result.dump());
                        }
                        // Preserve the normal shortcut command as a nested/VM fallback.
                        writeShortcutCommand(ShortcutAction::CtrlAltDel);
                        return;
                    }
                    if (action == "start_menu" || action == "windows_key" || action == "win") {
                        writeShortcutCommand(ShortcutAction::StartMenu);
                        return;
                    }
                    if (action == "win_d" || action == "show_desktop") { writeShortcutCommand(ShortcutAction::WinD); return; }
                    if (action == "win_r" || action == "run_dialog") { writeShortcutCommand(ShortcutAction::WinR); return; }
                    if (action == "win_e" || action == "explorer" || action == "file_explorer") { writeShortcutCommand(ShortcutAction::WinE); return; }
                    if (action == "win_tab" || action == "task_view") { writeShortcutCommand(ShortcutAction::WinTab); return; }
                    if (action == "alt_tab" || action == "app_switcher") { writeShortcutCommand(ShortcutAction::AltTab); return; }
                    if (action == "alt_tab_begin" || action == "app_switcher_begin") { writeShortcutCommand(ShortcutAction::AltTabBegin); return; }
                    if (action == "alt_tab_next" || action == "app_switcher_next") { writeShortcutCommand(ShortcutAction::AltTabNext); return; }
                    if (action == "alt_tab_end" || action == "app_switcher_end") { writeShortcutCommand(ShortcutAction::AltTabEnd); return; }
                    if (action == "alt_f4" || action == "close_window") { writeShortcutCommand(ShortcutAction::AltF4); return; }
                    if (action == "ctrl_shift_esc" || action == "task_manager_shortcut") { writeShortcutCommand(ShortcutAction::CtrlShiftEsc); return; }
                    if (action == "ctrl_esc") { writeShortcutCommand(ShortcutAction::CtrlEsc); return; }
                    if (action == "lock" || action == "lock_workstation") {
                        writeShortcutCommand(ShortcutAction::LockWorkstation);
                        return;
                    }
                    if (action == "task_manager" || action == "taskmgr") {
                        writeShortcutCommand(ShortcutAction::TaskManager);
                        return;
                    }
                    if (action == "explorer") {
                        writeShortcutCommand(ShortcutAction::Explorer);
                        return;
                    }
                    LogW("unknown shortcut action session=" + ctx.sessionId + " action=" + action);
                    return;
                }

                if (kind == "mouse_move") {
                    const int count = ctx.normalInputPipe.GetMonitorCount();
                    if (count <= 0) return;

                    const int idx = std::max(0, std::min(ctx.displayIndex, count - 1));
                    const auto mi = ctx.normalInputPipe.GetMonitorInfo(idx);

                    const double xn = msg.value("x_norm", 0.0);
                    const double yn = msg.value("y_norm", 0.0);

                    InputCmd cmd{};
                    cmd.type = InputCmdType::MouseMove;
                    cmd.mouseMove.x = mi.x + static_cast<int>(xn * static_cast<double>(mi.w));
                    cmd.mouseMove.y = mi.y + static_cast<int>(yn * static_cast<double>(mi.h));
                    cmd.mouseMove.remoteW = mi.w;
                    cmd.mouseMove.remoteH = mi.h;
                    cmd.mouseMove.monitorIndex = idx;
                    pipe.Write(cmd);
                    return;
                }
                auto readMouseButton = [&]() -> uint8_t {
                    if (!msg.contains("button")) return 0;

                    if (msg["button"].is_number_integer()) {
                        const int value = msg.value("button", 0);
                        if (value < 0) return 0;
                        if (value > 2) return 2;
                        return static_cast<uint8_t>(value);
                    }

                    if (msg["button"].is_string()) {
                        const std::string button = msg.value("button", "left");
                        if (button == "right") return 1;
                        if (button == "middle") return 2;
                        return 0;
                    }

                    return 0;
                };

                if (kind == "mouse_down" || kind == "mouse_up") {
                    InputCmd cmd{};
                    cmd.type = InputCmdType::MouseButton;
                    cmd.mouseButton.button = readMouseButton();
                    cmd.mouseButton.down = (kind == "mouse_down") ? 1 : 0;
                    pipe.Write(cmd);
                    return;
                }

                if (kind == "mouse_click") {
                    const uint8_t button = readMouseButton();

                    InputCmd down{};
                    down.type = InputCmdType::MouseButton;
                    down.mouseButton.button = button;
                    down.mouseButton.down = 1;
                    pipe.Write(down);

                    InputCmd up = down;
                    up.mouseButton.down = 0;
                    pipe.Write(up);
                    return;
                }

                if (kind == "wheel") {
                    InputCmd cmd{};
                    cmd.type = InputCmdType::MouseWheel;
                    cmd.mouseWheel.deltaX = msg.value("delta_x", 0);
                    cmd.mouseWheel.deltaY = msg.value("delta_y", 0);
                    pipe.Write(cmd);
                    return;
                }

                if (kind == "key_down" || kind == "key_up") {
                    WORD vk = 0;
                    bool extended = false;
                    const std::string code = msg.value("code", "");

                    // Browser KeyboardEvent.code is a physical key. That breaks UK/symbol
                    // characters when the technician and remote layouts differ. For plain
                    // printable key-downs the viewer now sends the actual character in
                    // "key"/"text"; inject that as Unicode instead of a US-style VK.
                    if (kind == "key_down" && !msg.value("ctrl", false) && !msg.value("alt", false) && !msg.value("meta", false)) {
                        std::string text = msg.value("text", msg.value("key", std::string()));
                        if (IsTextualKeyboardKey(text) &&
                            code != "ShiftLeft" && code != "ShiftRight" &&
                            code != "ControlLeft" && code != "ControlRight" &&
                            code != "AltLeft" && code != "AltRight" &&
                            code != "MetaLeft" && code != "MetaRight") {
                            writeTextCommand(text);
                            return;
                        }
                    }

                    if (!DomCodeToVk(code, vk, extended)) {
                        return;
                    }

                    InputCmd cmd{};
                    cmd.type = InputCmdType::KeyEvent;
                    cmd.key.vk = vk;
                    cmd.key.scanCode = static_cast<uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
                    cmd.key.down = (kind == "key_down") ? 1 : 0;
                    cmd.key.isExtended = extended ? 1 : 0;
                    pipe.Write(cmd);
                    return;
                }
            }

            void SendSessionState(const std::string& sessionId, const std::string& state) {
                if (!signaling_) return;
                json msg = { {"type", "session_state"}, {"session_id", sessionId}, {"state", state} };
                signaling_->send(msg.dump());
            }

            void SendSessionStateDirect(SessionContext& ctx, const std::string& state) {
                json msg = { {"type", "session_state"}, {"session_id", ctx.sessionId}, {"state", state} };
                SendSessionState(ctx.sessionId, state);
                const bool direct = SendMediaControl(ctx, json{ {"type", "viewer_control"}, {"payload", msg} });
                LogI("desktop state session=" + ctx.sessionId + " state=" + state +
                    " direct_webrtc=" + std::string(direct ? "true" : "false"));
            }

            static const char* StreamModeName(int mode) {
                switch (mode) {
                case 2: return "motion";
                case 1: return "active";
                default: return "idle";
                }
            }

            void SendStreamDiagnostics(const std::string& sessionId,
                const std::string& source,
                const hi5::StreamStats& stats,
                uint64_t framesForwarded,
                DesktopMode activeMode,
                bool backstageMode) {
                if (!signaling_) return;

                json msg = {
                    {"type", "stream_diagnostics"},
                    {"session_id", sessionId},
                    {"source", source},
                    {"codec", "VP8"},
                    {"transport", "WebRTC"},
                    {"mode", StreamModeName(stats.streamMode)},
                    {"target_fps", stats.targetFps},
                    {"capture_attempts", stats.captureAttempts},
                    {"changed_frames", stats.changedFrames},
                    {"skipped_frames", stats.skippedFrames},
                    {"written_frames", stats.writtenFrames},
                    {"input_events", stats.inputEvents},
                    {"capture_resets", stats.resetCount},
                    {"display_index", stats.displayIndex},
                    {"secure_desktop", stats.secureDesktopActive != 0},
                    {"active_desktop", activeMode == DesktopMode::Secure ? "secure" : "normal"},
                    {"backstage", backstageMode},
                    {"frames_forwarded_total", framesForwarded},
                    {"unix_ms", stats.unixMs}
                };
                signaling_->send(msg.dump());
            }

            void HandleRemoteFileListRequest(const std::string& sessionId, const std::string& rawPath) {
                namespace fs = std::filesystem;
                std::string path = rawPath.empty() ? "/" : rawPath;
                fs::path target;
                try {
#ifdef _WIN32
                    const bool listingDrives = (path == "/" || path == "drives" || path.empty());
                    if (!listingDrives) {
                        target = fs::path(path);
                    }
#else
                    const bool listingDrives = false;
                    target = fs::path(path);
#endif

                    std::vector<FileBrowserEntry> entries;
#ifdef _WIN32
                    if (listingDrives) {
                        DWORD mask = GetLogicalDrives();
                        for (char letter = 'A'; letter <= 'Z'; ++letter) {
                            if ((mask & (1u << (letter - 'A'))) == 0) continue;
                            std::string root;
                            root.push_back(letter);
                            root += ":\\\\";
                            FileBrowserEntry e;
                            e.name = root;
                            e.path = root;
                            e.isDir = true;
                            entries.push_back(std::move(e));
                        }
                    }
                    else
#endif
                        if (fs::exists(target) && fs::is_directory(target)) {
                            std::error_code ec;
                            fs::path parent = target.parent_path();
                            if (!parent.empty() && parent != target) {
                                FileBrowserEntry up;
                                up.name = "..";
                                up.path = parent.string();
                                up.isDir = true;
                                entries.push_back(std::move(up));
                            }
                            for (const auto& de : fs::directory_iterator(target, fs::directory_options::skip_permission_denied, ec)) {
                                FileBrowserEntry e;
                                e.name = de.path().filename().string();
                                e.path = de.path().string();
                                e.isDir = de.is_directory(ec);
                                if (!e.isDir) {
                                    e.size = de.file_size(ec);
                                }
                                entries.push_back(std::move(e));
                            }
                        }
                    sessionBridge_.SetRemoteFileList(path, entries);
                    json payload;
                    payload["type"] = "remote_file_list";
                    payload["session_id"] = sessionId;
                    payload["path"] = path;
                    payload["computer_name"] = LocalComputerName();
                    payload["hostname"] = payload["computer_name"];
                    payload["entries"] = json::array();
                    for (const auto& e : entries) {
                        payload["entries"].push_back({
                            {"name", e.name}, {"path", e.path}, {"is_dir", e.isDir}, {"isDir", e.isDir}, {"size", e.size}
                            });
                    }
                    SendFilePayloadToViewer(sessionId, payload);
                }
                catch (const std::exception& ex) {
                    LogW(std::string("remote file list failed path=") + path + " err=" + ex.what());
                }
            }

            static std::string Base64Encode(const std::vector<unsigned char>& data) {
                static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string out;
                out.reserve(((data.size() + 2) / 3) * 4);
                int val = 0;
                int valb = -6;
                for (unsigned char c : data) {
                    val = (val << 8) + c;
                    valb += 8;
                    while (valb >= 0) {
                        out.push_back(table[(val >> valb) & 0x3F]);
                        valb -= 6;
                    }
                }
                if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
                while (out.size() % 4) out.push_back('=');
                return out;
            }

            static std::vector<unsigned char> Base64Decode(const std::string& input) {
                static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::vector<int> T(256, -1);
                for (int i = 0; i < 64; ++i) T[static_cast<unsigned char>(table[i])] = i;
                std::vector<unsigned char> out;
                int val = 0;
                int valb = -8;
                for (unsigned char c : input) {
                    if (c == '=') break;
                    if (T[c] == -1) continue;
                    val = (val << 6) + T[c];
                    valb += 6;
                    if (valb >= 0) {
                        out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
                        valb -= 8;
                    }
                }
                return out;
            }

            static std::filesystem::path ResolveRemotePath(const std::string& rawPath) {
                namespace fs = std::filesystem;
#ifdef _WIN32
                if (rawPath.empty() || rawPath == "/" || rawPath == "drives") {
                    return fs::path();
                }
#endif
                return fs::path(rawPath);
            }

            void SendFilePayloadToViewer(const std::string& sessionId, json payload) {
                if (!signaling_) return;

                payload["session_id"] = sessionId;
                signaling_->send(payload.dump());
            }

            void SendFileError(const std::string& sessionId, const std::string& requestType, const std::string& path, const std::string& error) {
                json payload = {
                    {"type", requestType + std::string("_error")},
                    {"session_id", sessionId},
                    {"path", path},
                    {"error", error}
                };
                SendFilePayloadToViewer(sessionId, payload);
            }


            static bool IsProtectedRemotePath(const std::filesystem::path& target) {
#ifdef _WIN32
                std::string s = target.string();
                std::replace(s.begin(), s.end(), '/', '\\');
                std::string lower = s;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.size() == 3 && std::isalpha(static_cast<unsigned char>(lower[0])) && lower[1] == ':' && lower[2] == '\\') return true;
                const std::string name = target.filename().string();
                std::string n = name;
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (n == "$recycle.bin" || n == "system volume information" || n == "recovery" || n == "windows" || n == "program files" || n == "program files (x86)" || n == "programdata") return true;
                if (n == "pagefile.sys" || n == "hiberfil.sys" || n == "swapfile.sys" || n == "bootmgr") return true;
                if (lower.find("\\$recycle.bin") != std::string::npos || lower.find("\\system volume information") != std::string::npos) return true;
#else
                (void)target;
#endif
                return false;
            }

            void SendChunkedFileDownload(const std::string& sessionId, const std::string& path, const std::filesystem::path& target, std::uintmax_t size) {
                constexpr std::size_t chunkBytes = 32u * 1024u; // keep JSON/base64 WS messages safely below common proxy limits
                const std::string transferId = std::string("dl_") + std::to_string(NowUnixMs());
                json startMsg = {
                    {"type", "file_transfer_start"},
                    {"session_id", sessionId},
                    {"transfer_id", transferId},
                    {"direction", "download"},
                    {"path", path},
                    {"name", target.filename().string()},
                    {"size", size},
                    {"chunk_size", chunkBytes}
                };
                SendFilePayloadToViewer(sessionId, startMsg);

                std::ifstream in(target, std::ios::binary);
                if (!in) {
                    SendFilePayloadToViewer(sessionId, json{ {"type","file_transfer_error"},{"transfer_id",transferId},{"path",path},{"error","Could not open file for chunked download"} });
                    return;
                }
                std::vector<unsigned char> buffer(chunkBytes);
                std::uintmax_t offset = 0;
                int chunkIndex = 0;
                while (in && offset < size) {
                    const std::uintmax_t remaining = size - offset;
                    const std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, chunkBytes));
                    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
                    const std::size_t got = static_cast<std::size_t>(in.gcount());
                    if (got == 0) break;
                    std::vector<unsigned char> chunk(buffer.begin(), buffer.begin() + got);
                    json chunkMsg = {
                        {"type", "file_transfer_chunk"},
                        {"session_id", sessionId},
                        {"transfer_id", transferId},
                        {"direction", "download"},
                        {"path", path},
                        {"offset", offset},
                        {"chunk_index", chunkIndex++},
                        {"size", got},
                        {"data", Base64Encode(chunk)}
                    };
                    SendFilePayloadToViewer(sessionId, chunkMsg);
                    offset += got;
                }
                SendFilePayloadToViewer(sessionId, json{ {"type","file_transfer_complete"},{"session_id",sessionId},{"transfer_id",transferId},{"direction","download"},{"path",path},{"size",offset} });
            }

            void HandleRemoteFileDownloadRequest(const std::string& sessionId, const json& msg) {
                namespace fs = std::filesystem;
                const std::string path = msg.value("path", std::string());
                constexpr std::uintmax_t maxInlineBytes = 512ull * 1024ull; // larger files use chunked transfer to avoid WS message size limits
                try {
                    fs::path target = ResolveRemotePath(path);
                    if (target.empty() || !fs::exists(target)) {
                        SendFileError(sessionId, "remote_file_download", path, "File does not exist");
                        return;
                    }
                    if (fs::is_directory(target)) {
                        SendFileError(sessionId, "remote_file_download", path, "Remote folder download is disabled until chunked folder transfers are implemented");
                        return;
                    }
                    if (!fs::is_regular_file(target)) {
                        SendFileError(sessionId, "remote_file_download", path, "Target is not a regular file");
                        return;
                    }
                    if (IsProtectedRemotePath(target)) {
                        SendFileError(sessionId, "remote_file_download", path, "Protected/system path cannot be downloaded");
                        return;
                    }
                    const auto size = fs::file_size(target);
                    if (size > maxInlineBytes) {
                        LogI("using chunked file download path=" + path + " size=" + std::to_string(static_cast<unsigned long long>(size)));
                        SendChunkedFileDownload(sessionId, path, target, size);
                        return;
                    }
                    std::ifstream in(target, std::ios::binary);
                    if (!in) {
                        SendFileError(sessionId, "remote_file_download", path, "Could not open file for reading");
                        return;
                    }
                    std::vector<unsigned char> bytes(static_cast<size_t>(size));
                    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    json payload = {
                        {"type", "remote_file_download"},
                        {"session_id", sessionId},
                        {"path", path},
                        {"name", target.filename().string()},
                        {"size", static_cast<std::uint64_t>(bytes.size())},
                        {"encoding", "base64"},
                        {"data", Base64Encode(bytes)}
                    };
                    SendFilePayloadToViewer(sessionId, payload);
                }
                catch (const std::exception& ex) {
                    SendFileError(sessionId, "remote_file_download", path, ex.what());
                }
            }

            struct IncomingFileUpload {
                std::filesystem::path target;
                std::unique_ptr<std::ofstream> stream;
                std::uint64_t expected = 0;
                std::uint64_t received = 0;
            };

            static std::string FileUploadKey(const std::string& sessionId, const std::string& transferId) {
                return sessionId + "|" + transferId;
            }

            void HandleRemoteFileUploadStart(const std::string& sessionId, const json& msg) {
                namespace fs = std::filesystem;
                const std::string transferId = msg.value("transfer_id", std::string());
                const std::string path = msg.value("path", std::string());
                if (transferId.empty() || path.empty()) {
                    SendFileError(sessionId, "remote_file_upload", path, "Missing upload transfer id or target path");
                    return;
                }
                try {
                    fs::path target = ResolveRemotePath(path);
                    if (target.empty()) { SendFileError(sessionId, "remote_file_upload", path, "Invalid target path"); return; }
                    if (!target.parent_path().empty()) fs::create_directories(target.parent_path());
                    auto stream = std::make_unique<std::ofstream>(target, std::ios::binary | std::ios::trunc);
                    if (!*stream) { SendFileError(sessionId, "remote_file_upload", path, "Could not open file for chunked upload"); return; }
                    auto state = std::make_unique<IncomingFileUpload>();
                    state->target = target;
                    state->stream = std::move(stream);
                    state->expected = msg.value("size", static_cast<std::uint64_t>(0));
                    { std::lock_guard<std::mutex> lock(fileUploadsMu_); fileUploads_[FileUploadKey(sessionId, transferId)] = std::move(state); }
                    SendFilePayloadToViewer(sessionId, json{{"type","remote_file_upload_started"},{"transfer_id",transferId},{"path",path}});
                } catch (const std::exception& ex) { SendFileError(sessionId, "remote_file_upload", path, ex.what()); }
            }

            void HandleRemoteFileUploadChunk(const std::string& sessionId, const json& msg) {
                const std::string transferId = msg.value("transfer_id", std::string());
                const std::string path = msg.value("path", std::string());
                const std::vector<unsigned char> bytes = Base64Decode(msg.value("data", std::string()));
                std::lock_guard<std::mutex> lock(fileUploadsMu_);
                auto it = fileUploads_.find(FileUploadKey(sessionId, transferId));
                if (it == fileUploads_.end() || !it->second || !it->second->stream) return;
                if (!bytes.empty()) it->second->stream->write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!*it->second->stream) {
                    fileUploads_.erase(it);
                    SendFileError(sessionId, "remote_file_upload", path, "Failed while writing upload chunk");
                    return;
                }
                it->second->received += static_cast<std::uint64_t>(bytes.size());
            }

            void HandleRemoteFileUploadComplete(const std::string& sessionId, const json& msg) {
                const std::string transferId = msg.value("transfer_id", std::string());
                const std::string path = msg.value("path", std::string());
                std::filesystem::path target;
                std::uint64_t received = 0;
                std::uint64_t expected = 0;
                {
                    std::lock_guard<std::mutex> lock(fileUploadsMu_);
                    auto it = fileUploads_.find(FileUploadKey(sessionId, transferId));
                    if (it == fileUploads_.end() || !it->second) { SendFileError(sessionId, "remote_file_upload", path, "Upload transfer was not found"); return; }
                    target = it->second->target; received = it->second->received; expected = it->second->expected;
                    if (it->second->stream) { it->second->stream->flush(); it->second->stream->close(); }
                    fileUploads_.erase(it);
                }
                if (expected && received != expected) {
                    SendFileError(sessionId, "remote_file_upload", path, "Upload size mismatch");
                    return;
                }
                SendFilePayloadToViewer(sessionId, json{{"type","remote_file_upload_complete"},{"transfer_id",transferId},{"path",path},{"size",received}});
                HandleRemoteFileListRequest(sessionId, target.parent_path().string());
            }

            void CancelFileUploadsForSession(const std::string& sessionId) {
                std::lock_guard<std::mutex> lock(fileUploadsMu_);
                const std::string prefix = sessionId + "|";
                for (auto it = fileUploads_.begin(); it != fileUploads_.end(); ) {
                    if (it->first.rfind(prefix, 0) == 0) it = fileUploads_.erase(it); else ++it;
                }
            }

            void HandleRemoteFileUploadRequest(const std::string& sessionId, const json& msg) {
                namespace fs = std::filesystem;
                const std::string path = msg.value("path", std::string());
                const std::string data = msg.value("data", std::string());
                try {
                    fs::path target = ResolveRemotePath(path);
                    if (target.empty()) {
                        SendFileError(sessionId, "remote_file_upload", path, "Invalid target path");
                        return;
                    }
                    fs::create_directories(target.parent_path());
                    std::vector<unsigned char> bytes = Base64Decode(data);
                    std::ofstream out(target, std::ios::binary | std::ios::trunc);
                    if (!out) {
                        SendFileError(sessionId, "remote_file_upload", path, "Could not open file for writing");
                        return;
                    }
                    if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    json payload = {
                        {"type", "remote_file_upload_complete"},
                        {"session_id", sessionId},
                        {"path", path},
                        {"size", static_cast<std::uint64_t>(bytes.size())}
                    };
                    SendFilePayloadToViewer(sessionId, payload);
                    HandleRemoteFileListRequest(sessionId, target.parent_path().string());
                }
                catch (const std::exception& ex) {
                    SendFileError(sessionId, "remote_file_upload", path, ex.what());
                }
            }

            void HandleRemoteFileDeleteRequest(const std::string& sessionId, const json& msg) {
                namespace fs = std::filesystem;
                const std::string path = msg.value("path", std::string());
                try {
                    fs::path target = ResolveRemotePath(path);
                    if (target.empty() || !fs::exists(target)) {
                        SendFileError(sessionId, "remote_file_delete", path, "Path does not exist");
                        return;
                    }
                    std::uintmax_t removed = fs::is_directory(target) ? fs::remove_all(target) : (fs::remove(target) ? 1 : 0);
                    json payload = {
                        {"type", "remote_file_delete_complete"},
                        {"session_id", sessionId},
                        {"path", path},
                        {"removed", static_cast<std::uint64_t>(removed)}
                    };
                    SendFilePayloadToViewer(sessionId, payload);
                    HandleRemoteFileListRequest(sessionId, target.parent_path().string());
                }
                catch (const std::exception& ex) {
                    SendFileError(sessionId, "remote_file_delete", path, ex.what());
                }
            }

            void HandleRemoteFileMkdirRequest(const std::string& sessionId, const json& msg) {
                namespace fs = std::filesystem;
                const std::string path = msg.value("path", std::string());
                try {
                    fs::path target = ResolveRemotePath(path);
                    if (target.empty()) {
                        SendFileError(sessionId, "remote_file_mkdir", path, "Invalid folder path");
                        return;
                    }
                    fs::create_directories(target);
                    json payload = {
                        {"type", "remote_file_mkdir_complete"},
                        {"session_id", sessionId},
                        {"path", path}
                    };
                    SendFilePayloadToViewer(sessionId, payload);
                    HandleRemoteFileListRequest(sessionId, target.parent_path().string());
                }
                catch (const std::exception& ex) {
                    SendFileError(sessionId, "remote_file_mkdir", path, ex.what());
                }
            }

            void HandleRemoteFileRenameRequest(const std::string& sessionId, const json& msg) {
                namespace fs = std::filesystem;
                const std::string from = msg.value("from", std::string());
                const std::string to = msg.value("to", std::string());
                try {
                    fs::path src = ResolveRemotePath(from);
                    fs::path dst = ResolveRemotePath(to);
                    if (src.empty() || dst.empty() || !fs::exists(src)) {
                        SendFileError(sessionId, "remote_file_rename", from, "Invalid source or target path");
                        return;
                    }
                    if (IsProtectedRemotePath(src) || IsProtectedRemotePath(dst)) {
                        SendFileError(sessionId, "remote_file_rename", from, "Protected/system path cannot be renamed");
                        return;
                    }
                    fs::rename(src, dst);
                    json payload = {
                        {"type", "remote_file_rename_complete"},
                        {"session_id", sessionId},
                        {"from", from},
                        {"to", to}
                    };
                    SendFilePayloadToViewer(sessionId, payload);
                    HandleRemoteFileListRequest(sessionId, dst.parent_path().string());
                }
                catch (const std::exception& ex) {
                    SendFileError(sessionId, "remote_file_rename", from, ex.what());
                }
            }


            void HandleSwitchMonitor(const std::string& sessionId, int requested) {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                auto it = sessions_.find(sessionId);
                if (it == sessions_.end()) {
                    return;
                }

                SessionContext& ctx = *it->second;
                ctx.displayIndex = requested;

                InputCmd cmd{};
                cmd.type = InputCmdType::SwitchMonitor;
                cmd.switchMonitor.monitorIndex = requested;

                ctx.normalInputPipe.Write(cmd);
                ctx.secureInputPipe.Write(cmd);

                LogI("switch_monitor relayed session=" + sessionId +
                    " requested=" + std::to_string(requested));
            }

            bool SendMediaControl(SessionContext& ctx, const json& message) {
                std::lock_guard<std::mutex> lock(ctx.mediaControlMu);
                return ctx.mediaControlPipe.SendLine(message.dump());
            }

            bool DispatchMediaFastMouse(SessionContext& ctx, double xNorm, double yNorm, uint64_t seq, double clientTsMs) {
                InputPipeWriter& targetPipe = ctx.secureFallbackOwnsInput.load(std::memory_order_acquire)
                    ? ctx.secureInputPipe
                    : (ctx.unifiedDesktopStreamer
                        ? ctx.normalInputPipe
                        : ((ctx.activeMode == DesktopMode::Secure) ? ctx.secureInputPipe : ctx.normalInputPipe));
                auto monitor = targetPipe.GetMonitorInfo(ctx.displayIndex);
                if (monitor.w <= 0 || monitor.h <= 0) monitor = ctx.normalInputPipe.GetMonitorInfo(ctx.displayIndex);
                if (monitor.w <= 0 || monitor.h <= 0) return false;
                const int32_t x = monitor.x + static_cast<int32_t>(xNorm * static_cast<double>(std::max(1, monitor.w - 1)));
                const int32_t y = monitor.y + static_cast<int32_t>(yNorm * static_cast<double>(std::max(1, monitor.h - 1)));
                return targetPipe.PublishFastMouseTarget(x, y, ctx.displayIndex, seq,
                    clientTsMs > 0.0 ? static_cast<uint64_t>(clientTsMs) : 0);
            }

            bool LaunchMediaHost(SessionContext& ctx,
                const std::vector<std::string>& iceServers,
                int width, int height, int fps, int bitrateKbps,
                const std::string& codecMode, bool enableAudio) {
                const std::string exePath = MediaHostExePath();
                std::string cmdLine =
                    "--session " + QuoteArg(ctx.sessionId) +
                    " --shmem " + QuoteArg(ctx.mediaShmemName) +
                    " --control-pipe " + QuoteArg(ctx.mediaControlPipeName) +
                    " --event-pipe " + QuoteArg(ctx.mediaEventPipeName) +
                    " --stop-event " + QuoteArg(ctx.mediaStopEventName) +
                    " --width " + std::to_string(width) +
                    " --height " + std::to_string(height) +
                    " --fps " + std::to_string(fps) +
                    " --bitrate " + std::to_string(bitrateKbps) +
                    " --codec " + QuoteArg(codecMode) +
                    " --audio " + std::string(enableAudio ? "1" : "0");
                for (const auto& ice : iceServers) cmdLine += " --ice-server " + QuoteArg(ice);

                ctx.mediaHostProcess = LaunchServiceChildProcess(exePath, cmdLine);
                if (!ctx.mediaHostProcess) {
                    LogE("launch media host FAILED session=" + ctx.sessionId + " err=" + std::to_string(GetLastError()));
                    return false;
                }
                AssignProcessToSessionJob(ctx.sessionJob, ctx.mediaHostProcess, ctx.sessionId, "media-host");
                if (!ctx.mediaControlPipe.Connect(ctx.mediaControlPipeName, 80, 125)) {
                    LogE("connect media control pipe FAILED session=" + ctx.sessionId);
                    return false;
                }
                for (int i = 0; i < 250 && !ctx.mediaReady.load(std::memory_order_acquire); ++i) {
                    if (WaitForSingleObject(ctx.mediaHostProcess, 0) == WAIT_OBJECT_0) break;
                    Sleep(20);
                }
                if (!ctx.mediaReady.load(std::memory_order_acquire)) {
                    LogE("media host did not become ready session=" + ctx.sessionId);
                    return false;
                }
                LogI("launch media host ok session=" + ctx.sessionId +
                    " pid=" + std::to_string(GetProcessId(ctx.mediaHostProcess)) +
                    " codec=" + codecMode);
                return true;
            }

            bool LaunchNormalStreamer(SessionContext& ctx) {
                const DWORD consoleSession = ctx.activeConsoleSessionId != 0xFFFFFFFF
                    ? ctx.activeConsoleSessionId
                    : ActiveConsoleSessionId();
                if (consoleSession == 0xFFFFFFFF) {
                    LogW("launch normal streamer skipped, no active console session=" + ctx.sessionId);
                    return false;
                }
                if (ctx.sessionMode == SessionMode::Console &&
                    (ctx.logoffLatched || ctx.loginDesktopMode || !InteractiveUserSessionReady(consoleSession))) {
                    LogI("launch normal streamer deferred while secure desktop owns console session=" + ctx.sessionId +
                        " console=" + SessionIdToString(consoleSession) +
                        " logoff_latched=" + std::string(ctx.logoffLatched ? "true" : "false"));
                    return false;
                }

                ctx.activeConsoleSessionId = consoleSession;
                ctx.lastSeenConsoleSessionId = consoleSession;

                const std::string exePath = RemoteHostExePath();

                ResetEvent(ctx.normalStopEvent);

                std::string cmdLine =
                    "--mode streamer"
                    " --session " + ctx.sessionId +
                    " --shmem " + ctx.normalShmemName +
                    " --input-pipe " + ctx.normalInputPipeName +
                    " --stop-event " + ctx.normalStopEventName +
                    " --chat-pipe " + ctx.chatPipeName +
                    " --fps " + std::to_string(ctx.fps) +
                    " --display " + std::to_string(ctx.displayIndex) +
                    (ctx.unifiedDesktopStreamer ? " --dynamic-desktop" : "") +
                    (ctx.disableGpuTransport ? " --disable-gpu-transport" : "");

                LogI("launch normal streamer [NORMAL_SHMEM_EXPECTED_NO_UAC] session=" + ctx.sessionId + " cmd=" + cmdLine);

                ctx.normalStreamerProcess = LaunchInElevatedDefaultSessionForSession(
                    std::string(exePath), cmdLine, consoleSession);
                if (!ctx.normalStreamerProcess) {
                    LogE("launch normal streamer FAILED session=" + ctx.sessionId);
                    return false;
                }

                ctx.normalStreamerSessionId = consoleSession;
                ctx.normalRetireRequestedAt = {};
                AssignProcessToSessionJob(ctx.sessionJob, ctx.normalStreamerProcess, ctx.sessionId, "normal-streamer");
                LogI("launch normal streamer ok session=" + ctx.sessionId +
                    " pid=" + std::to_string(GetProcessId(ctx.normalStreamerProcess)) +
                    " console=" + SessionIdToString(ctx.activeConsoleSessionId));

                SyncChatStateToContext(ctx);
                return true;
            }

            bool LaunchSecureStreamer(SessionContext& ctx) {
                if (ctx.secureStreamerProcess) {
                    return true;
                }

                const auto launchNow = std::chrono::steady_clock::now();
                if (SecureHostRestartBlocked(ctx, launchNow)) {
                    ctx.lastSecureLaunchAttempt = launchNow;
                    return false;
                }
                ctx.lastSecureLaunchAttempt = launchNow;

                const std::string exePath = RemoteHostExePath();

                if (!ctx.secureShmem.IsOpen()) {
                    if (!ctx.secureShmem.CreateProducer(ctx.secureShmemName)) {
                        LogE("create secure shmem FAILED session=" + ctx.sessionId +
                            " err=" + std::to_string(GetLastError()));
                        RecordSecureHostFailure(ctx, launchNow, "shared memory create failed");
                        return false;
                    }
                    LogI("secure shmem allocated on demand session=" + ctx.sessionId);
                }

                ResetEvent(ctx.secureStopEvent);

                std::string cmdLine =
                    "--mode streamer"
                    " --session " + ctx.sessionId +
                    " --shmem " + ctx.secureShmemName +
                    " --input-pipe " + ctx.secureInputPipeName +
                    " --stop-event " + ctx.secureStopEventName +
                    " --chat-pipe " + ctx.chatPipeName +
                    " --fps " + std::to_string(ctx.fps) +
                    " --display " + std::to_string(ctx.displayIndex);

                LogI("launch secure streamer session=" + ctx.sessionId + " cmd=" + cmdLine);

                DWORD secureSession = ctx.activeConsoleSessionId;
                if (secureSession == 0xFFFFFFFF) secureSession = ActiveConsoleSessionId();
                ctx.secureStreamerProcess = LaunchOnSecureDesktopForSession(
                    std::string(exePath), cmdLine, secureSession);
                if (!ctx.secureStreamerProcess) {
                    LogE("launch secure streamer FAILED session=" + ctx.sessionId);
                    RecordSecureHostFailure(ctx, launchNow, "process launch failed");
                    ctx.secureLaunchInProgress = false;
                    ctx.secureShmem.Close();
                    return false;
                }

                ctx.secureReady = false;
                ctx.secureLaunchInProgress = true;
                ctx.secureRetiring = false;
                ctx.secureStreamerSessionId = secureSession;
                ctx.secureRetireRequestedAt = {};

                AssignProcessToSessionJob(ctx.sessionJob, ctx.secureStreamerProcess, ctx.sessionId, "secure-streamer");
                LogI("launch secure streamer ok session=" + ctx.sessionId +
                    " pid=" + std::to_string(GetProcessId(ctx.secureStreamerProcess)));

                SyncChatStateToContext(ctx);
                return true;
            }


            bool LaunchBackstageHost(SessionContext& ctx) {
                if (ctx.backstageHostProcess) {
                    return true;
                }

                const std::string exePath = RemoteHostExePath();

                ResetEvent(ctx.normalStopEvent);

                int backgroundWidth = ctx.lastCaptureMonitorWidth > 0
                    ? ctx.lastCaptureMonitorWidth : ctx.displayGeometryAtStart.primaryWidth;
                int backgroundHeight = ctx.lastCaptureMonitorHeight > 0
                    ? ctx.lastCaptureMonitorHeight : ctx.displayGeometryAtStart.primaryHeight;
                if (backgroundWidth <= 0) backgroundWidth = 1280;
                if (backgroundHeight <= 0) backgroundHeight = 720;
                backgroundWidth = std::clamp(backgroundWidth, 800, 2560);
                backgroundHeight = std::clamp(backgroundHeight, 600, 1600);

                std::string cmdLine =
                    "--mode backstage-host"
                    " --session " + ctx.sessionId +
                    " --shmem " + ctx.normalShmemName +
                    " --input-pipe " + ctx.normalInputPipeName +
                    " --stop-event " + ctx.normalStopEventName +
                    " --fps 30"
                    " --width " + std::to_string(backgroundWidth) +
                    " --height " + std::to_string(backgroundHeight);

                std::string nativeBackground = ReadConfigString("HI5_BACKGROUND_NATIVE_DESKTOP", "1");
                std::transform(nativeBackground.begin(), nativeBackground.end(), nativeBackground.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                const bool nativeRequested = nativeBackground != "0" && nativeBackground != "false" && nativeBackground != "off";
                if (nativeRequested) cmdLine += " --native-desktop";

                LogI("launch backstage host session=" + ctx.sessionId +
                    " native_desktop=" + std::string(nativeRequested ? "true" : "false") +
                    " geometry=" + std::to_string(backgroundWidth) + "x" + std::to_string(backgroundHeight) +
                    " cmd=" + cmdLine);

                ctx.backstageHostProcess = LaunchInElevatedDefaultSession(std::string(exePath), cmdLine);
                if (!ctx.backstageHostProcess) {
                    LogE("launch backstage host FAILED session=" + ctx.sessionId);
                    return false;
                }

                ctx.backstageMode = true;
                ctx.activeMode = DesktopMode::Normal;
                ctx.lastNormalFrameAt = {};
                AssignProcessToSessionJob(ctx.sessionJob, ctx.backstageHostProcess, ctx.sessionId, "backstage-host");
                LogI("launch backstage host ok session=" + ctx.sessionId +
                    " pid=" + std::to_string(GetProcessId(ctx.backstageHostProcess)));
                return true;
            }

            void StopBackstageHost(SessionContext& ctx) {
                if (ctx.normalStopEvent) {
                    SetEvent(ctx.normalStopEvent);
                }

                if (ctx.backstageHostProcess) {
                    const DWORD waitRc = WaitForSingleObject(ctx.backstageHostProcess, 2500);
                    if (waitRc == WAIT_TIMEOUT) {
                        TerminateProcess(ctx.backstageHostProcess, 0);
                        WaitForSingleObject(ctx.backstageHostProcess, 1000);
                        LogW("forced backstage host termination session=" + ctx.sessionId);
                    }
                    CloseHandle(ctx.backstageHostProcess);
                    ctx.backstageHostProcess = nullptr;
                }
                ctx.backstageMode = false;
                if (ctx.normalStopEvent) {
                    ResetEvent(ctx.normalStopEvent);
                }
            }

            void HandleBackstageStart(const std::string& sessionId) {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                auto it = sessions_.find(sessionId);
                if (it == sessions_.end()) {
                    LogW("backstage_start ignored, session not found=" + sessionId);
                    return;
                }
                SessionContext& ctx = *it->second;
                if (ctx.sessionMode == SessionMode::Console) {
                    LogW("backstage_start rejected for immutable console session=" + sessionId);
                    SendSessionStateDirect(ctx, "console_ready");
                    return;
                }
                if (ctx.backstageMode && ctx.backstageHostProcess) {
                    SendSessionStateDirect(ctx, "backstage_ready");
                    return;
                }

                SendSessionStateDirect(ctx, "backstage_entering");
                ctx.backstageMode = true;
                StopNormalStreamer(ctx);
                StopSecureStreamer(ctx);
                ctx.uacRequested = false;
                ctx.secureStateAnnounced = false;
                ctx.handoffStateAnnounced = false;

                if (LaunchBackstageHost(ctx)) {
                    SendSessionStateDirect(ctx, "backstage_ready");
                    return;
                }

                LogE("backstage_start failed session=" + sessionId);
                SendSessionStateDirect(ctx, "backstage_failed");
                if (ctx.sessionMode == SessionMode::Console) {
                    ctx.backstageMode = false;
                    if (LaunchNormalStreamer(ctx)) {
                        SendSessionStateDirect(ctx, "console_ready");
                    }
                }
            }

            void HandleBackstageStop(const std::string& sessionId) {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                auto it = sessions_.find(sessionId);
                if (it == sessions_.end()) {
                    LogW("backstage_stop ignored, session not found=" + sessionId);
                    return;
                }
                SessionContext& ctx = *it->second;
                if (ctx.sessionMode == SessionMode::Backstage) {
                    LogW("HandleBackstageStop ignored for locked backstage session=" + sessionId);
                    SendSessionStateDirect(ctx, "backstage_ready");
                    return;
                }
                if (!ctx.backstageMode && !ctx.backstageHostProcess) {
                    SendSessionStateDirect(ctx, "console_ready");
                    return;
                }

                SendSessionStateDirect(ctx, "console_entering");
                if (ctx.backstageHostProcess) {
                    StopBackstageHost(ctx);
                }
                ctx.backstageMode = false;
                ctx.activeConsoleSessionId = ActiveConsoleSessionId();
                ctx.interactiveUserReady = InteractiveUserSessionReady(ctx.activeConsoleSessionId);
                ctx.loginDesktopMode = ctx.activeConsoleSessionId != 0xFFFFFFFF && !ctx.interactiveUserReady;
                ctx.normalDesktopUnavailable = ctx.loginDesktopMode;

                const bool launched = ctx.loginDesktopMode ? LaunchSecureStreamer(ctx) : LaunchNormalStreamer(ctx);
                SendSessionStateDirect(ctx, launched ? "console_ready" : "console_failed");
            }

            void StopSecureStreamer(SessionContext& ctx) {
                if (ctx.secureStopEvent) {
                    SetEvent(ctx.secureStopEvent);
                }

                if (ctx.secureStreamerProcess) {
                    const DWORD waitRc = WaitForSingleObject(ctx.secureStreamerProcess, 3000);
                    if (waitRc == WAIT_TIMEOUT) {
                        TerminateProcess(ctx.secureStreamerProcess, 0);
                        WaitForSingleObject(ctx.secureStreamerProcess, 1000);
                        LogW("forced secure streamer termination session=" + ctx.sessionId);
                    }
                    CloseHandle(ctx.secureStreamerProcess);
                    ctx.secureStreamerProcess = nullptr;
                }

                ctx.secureStreamerSessionId = 0xFFFFFFFF;
                ctx.secureRetireRequestedAt = {};
                ctx.secureRetiring = false;
                ctx.secureReady = false;
                ctx.secureLaunchInProgress = false;
                ctx.secureShmem.Close();
            }

            void StopNormalStreamer(SessionContext& ctx) {
                if (ctx.normalStopEvent) {
                    SetEvent(ctx.normalStopEvent);
                }

                if (ctx.normalStreamerProcess) {
                    const DWORD waitRc = WaitForSingleObject(ctx.normalStreamerProcess, 2500);
                    if (waitRc == WAIT_TIMEOUT) {
                        TerminateProcess(ctx.normalStreamerProcess, 0);
                        WaitForSingleObject(ctx.normalStreamerProcess, 1000);
                        LogW("forced normal streamer termination session=" + ctx.sessionId);
                    }
                    CloseHandle(ctx.normalStreamerProcess);
                    ctx.normalStreamerProcess = nullptr;
                }

                ctx.normalStreamerSessionId = 0xFFFFFFFF;
                ctx.normalRetireRequestedAt = {};
                ctx.lastNormalFrameAt = {};
            }

            void StartStreamerSession(const std::string& sessionId,
                const std::vector<std::string>& iceServers,
                const std::function<void(const std::string&)>& sendFn,
                int width,
                int height,
                int fps,
                int bitrateKbps,
                SessionMode sessionMode,
                const std::string& technicianName) {
                StopSession(sessionId);

                auto ctx = std::make_unique<SessionContext>();
                ctx->sessionId = sessionId;
                ctx->iceServers = iceServers;
                ctx->technicianName = technicianName.empty() ? "Technician" : technicianName;
                ctx->displayIndex = 0;
                ctx->fps = fps;
                ctx->activeMode = DesktopMode::Normal;
                ctx->sessionMode = sessionMode;
                ctx->backstageMode = sessionMode == SessionMode::Backstage;
                ctx->uacRequested = false;
                {
                    std::string dynamicDesktop = ReadConfigString("HI5_DYNAMIC_DESKTOP", "1");
                    std::transform(dynamicDesktop.begin(), dynamicDesktop.end(), dynamicDesktop.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    ctx->unifiedDesktopStreamer = dynamicDesktop != "0" && dynamicDesktop != "false" && dynamicDesktop != "off";

                    // The cross-process shared-D3D11 path is still experimental. A failed
                    // handle/synchronisation path produces a connected WebRTC session with
                    // no normal-desktop picture, while secure/CAD keeps working because it
                    // uses raw I420. Keep the known-good raw path as the production default
                    // and allow GPU transport to be explicitly re-enabled for targeted tests.
                    std::string gpuTransport = ReadConfigString("HI5_GPU_FRAME_TRANSPORT", "0");
                    std::transform(gpuTransport.begin(), gpuTransport.end(), gpuTransport.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    const bool enableGpuTransport =
                        gpuTransport == "1" || gpuTransport == "true" ||
                        gpuTransport == "on" || gpuTransport == "yes";
                    ctx->disableGpuTransport = !enableGpuTransport;
                    LogI("desktop frame transport session=" + sessionId +
                        " gpu=" + std::string(enableGpuTransport ? "enabled" : "disabled") +
                        " default=raw-i420");
                }
                ctx->unifiedSecureReady = false;
                ctx->secureReady = false;
                ctx->secureLaunchInProgress = false;
                ctx->activeConsoleSessionId = ActiveConsoleSessionId();
                ctx->lastSeenConsoleSessionId = ctx->activeConsoleSessionId;
                ctx->pendingConsoleSessionId = ctx->activeConsoleSessionId;
                ctx->bannerConsoleSessionId = ctx->activeConsoleSessionId;
                ctx->interactiveUserReady = sessionMode == SessionMode::Console &&
                    InteractiveUserSessionReady(ctx->activeConsoleSessionId);
                ctx->loginDesktopMode = sessionMode == SessionMode::Console &&
                    ctx->activeConsoleSessionId != 0xFFFFFFFF && !ctx->interactiveUserReady;
                ctx->normalDesktopUnavailable = ctx->loginDesktopMode;
                ctx->logoffLatched = ctx->loginDesktopMode;
                ctx->lastSessionChangeSeq = g_sessionChangeSeq.load(std::memory_order_acquire);
                ctx->consoleHandoffActive = ctx->loginDesktopMode;
                ctx->bannerRebindPending = ctx->loginDesktopMode;
                if (ctx->loginDesktopMode) {
                    ctx->activeMode = DesktopMode::Secure;
                    ctx->secureFallbackOwnsInput.store(true, std::memory_order_release);
                }
                ctx->lastConsoleSessionPoll = std::chrono::steady_clock::now();
                ctx->displayGeometryAtStart = ReadDisplayGeometrySnapshot();
                ctx->lastDisplayGeometry = ctx->displayGeometryAtStart;
                LogDisplayGeometry(sessionId, "session_start", ctx->displayGeometryAtStart);

                const std::string prefix = sessionId.substr(0, std::min<size_t>(16, sessionId.size()));

                ctx->normalShmemName = "Global\\Hi5Stream_" + prefix;
                ctx->secureShmemName = "Global\\Hi5Stream_UAC_" + prefix;
                ctx->normalInputPipeName = "Global\\Hi5Input_" + prefix;
                ctx->secureInputPipeName = "Global\\Hi5Input_UAC_" + prefix;
                ctx->normalStopEventName = "Global\\Hi5Stop_" + prefix;
                ctx->secureStopEventName = "Global\\Hi5Stop_UAC_" + prefix;
                ctx->mediaShmemName = "Global\\Hi5Media_" + prefix;
                ctx->mediaControlPipeName = "\\\\.\\pipe\\Hi5MediaCtrl_" + prefix;
                ctx->mediaEventPipeName = "\\\\.\\pipe\\Hi5MediaEvt_" + prefix;
                ctx->mediaStopEventName = "Global\\Hi5MediaStop_" + prefix;
                ctx->chatPipeName = "\\\\.\\pipe\\Hi5Chat_" + prefix;

                LogI("initial console session session=" + sessionId +
                    " console=" + SessionIdToString(ctx->activeConsoleSessionId) +
                    " interactive_user=" + std::string(ctx->interactiveUserReady ? "true" : "false") +
                    " login_desktop=" + std::string(ctx->loginDesktopMode ? "true" : "false"));
                LogI("chat pipe name session=" + sessionId + " pipe=" + ctx->chatPipeName);

                if (!ctx->normalShmem.CreateProducer(ctx->normalShmemName)) {
                    LogE("create normal shmem FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                    return;
                }
                if (!ctx->mediaShmem.CreateProducer(ctx->mediaShmemName)) {
                    LogE("create media shmem FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                    ctx->normalShmem.Close();
                    return;
                }

                // Secure-desktop frame memory is allocated lazily only when a
                // fallback secure helper is actually needed. Most sessions never
                // need it, so avoid carrying another 32 MB mapping for the entire
                // session.

                if (!ctx->normalInputPipe.Create(ctx->normalInputPipeName)) {
                    LogW("create normal input pipe FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                }

                if (!ctx->secureInputPipe.Create(ctx->secureInputPipeName)) {
                    LogW("create secure input pipe FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                }

                ctx->normalStopEvent = CreateEventA(nullptr, TRUE, FALSE, ctx->normalStopEventName.c_str());
                if (!ctx->normalStopEvent) {
                    LogE("create normal stop event FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                    ctx->secureInputPipe.Close();
                    ctx->normalInputPipe.Close();
                    ctx->secureShmem.Close();
                    ctx->mediaShmem.Close();
                    ctx->normalShmem.Close();
                    return;
                }

                ctx->secureStopEvent = CreateEventA(nullptr, TRUE, FALSE, ctx->secureStopEventName.c_str());
                if (!ctx->secureStopEvent) {
                    LogE("create secure stop event FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                    CloseHandle(ctx->normalStopEvent);
                    ctx->normalStopEvent = nullptr;
                    ctx->secureInputPipe.Close();
                    ctx->normalInputPipe.Close();
                    ctx->secureShmem.Close();
                    ctx->mediaShmem.Close();
                    ctx->normalShmem.Close();
                    return;
                }

                ctx->sessionJob = CreateSessionJobObject(sessionId);
                if (!ctx->sessionJob) {
                    LogW("session will use fallback cleanup because Job Object creation failed session=" + sessionId);
                }

                ctx->chatPipeServer = std::make_unique<NamedPipeServer>();
                ctx->chatPipeServer->Start(ctx->chatPipeName, [this](const std::string& raw) {
                    const auto msg = json::parse(raw, nullptr, false);
                    if (msg.is_discarded()) return;
                    const std::string type = msg.value("type", "");
                    if (type == "chat_message") {
                        const std::string body = ChatBodyFromJson(msg);
                        const std::string sender = msg.value("sender", std::string("user"));
                        const std::string displayName = msg.value("display_name", std::string("Remote user"));
                        const std::string sid = msg.value("session_id", sessionBridge_.GetActiveSession());
                        AppendChatTranscript(sid, sender, displayName, body);

                        json outbound = {
                            {"type", "chat_message"},
                            {"session_id", sid},
                            {"sender", sender},
                            {"display_name", displayName},
                            {"body", body},
                            {"unix_ms", NowUnixMs()}
                        };
                        LogI("chat message from remote user session=" + sid + " bytes=" + std::to_string(body.size()));
                        if (signaling_) {
                            signaling_->send(outbound.dump());
                        }
                    }
                    });

                ctx->mediaStopEvent = CreateEventA(nullptr, TRUE, FALSE, ctx->mediaStopEventName.c_str());
                if (!ctx->mediaStopEvent) {
                    LogE("create media stop event FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                    if (ctx->chatPipeServer) {
                        ctx->chatPipeServer->Stop();
                        ctx->chatPipeServer.reset();
                    }
                    if (ctx->sessionJob) {
                        CloseHandle(ctx->sessionJob);
                        ctx->sessionJob = nullptr;
                    }
                    if (ctx->secureStopEvent) CloseHandle(ctx->secureStopEvent);
                    if (ctx->normalStopEvent) CloseHandle(ctx->normalStopEvent);
                    ctx->secureStopEvent = nullptr;
                    ctx->normalStopEvent = nullptr;
                    ctx->secureInputPipe.Close();
                    ctx->normalInputPipe.Close();
                    ctx->secureShmem.Close();
                    ctx->mediaShmem.Close();
                    ctx->normalShmem.Close();
                    return;
                }

                std::string requestedCodec = ReadConfigString("HI5_CODEC", "auto");
                std::transform(requestedCodec.begin(), requestedCodec.end(), requestedCodec.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (requestedCodec != "av1_hw" && requestedCodec != "av1" && requestedCodec != "av1_sw" &&
                    requestedCodec != "vp9_hw" && requestedCodec != "vp9" && requestedCodec != "vp9_sw" &&
                    requestedCodec != "h265_hw" && requestedCodec != "h265" && requestedCodec != "h265_sw" &&
                    requestedCodec != "h264_hw" && requestedCodec != "h264" && requestedCodec != "h264_sw" &&
                    requestedCodec != "vp8") {
                    requestedCodec = "auto";
                }

                SessionContext* rawCtx = ctx.get();
                ctx->mediaEventPipeServer = std::make_unique<NamedPipeServer>();
                ctx->mediaEventPipeServer->Start(ctx->mediaEventPipeName,
                    [this, rawCtx, sendFn, sid = sessionId](const std::string& raw) {
                        const auto message = json::parse(raw, nullptr, false);
                        if (message.is_discarded()) return;
                        const std::string type = message.value("type", std::string());
                        if (type == "ready") {
                            rawCtx->mediaReady.store(true, std::memory_order_release);
                            return;
                        }
                        if (type == "signal") {
                            const std::string payload = message.value("payload", std::string());
                            if (!payload.empty()) sendFn(payload);
                            return;
                        }
                        if (type == "input_event" && message.contains("payload") && message["payload"].is_object()) {
                            DispatchInputToPipe(*rawCtx, message["payload"]);
                            return;
                        }
                        if (type == "gpu_transport_failed") {
                            const std::string reason = message.value("reason", std::string("unknown"));
                            LogW("GPU transport failed in media host session=" + sid + " reason=" + reason +
                                "; requesting raw-I420 fallback");
                            rawCtx->gpuTransportFallbackRequested.store(true, std::memory_order_release);
                            return;
                        }
                        if (type == "mouse_move") {
                            DispatchMediaFastMouse(*rawCtx,
                                message.value("x_norm", 0.0), message.value("y_norm", 0.0),
                                message.value("seq", static_cast<uint64_t>(0)), message.value("client_ts", 0.0));
                            return;
                        }
                        if (type == "closed") {
                            const std::string reason = message.value("reason", std::string("peer_connection_closed_or_failed"));
                            LogW("WebRTC media host closed session=" + sid + " reason=" + reason + "; scheduling cleanup");
                            std::thread([this, sid]() {
                                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                                StopSession(sid);
                                CleanupOrphanStreamerProcesses(sid);
                            }).detach();
                        }
                    });

                if (!LaunchMediaHost(*ctx, iceServers, width, height, fps, bitrateKbps,
                    requestedCodec, sessionMode == SessionMode::Console)) {
                    LogE("media host startup failed session=" + sessionId);
                    if (ctx->mediaStopEvent) SetEvent(ctx->mediaStopEvent);
                    ctx->mediaControlPipe.Close();
                    if (ctx->mediaEventPipeServer) {
                        ctx->mediaEventPipeServer->Stop();
                        ctx->mediaEventPipeServer.reset();
                    }
                    if (ctx->mediaHostProcess) {
                        WaitForSingleObject(ctx->mediaHostProcess, 1500);
                        CloseHandle(ctx->mediaHostProcess);
                        ctx->mediaHostProcess = nullptr;
                    }
                    if (ctx->sessionJob) {
                        CloseHandle(ctx->sessionJob);
                        ctx->sessionJob = nullptr;
                    }
                    if (ctx->mediaStopEvent) CloseHandle(ctx->mediaStopEvent);
                    if (ctx->secureStopEvent) CloseHandle(ctx->secureStopEvent);
                    if (ctx->normalStopEvent) CloseHandle(ctx->normalStopEvent);
                    ctx->mediaStopEvent = nullptr;
                    ctx->secureStopEvent = nullptr;
                    ctx->normalStopEvent = nullptr;
                    ctx->secureInputPipe.Close();
                    ctx->normalInputPipe.Close();
                    ctx->secureShmem.Close();
                    ctx->mediaShmem.Close();
                    ctx->normalShmem.Close();
                    return;
                }

                const bool launched = (sessionMode == SessionMode::Backstage)
                    ? LaunchBackstageHost(*ctx)
                    : (ctx->loginDesktopMode ? LaunchSecureStreamer(*ctx) : LaunchNormalStreamer(*ctx));

                if (!launched) {
                    StopPresenceBanner(sessionId);
                    StopChatOverlay(sessionId);
                    if (ctx->mediaStopEvent) SetEvent(ctx->mediaStopEvent);
                    ctx->mediaControlPipe.Close();
                    if (ctx->mediaEventPipeServer) {
                        ctx->mediaEventPipeServer->Stop();
                        ctx->mediaEventPipeServer.reset();
                    }
                    if (ctx->mediaHostProcess) {
                        WaitForSingleObject(ctx->mediaHostProcess, 1500);
                        CloseHandle(ctx->mediaHostProcess);
                        ctx->mediaHostProcess = nullptr;
                    }
                    if (ctx->mediaStopEvent) {
                        CloseHandle(ctx->mediaStopEvent);
                        ctx->mediaStopEvent = nullptr;
                    }
                    CloseHandle(ctx->secureStopEvent);
                    CloseHandle(ctx->normalStopEvent);
                    ctx->secureStopEvent = nullptr;
                    ctx->normalStopEvent = nullptr;
                    ctx->secureInputPipe.Close();
                    ctx->normalInputPipe.Close();
                    ctx->secureShmem.Close();
                    ctx->mediaShmem.Close();
                    ctx->normalShmem.Close();
                    if (ctx->sessionJob) {
                        CloseHandle(ctx->sessionJob);
                        ctx->sessionJob = nullptr;
                    }
                    return;
                }

                ctx->framePumpThread = std::thread([this, raw = ctx.get()]() {
                    FramePumpLoop(*raw);
                    });

                {
                    std::lock_guard<std::mutex> lock(sessionsMu_);
                    sessions_[sessionId] = std::move(ctx);
                }

                if (!SendMediaControl(*rawCtx, json{ {"type", "start"} })) {
                    LogW("failed to send media start command session=" + sessionId);
                }
                LogI("session fully registered session=" + sessionId);
            }

            void FramePumpLoop(SessionContext& ctx) {
                LogI("frame pump start session=" + ctx.sessionId);

                auto nextMonitorPoll = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                auto nextSecurePoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
                uint64_t framesForwarded = 0;
                uint64_t lastNormalStatsSeq = 0;
                uint64_t lastSecureStatsSeq = 0;
                auto nextDiagnosticsPoll = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                auto nextMemoryDiagnostics = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                const uint64_t maxFrameAgeNs = static_cast<uint64_t>(ReadConfigInt("HI5_MAX_FRAME_AGE_MS", 250, 50, 2000)) * 1000000ull;
                uint64_t staleFramesDropped = 0;
                auto nextStaleFrameLog = std::chrono::steady_clock::now();
                ctx.lastNormalLaunchAttempt = std::chrono::steady_clock::now();
                ctx.lastSecureLaunchAttempt = {};
                ctx.lastConsoleSessionPoll = std::chrono::steady_clock::now();

                // Keep raw I420 buffers for the lifetime of the session frame pump.
                // Recreating several multi-megabyte vector sets on every poll caused
                // Windows' heap/working set to grow far beyond the live frame data.
                I420Frame normalFrame;
                I420Frame secureFrame;
                SharedGpuFrame normalGpuFrame;
                uint64_t normalTs = 0;
                uint64_t secureTs = 0;

                auto duplicateGpuHandleForMedia = [&](const SharedGpuFrame& source, SharedGpuFrame& target) -> bool {
                    target = source;
                    if (!ctx.normalStreamerProcess || !ctx.mediaHostProcess || source.sharedHandle == 0) return false;
                    const DWORD sourcePid = GetProcessId(ctx.normalStreamerProcess);
                    if (sourcePid == 0) return false;
                    if (ctx.gpuHandleSourcePid != sourcePid) {
                        ctx.gpuHandleMap.clear();
                        ctx.gpuHandleSourcePid = sourcePid;
                    }
                    auto it = ctx.gpuHandleMap.find(source.sharedHandle);
                    if (it == ctx.gpuHandleMap.end()) {
                        HANDLE duplicated = nullptr;
                        const HANDLE sourceHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(source.sharedHandle));
                        if (!DuplicateHandle(ctx.normalStreamerProcess, sourceHandle, ctx.mediaHostProcess,
                            &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS) || !duplicated) {
                            LogW("[gpu-transport] DuplicateHandle failed session=" + ctx.sessionId +
                                " source_pid=" + std::to_string(sourcePid) +
                                " media_pid=" + std::to_string(GetProcessId(ctx.mediaHostProcess)) +
                                " err=" + std::to_string(GetLastError()));
                            return false;
                        }
                        const uint64_t mediaHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(duplicated));
                        it = ctx.gpuHandleMap.emplace(source.sharedHandle, mediaHandle).first;
                        LogI("[gpu-transport] duplicated NT handle session=" + ctx.sessionId +
                            " source_pid=" + std::to_string(sourcePid) +
                            " media_pid=" + std::to_string(GetProcessId(ctx.mediaHostProcess)) +
                            " source_handle=" + std::to_string(source.sharedHandle) +
                            " media_handle=" + std::to_string(mediaHandle));
                    }
                    target.sharedHandle = it->second;
                    return true;
                };

                const auto isNearBlackTransitionFrame = [](const I420Frame& frame) -> bool {
                    if (frame.y.empty()) return false;
                    const size_t step = std::max<size_t>(1, frame.y.size() / 4096);
                    uint64_t sum = 0;
                    uint8_t maxY = 0;
                    size_t count = 0;
                    for (size_t i = 0; i < frame.y.size(); i += step) {
                        const uint8_t y = frame.y[i];
                        sum += y;
                        maxY = std::max(maxY, y);
                        ++count;
                    }
                    if (count == 0) return false;
                    const double avgY = static_cast<double>(sum) / static_cast<double>(count);
                    return maxY <= 30 && avgY <= 21.0;
                };

                while (!ctx.framePumpStop.load()) {
                    const auto now = std::chrono::steady_clock::now();

                    if (ctx.gpuTransportFallbackRequested.exchange(false, std::memory_order_acq_rel) &&
                        !ctx.disableGpuTransport) {
                        ctx.disableGpuTransport = true;
                        ctx.gpuHandleMap.clear();
                        ctx.gpuHandleSourcePid = 0;
                        LogW("[gpu-transport] switching normal streamer to raw-I420 fallback session=" + ctx.sessionId);
                        if (ctx.normalStreamerProcess && ctx.normalStopEvent) SetEvent(ctx.normalStopEvent);
                    }

                    if (ConsumePresenceBannerAction(ctx.sessionId, true)) {
                        LogI("[presence] local user opened chat session=" + ctx.sessionId);
                        ChatOverlayState* overlay = EnsureChatOverlay(ctx.sessionId, ctx.sessionJob,
                            [this](const std::string& sid, const std::string& replyBody) {
                                AppendChatTranscript(sid, "user", "Remote user", replyBody);
                                if (signaling_) {
                                    json out = {
                                        {"type", "chat_message"},
                                        {"session_id", sid},
                                        {"sender", "user"},
                                        {"display_name", "Remote user"},
                                        {"body", replyBody},
                                        {"unix_ms", NowUnixMs()}
                                    };
                                    signaling_->send(out.dump());
                                }
                            });
                        if (overlay) {
                            {
                                std::lock_guard<std::mutex> lock(overlay->mu);
                                overlay->userDismissed = false;
                                overlay->pendingToOverlay.push_back(json{
                                    {"type", "show"}, {"session_id", ctx.sessionId}
                                }.dump());
                            }
                            FlushQueuedChatToOverlay(overlay);
                        }
                    }

                    if (ConsumePresenceBannerAction(ctx.sessionId, false)) {
                        LogI("[presence] local user ending remote session=" + ctx.sessionId);
                        const std::string sid = ctx.sessionId;
                        if (signaling_) {
                            json ended = {
                                {"type", "viewer_disconnected"},
                                {"session_id", sid},
                                {"reason", "user_ended_session"},
                                {"ended_by", "user"}
                            };
                            signaling_->send(ended.dump());
                        }
                        std::thread([this, sid]() { StopSession(sid); }).detach();
                        return;
                    }

                    // Prefer authoritative SCM/WTS session lifecycle events over polling.
                    // A LOGOFF event latches the stream onto Winlogon immediately, even if
                    // WTSQueryUserToken still temporarily succeeds for the dying desktop.
                    const uint64_t sessionChangeSeq = g_sessionChangeSeq.load(std::memory_order_acquire);
                    if (sessionChangeSeq != ctx.lastSessionChangeSeq) {
                        ctx.lastSessionChangeSeq = sessionChangeSeq;
                        const DWORD eventType = g_sessionChangeType.load(std::memory_order_acquire);
                        const DWORD eventSession = g_sessionChangeSessionId.load(std::memory_order_acquire);
                        const bool relevantSession = eventSession == ctx.activeConsoleSessionId ||
                            eventSession == ctx.normalStreamerSessionId ||
                            eventSession == ctx.secureStreamerSessionId ||
                            ctx.activeConsoleSessionId == 0xFFFFFFFF;

                        if (ctx.sessionMode == SessionMode::Console && relevantSession && eventType == WTS_SESSION_LOGOFF) {
                            ctx.logoffLatched = true;
                            ctx.loginDesktopMode = true;
                            ctx.normalDesktopUnavailable = true;
                            ctx.normalRecoveryArmed = false;
                            ctx.interactiveUserReady = false;
                            ctx.consoleSwitchInProgress = false;
                            ctx.consoleHandoffActive = true;
                            ctx.bannerRebindPending = true;
                            ctx.uacRequested = false;
                            ctx.unifiedSecureReady = false;
                            ctx.secureReady = false;
                            ctx.consoleNormalReadyAfterTickNs = 0;
                            ctx.secureFallbackOwnsInput.store(true, std::memory_order_release);
                            ++ctx.consoleGeneration;

                            if (!ctx.handoffStateAnnounced) {
                                SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                ctx.handoffStateAnnounced = true;
                            }
                            if (ctx.normalStreamerProcess && ctx.normalStopEvent) {
                                SetEvent(ctx.normalStopEvent);
                                ctx.normalRetireRequestedAt = now;
                            }
                            ctx.lastSecureLaunchAttempt = now - std::chrono::seconds(1);
                            LogI("[session-handoff] SCM logoff latched session=" + ctx.sessionId +
                                " windows_session=" + SessionIdToString(eventSession));
                        }
                        else if (ctx.sessionMode == SessionMode::Console && eventType == WTS_SESSION_LOGON) {
                            // Release the logoff latch only on an actual Windows logon event.
                            // The secure feed remains visible until the normal worker produces a
                            // fresh frame, so this event never causes an intermediate black screen.
                            ctx.logoffLatched = false;
                            ctx.normalRecoveryArmed = true;
                            ctx.bannerRebindPending = true;
                            ctx.lastNormalLaunchAttempt = now - std::chrono::seconds(3);
                            LogI("[session-handoff] SCM logon observed session=" + ctx.sessionId +
                                " windows_session=" + SessionIdToString(eventSession));
                        }
                    }

                    // Console logoff/login is a transport-worker migration, not a WebRTC
                    // session restart. Keep the peer connection and last good video frame alive
                    // while Winlogon and the next interactive session come and go.
                    if (now - ctx.lastConsoleSessionPoll >= std::chrono::milliseconds(250)) {
                        ctx.lastConsoleSessionPoll = now;
                        const DWORD currentConsole = ActiveConsoleSessionId();

                        if (currentConsole != ctx.lastSeenConsoleSessionId) {
                            LogW("[session-handoff] console change session=" + ctx.sessionId +
                                " old_seen=" + SessionIdToString(ctx.lastSeenConsoleSessionId) +
                                " new_seen=" + SessionIdToString(currentConsole));
                            ctx.lastSeenConsoleSessionId = currentConsole;
                            ctx.pendingConsoleSessionId = currentConsole;
                            ctx.lastConsoleSwitchDetected = now;
                            ctx.consoleSwitchInProgress = true;
                            ctx.consoleHandoffActive = true;
                            ctx.bannerRebindPending = true;
                            ++ctx.consoleGeneration;

                            if (!ctx.handoffStateAnnounced) {
                                SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                ctx.handoffStateAnnounced = true;
                            }
                            LogI("[session-handoff] entering generation=" + std::to_string(ctx.consoleGeneration) +
                                " session=" + ctx.sessionId);
                        }

                        // A short debounce is enough to avoid the transient no-console value.
                        // The old 1.5 second hold made Winlogon visibly black.
                        if (ctx.consoleSwitchInProgress &&
                            now - ctx.lastConsoleSwitchDetected >= std::chrono::milliseconds(150) &&
                            currentConsole == ctx.pendingConsoleSessionId) {
                            ctx.consoleSwitchInProgress = false;

                            if (currentConsole != 0xFFFFFFFF && currentConsole != ctx.activeConsoleSessionId) {
                                LogW("[session-handoff] target console ready session=" + ctx.sessionId +
                                    " old=" + SessionIdToString(ctx.activeConsoleSessionId) +
                                    " new=" + SessionIdToString(currentConsole));
                                const DWORD previousConsole = ctx.activeConsoleSessionId;
                                ctx.activeConsoleSessionId = currentConsole;
                                ctx.consoleNormalReadyAfterTickNs = static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                                ctx.uacRequested = false;
                                ctx.unifiedSecureReady = false;
                                ctx.secureReady = false;
                                ctx.normalInputPipe.ResetConsumerState();
                                ctx.secureInputPipe.ResetConsumerState();

                                // Retire helpers bound to the old Windows session without blocking
                                // the WebRTC/frame-pump thread. The secure bridge or last good
                                // frame remains visible while their replacements warm up.
                                if (ctx.normalStreamerProcess && ctx.normalStreamerSessionId != currentConsole) {
                                    if (ctx.normalStopEvent) SetEvent(ctx.normalStopEvent);
                                    ctx.normalRetireRequestedAt = now;
                                    LogI("[session-handoff] retire old normal helper session=" + ctx.sessionId +
                                        " worker_console=" + SessionIdToString(ctx.normalStreamerSessionId) +
                                        " target_console=" + SessionIdToString(currentConsole));
                                }
                                if (ctx.secureStreamerProcess && ctx.secureStreamerSessionId != currentConsole) {
                                    if (ctx.secureStopEvent) SetEvent(ctx.secureStopEvent);
                                    ctx.secureRetiring = true;
                                    ctx.secureRetireRequestedAt = now;
                                    LogI("[session-handoff] retire old secure helper session=" + ctx.sessionId +
                                        " worker_console=" + SessionIdToString(ctx.secureStreamerSessionId) +
                                        " target_console=" + SessionIdToString(currentConsole));
                                }

                                ctx.lastNormalLaunchAttempt = now - std::chrono::seconds(3);
                                ctx.lastSecureLaunchAttempt = now - std::chrono::seconds(1);
                                LogI("[session-handoff] migration armed session=" + ctx.sessionId +
                                    " from=" + SessionIdToString(previousConsole) +
                                    " to=" + SessionIdToString(currentConsole));
                            }
                        }

                        // A console id alone is not proof that winsta0\default exists.
                        // At Winlogon, WTSGetActiveConsoleSessionId returns a real id while
                        // WTSQueryUserToken returns ERROR_NO_TOKEN. Treat that as a stable
                        // login-screen stream owned exclusively by the secure helper.
                        if (ctx.sessionMode == SessionMode::Console &&
                            currentConsole != 0xFFFFFFFF &&
                            currentConsole == ctx.activeConsoleSessionId) {
                            const bool interactiveReady = !ctx.logoffLatched && InteractiveUserSessionReady(currentConsole);
                            if (interactiveReady != ctx.interactiveUserReady) {
                                LogI("[session-handoff] interactive user state session=" + ctx.sessionId +
                                    " console=" + SessionIdToString(currentConsole) +
                                    " ready=" + std::string(interactiveReady ? "true" : "false"));
                                ctx.interactiveUserReady = interactiveReady;

                                if (!interactiveReady) {
                                    const bool newHandoff = !ctx.consoleHandoffActive;
                                    ctx.loginDesktopMode = true;
                                    ctx.normalDesktopUnavailable = true;
                                    ctx.consoleHandoffActive = true;
                                    ctx.bannerRebindPending = true;
                                    ctx.uacRequested = false;
                                    ctx.unifiedSecureReady = false;
                                    // Re-prime Winlogon even when the secure helper already exists.
                                    // This forces a clean keyframe instead of carrying prediction
                                    // history from the sign-out animation into the login screen.
                                    ctx.secureReady = false;
                                    ctx.consoleNormalReadyAfterTickNs = 0;
                                    if (newHandoff) ++ctx.consoleGeneration;

                                    if (!ctx.handoffStateAnnounced) {
                                        SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                        ctx.handoffStateAnnounced = true;
                                    }

                                    if (ctx.normalStreamerProcess && ctx.normalStopEvent) {
                                        SetEvent(ctx.normalStopEvent);
                                        ctx.normalRetireRequestedAt = now;
                                    }
                                    ctx.lastSecureLaunchAttempt = now - std::chrono::seconds(1);
                                    LogI("[session-handoff] Winlogon is authoritative session=" + ctx.sessionId +
                                        " console=" + SessionIdToString(currentConsole));
                                }
                                else if (ctx.loginDesktopMode) {
                                    // A user token has appeared. Keep the secure/login feed visible
                                    // while the normal worker warms, then switch video + input together.
                                    ctx.loginDesktopMode = false;
                                    ctx.normalRecoveryArmed = true;
                                    ctx.consoleHandoffActive = true;
                                    ctx.bannerRebindPending = true;
                                    ctx.consoleNormalReadyAfterTickNs =
                                        static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                                    ctx.normalInputPipe.ResetConsumerState();
                                    ctx.lastNormalLaunchAttempt = now - std::chrono::seconds(3);
                                    ++ctx.consoleGeneration;
                                    if (!ctx.handoffStateAnnounced) {
                                        SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                        ctx.handoffStateAnnounced = true;
                                    }
                                    LogI("[session-handoff] interactive desktop available session=" + ctx.sessionId +
                                        " console=" + SessionIdToString(currentConsole));
                                }
                            }
                        }
                    }

                    if (ctx.consoleHandoffActive && ctx.normalStreamerProcess &&
                        ctx.normalStreamerSessionId != 0xFFFFFFFF &&
                        ctx.normalStreamerSessionId != ctx.activeConsoleSessionId &&
                        ctx.normalRetireRequestedAt.time_since_epoch().count() != 0 &&
                        now - ctx.normalRetireRequestedAt >= std::chrono::milliseconds(750)) {
                        LogW("[session-handoff] forcing old normal helper exit session=" + ctx.sessionId +
                            " worker_console=" + SessionIdToString(ctx.normalStreamerSessionId));
                        TerminateProcess(ctx.normalStreamerProcess, 0);
                        WaitForSingleObject(ctx.normalStreamerProcess, 100);
                    }

                    if (ctx.normalStreamerProcess) {
                        const DWORD waitRc = WaitForSingleObject(ctx.normalStreamerProcess, 0);
                        if (waitRc == WAIT_OBJECT_0) {
                            DWORD exitCode = 0;
                            GetExitCodeProcess(ctx.normalStreamerProcess, &exitCode);
                            LogW("normal streamer exited session=" + ctx.sessionId +
                                " exitCode=" + std::to_string(exitCode));
                            CloseHandle(ctx.normalStreamerProcess);
                            ctx.normalStreamerProcess = nullptr;
                            ctx.normalStreamerSessionId = 0xFFFFFFFF;
                            ctx.normalRetireRequestedAt = {};
                            ctx.lastNormalFrameAt = {};

                            const bool firstNormalLoss = !ctx.normalDesktopUnavailable;
                            const bool recoveryProbeWasArmed = ctx.normalDesktopUnavailable && ctx.normalRecoveryArmed;
                            ctx.normalDesktopUnavailable = true;
                            ctx.normalRecoveryArmed = recoveryProbeWasArmed;
                            ctx.consoleHandoffActive = true;
                            // Give the SCM LOGOFF notification time to arrive and disarm normal
                            // recovery. The normal desktop is not allowed back into the stream until
                            // Windows reports a genuine LOGON/user-desktop transition.
                            ctx.lastNormalLaunchAttempt = now + std::chrono::milliseconds(1500);

                            if (firstNormalLoss && !ctx.handoffStateAnnounced) {
                                SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                ctx.handoffStateAnnounced = true;
                            }

                            // Start Menu sign-out can kill the normal desktop before it sets UACActive.
                            // Move immediately to the secure capture path and let SCM LOGOFF latch it.
                            if (!ctx.secureStreamerProcess && !ctx.secureLaunchInProgress &&
                                ctx.activeConsoleSessionId != 0xFFFFFFFF &&
                                now - ctx.lastSecureLaunchAttempt >= std::chrono::milliseconds(300)) {
                                LaunchSecureStreamer(ctx);
                            }
                        }
                    }

                    // During console migration warm the Winlogon helper and the new normal
                    // helper under the same WebRTC session. The first valid frame decides the
                    // visible source; there is no peer/data-channel restart.
                    if (!ctx.backstageMode && (ctx.consoleHandoffActive || ctx.loginDesktopMode) &&
                        ctx.activeConsoleSessionId != 0xFFFFFFFF &&
                        !ctx.secureStreamerProcess && !ctx.secureLaunchInProgress &&
                        now - ctx.lastSecureLaunchAttempt >= std::chrono::milliseconds(300)) {
                        LaunchSecureStreamer(ctx);
                    }

                    if (!ctx.backstageMode &&
                        !ctx.normalStreamerProcess &&
                        !ctx.consoleSwitchInProgress &&
                        !ctx.logoffLatched &&
                        !ctx.loginDesktopMode &&
                        ctx.interactiveUserReady &&
                        (!ctx.normalDesktopUnavailable || ctx.normalRecoveryArmed) &&
                        ctx.activeConsoleSessionId != 0xFFFFFFFF &&
                        ActiveConsoleSessionId() == ctx.activeConsoleSessionId &&
                        now - ctx.lastNormalLaunchAttempt >= std::chrono::milliseconds(250)) {
                        ctx.lastNormalLaunchAttempt = now;
                        if (ctx.normalDesktopUnavailable) {
                            // Probe the recovered normal desktop behind the still-visible secure
                            // feed. Do not announce Switching; only commit once a fresh normal
                            // frame proves winsta0\\default is actually usable.
                            ctx.consoleHandoffActive = true;
                            ctx.consoleNormalReadyAfterTickNs =
                                static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                        }
                        LaunchNormalStreamer(ctx);
                    }

                    if (ctx.backstageHostProcess) {
                        const DWORD waitRc = WaitForSingleObject(ctx.backstageHostProcess, 0);
                        if (waitRc == WAIT_OBJECT_0) {
                            DWORD exitCode = 0;
                            GetExitCodeProcess(ctx.backstageHostProcess, &exitCode);
                            LogW("backstage host exited session=" + ctx.sessionId +
                                " exitCode=" + std::to_string(exitCode));
                            CloseHandle(ctx.backstageHostProcess);
                            ctx.backstageHostProcess = nullptr;
                            ctx.backstageMode = ctx.sessionMode == SessionMode::Backstage;
                            ctx.lastNormalFrameAt = {};
                            if (ctx.sessionMode == SessionMode::Backstage) {
                                LaunchBackstageHost(ctx);
                            }
                        }
                    }

                    if (ctx.consoleHandoffActive && ctx.secureStreamerProcess &&
                        ctx.secureStreamerSessionId != 0xFFFFFFFF &&
                        ctx.secureStreamerSessionId != ctx.activeConsoleSessionId &&
                        ctx.secureRetireRequestedAt.time_since_epoch().count() != 0 &&
                        now - ctx.secureRetireRequestedAt >= std::chrono::milliseconds(750)) {
                        LogW("[session-handoff] forcing old secure helper exit session=" + ctx.sessionId +
                            " worker_console=" + SessionIdToString(ctx.secureStreamerSessionId));
                        TerminateProcess(ctx.secureStreamerProcess, 0);
                        WaitForSingleObject(ctx.secureStreamerProcess, 100);
                    }

                    if (ctx.secureStreamerProcess) {
                        const DWORD waitRc = WaitForSingleObject(ctx.secureStreamerProcess, 0);
                        if (waitRc == WAIT_OBJECT_0) {
                            DWORD exitCode = 0;
                            const bool expectedSecureExit = ctx.secureRetiring;
                            GetExitCodeProcess(ctx.secureStreamerProcess, &exitCode);
                            LogW("secure streamer exited session=" + ctx.sessionId +
                                " exitCode=" + std::to_string(exitCode));
                            if (!expectedSecureExit) {
                                RecordSecureHostFailure(ctx, now, "process exited code=" + std::to_string(exitCode));
                            }
                            CloseHandle(ctx.secureStreamerProcess);
                            ctx.secureStreamerProcess = nullptr;
                            ctx.secureStreamerSessionId = 0xFFFFFFFF;
                            ctx.secureRetireRequestedAt = {};
                            ctx.secureRetiring = false;
                            ctx.secureReady = false;
                            ctx.secureLaunchInProgress = false;
                            ctx.secureShmem.Close();
                        }
                    }

                    if (now >= nextSecurePoll) {
                        if (ctx.backstageMode || ctx.loginDesktopMode) {
                            // Winlogon/login-screen mode is already a secure desktop. Do not
                            // infer UAC state from a normal input pipe that intentionally has
                            // no worker attached.
                            ctx.uacRequested = false;
                            nextSecurePoll = now + std::chrono::milliseconds(250);
                        }
                        else {
                            const bool previousUacRequested = ctx.uacRequested;
                            ctx.uacRequested = ctx.normalInputPipe.GetUACActive();

                            if (ctx.uacRequested != previousUacRequested) {
                                uint64_t transitionTickNs = ctx.normalInputPipe.GetDesktopTransitionTickNs();
                                if (transitionTickNs == 0) {
                                    transitionTickNs = static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                                }
                                ctx.unifiedSecureReady = false;
                                if (ctx.uacRequested) {
                                    ctx.uacDetectedAt = now;
                                    ctx.desktopReturnAt = {};
                                    ctx.uacDetectedTickNs = transitionTickNs;
                                    ctx.desktopReturnTickNs = 0;
                                }
                                else {
                                    ctx.uacDetectedAt = {};
                                    ctx.desktopReturnAt = now;
                                    ctx.desktopReturnTickNs = transitionTickNs;
                                }
                            }

                            if (ctx.uacRequested) {
                                if (!ctx.secureStateAnnounced) {
                                    SendSessionState(ctx.sessionId, "secure_desktop_entering");
                                    ctx.secureStateAnnounced = true;
                                }

                                if (!ctx.unifiedDesktopStreamer) {
                                    if (!ctx.secureStreamerProcess && !ctx.secureLaunchInProgress) {
                                        LaunchSecureStreamer(ctx);
                                    }
                                }
                                else if (!ctx.unifiedSecureReady && !ctx.secureStreamerProcess && !ctx.secureLaunchInProgress &&
                                    ctx.uacDetectedAt.time_since_epoch().count() != 0 &&
                                    now - ctx.uacDetectedAt >= std::chrono::milliseconds(350) &&
                                    now - ctx.lastSecureLaunchAttempt >= std::chrono::milliseconds(300)) {
                                    LogW("dynamic desktop did not produce a secure frame quickly; launching fallback secure helper session=" + ctx.sessionId);
                                    LaunchSecureStreamer(ctx);
                                }
                            }
                            else if (ctx.secureStateAnnounced && !ctx.handoffStateAnnounced) {
                                SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                ctx.handoffStateAnnounced = true;
                            }

                            if (!ctx.unifiedDesktopStreamer) {
                                const bool normalFresh = ctx.lastNormalFrameAt.time_since_epoch().count() != 0 &&
                                    (now - ctx.lastNormalFrameAt) <= std::chrono::milliseconds(800);
                                if (!ctx.uacRequested && normalFresh) {
                                    if (ctx.handoffStateAnnounced) {
                                        SendSessionState(ctx.sessionId, "desktop_handoff_ready");
                                        ctx.handoffStateAnnounced = false;
                                    }
                                    if (ctx.secureStateAnnounced) {
                                        SendSessionState(ctx.sessionId, "secure_desktop_exited");
                                        ctx.secureStateAnnounced = false;
                                    }
                                    if (ctx.activeMode == DesktopMode::Secure || ctx.secureStreamerProcess) {
                                        ctx.activeMode = DesktopMode::Normal;
                                        StopSecureStreamer(ctx);
                                        LogI("active mode -> NORMAL session=" + ctx.sessionId);
                                    }
                                }
                            }

                            nextSecurePoll = now + std::chrono::milliseconds(25);
                        }
                    }

                    bool gotNormal = false;
                    bool gotNormalGpu = false;
                    bool gotNormalRaw = false;
                    bool gotSecure = false;
                    bool secureBecameReady = false;
                    bool secureTransitionBlank = false;

                    // Normal desktop prefers a shared D3D11 texture descriptor.
                    // Secure desktop and unsupported/all-monitor capture retain raw I420.
                    while (ctx.normalShmem.ReadSharedGpuFrame(normalGpuFrame, normalTs)) {
                        gotNormalGpu = true;
                    }
                    if (!gotNormalGpu) {
                        while (ctx.normalShmem.ReadRawI420Frame(normalFrame, normalTs)) {
                            gotNormalRaw = true;
                        }
                    }
                    gotNormal = gotNormalGpu || gotNormalRaw;
                    if (gotNormal) {
                        const uint64_t nowTickNs = static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                        if (normalTs != 0 && nowTickNs > normalTs && nowTickNs - normalTs > maxFrameAgeNs) {
                            gotNormal = false;
                            ++staleFramesDropped;
                        }
                        else {
                            ctx.lastNormalFrameAt = now;
                        }
                    }

                    if (ctx.secureStreamerProcess) {
                        while (ctx.secureShmem.ReadRawI420Frame(secureFrame, secureTs)) {
                            gotSecure = true;
                        }

                        if (gotSecure) {
                            const uint64_t nowTickNs = static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                            if (secureTs != 0 && nowTickNs > secureTs && nowTickNs - secureTs > maxFrameAgeNs) {
                                gotSecure = false;
                                ++staleFramesDropped;
                            }
                        }

                        if (gotSecure) {
                            ctx.lastSecureFrameAt = now;
                            secureTransitionBlank = ctx.uacRequested &&
                                ctx.uacDetectedAt.time_since_epoch().count() != 0 &&
                                now - ctx.uacDetectedAt < std::chrono::milliseconds(250) &&
                                isNearBlackTransitionFrame(secureFrame);
                            const bool secureWorkerCurrent = ctx.secureStreamerSessionId == 0xFFFFFFFF ||
                                ctx.activeConsoleSessionId == 0xFFFFFFFF ||
                                ctx.secureStreamerSessionId == ctx.activeConsoleSessionId;
                            if (!ctx.secureRetiring && secureWorkerCurrent && !secureTransitionBlank && !ctx.secureReady) {
                                secureBecameReady = true;
                                ctx.secureReady = true;
                                ctx.secureLaunchInProgress = false;
                                ResetSecureHostFailureCircuit(ctx);
                                ctx.activeMode = DesktopMode::Secure;
                                SendSessionState(ctx.sessionId, "secure_desktop_ready");
                                LogI("active mode -> SECURE session=" + ctx.sessionId);
                            }

                            // The secure helper may already have been ready while sign-out was
                            // still reporting an interactive token. As soon as Winlogon becomes
                            // authoritative, any fresh secure frame can complete the handoff.
                            if ((ctx.loginDesktopMode || ctx.normalDesktopUnavailable) && ctx.consoleHandoffActive &&
                                !ctx.secureRetiring && secureWorkerCurrent && !secureTransitionBlank) {
                                ctx.consoleHandoffActive = false;
                                ctx.consoleNormalReadyAfterTickNs = 0;
                                ctx.secureFallbackOwnsInput.store(true, std::memory_order_release);
                                // Winlogon is a usable steady-state desktop, including when a
                                // technician reconnects while nobody is logged in. Always tell
                                // the Viewer the handoff is complete once a fresh secure frame
                                // is available; do not require a prior "entering" announcement.
                                SendSessionState(ctx.sessionId, "desktop_handoff_ready");
                                ctx.handoffStateAnnounced = false;
                                LogI("[session-handoff] login desktop ready session=" + ctx.sessionId +
                                    " console=" + SessionIdToString(ctx.activeConsoleSessionId) +
                                    " input=secure");
                            }
                        }
                    }

                    bool sent = false;
                    const bool normalHandoffReady = ctx.consoleHandoffActive && gotNormal &&
                        !ctx.loginDesktopMode && ctx.interactiveUserReady &&
                        ctx.activeConsoleSessionId != 0xFFFFFFFF &&
                        ActiveConsoleSessionId() == ctx.activeConsoleSessionId &&
                        ctx.normalStreamerSessionId == ctx.activeConsoleSessionId &&
                        (ctx.consoleNormalReadyAfterTickNs == 0 || normalTs >= ctx.consoleNormalReadyAfterTickNs) &&
                        !(gotNormalRaw && isNearBlackTransitionFrame(normalFrame));
                    const bool useSecureFallback = !normalHandoffReady && !ctx.secureRetiring &&
                        ctx.secureStreamerProcess != nullptr &&
                        (ctx.normalDesktopUnavailable || ctx.loginDesktopMode || ctx.uacRequested ||
                            ctx.consoleHandoffActive || !gotNormal);

                    if (useSecureFallback) {
                        if (gotSecure && !secureTransitionBlank) {
                            ++framesForwarded;
                            const bool forceKf = secureBecameReady || ctx.activeMode != DesktopMode::Secure || !ctx.secureReady;
                            if (framesForwarded == 1 || (framesForwarded % 120) == 0) {
                                LogI("raw frame forwarded session=" + ctx.sessionId +
                                    " frames=" + std::to_string(framesForwarded) +
                                    " size=" + std::to_string(secureFrame.y.size() + secureFrame.u.size() + secureFrame.v.size()) +
                                    " mode=secure-fallback force_kf=" + std::to_string(forceKf ? 1 : 0));
                            }
                            ctx.secureFallbackOwnsInput.store(true, std::memory_order_release);
                            sent = ctx.mediaShmem.WriteRawI420Frame(secureFrame, secureTs, forceKf);
                        }
                    }
                    else if (!ctx.loginDesktopMode && gotNormal &&
                        (!ctx.normalDesktopUnavailable || normalHandoffReady) &&
                        (!ctx.consoleHandoffActive || normalHandoffReady)) {
                        const bool staleSecureCandidate = ctx.unifiedDesktopStreamer && ctx.uacRequested &&
                            (gotNormalGpu || (ctx.uacDetectedTickNs != 0 && normalTs < ctx.uacDetectedTickNs));
                        const bool staleNormalReturn = ctx.unifiedDesktopStreamer && !ctx.uacRequested &&
                            ctx.activeMode == DesktopMode::Secure && ctx.desktopReturnTickNs != 0 &&
                            normalTs < ctx.desktopReturnTickNs;
                        const bool blankSecureTransition = ctx.unifiedDesktopStreamer && ctx.uacRequested &&
                            ctx.uacDetectedAt.time_since_epoch().count() != 0 &&
                            now - ctx.uacDetectedAt < std::chrono::milliseconds(250) &&
                            gotNormalRaw && isNearBlackTransitionFrame(normalFrame);
                        const bool blankNormalReturn = ctx.unifiedDesktopStreamer && !ctx.uacRequested &&
                            ctx.desktopReturnAt.time_since_epoch().count() != 0 &&
                            now - ctx.desktopReturnAt < std::chrono::milliseconds(250) &&
                            gotNormalRaw && isNearBlackTransitionFrame(normalFrame);
                        const bool blankConsoleHandoff = ctx.consoleHandoffActive && gotNormalRaw &&
                            isNearBlackTransitionFrame(normalFrame);

                        if (!staleSecureCandidate && !staleNormalReturn && !blankSecureTransition &&
                            ctx.unifiedDesktopStreamer && ctx.uacRequested) {
                            const bool enteringSecure = !ctx.unifiedSecureReady || ctx.activeMode != DesktopMode::Secure;
                            ++framesForwarded;
                            const bool forceKf = enteringSecure || framesForwarded == 1;
                            ctx.secureFallbackOwnsInput.store(false, std::memory_order_release);
                            if (gotNormalRaw) {
                                sent = ctx.mediaShmem.WriteRawI420Frame(normalFrame, normalTs, forceKf);
                            }
                            if (enteringSecure && gotNormalRaw) {
                                ctx.unifiedSecureReady = true;
                                ctx.activeMode = DesktopMode::Secure;
                                ctx.handoffStateAnnounced = false;
                                SendSessionState(ctx.sessionId, "secure_desktop_ready");
                                LogI("active mode -> SECURE session=" + ctx.sessionId + " source=dynamic-desktop");
                            }
                        }
                        else if (!staleSecureCandidate && !staleNormalReturn && !blankNormalReturn &&
                            !blankConsoleHandoff && !ctx.uacRequested) {
                            const bool wasSecure = ctx.activeMode == DesktopMode::Secure || ctx.secureStreamerProcess != nullptr;
                            if (wasSecure) {
                                if (ctx.handoffStateAnnounced) {
                                    SendSessionState(ctx.sessionId, "desktop_handoff_ready");
                                    ctx.handoffStateAnnounced = false;
                                }
                                if (ctx.secureStateAnnounced) {
                                    SendSessionState(ctx.sessionId, "secure_desktop_exited");
                                    ctx.secureStateAnnounced = false;
                                }
                                if (ctx.secureStreamerProcess && ctx.secureStopEvent) {
                                    // Do not synchronously wait here; the normal frame is ready now.
                                    // Request the Winlogon helper to exit after the source/input
                                    // switch so the Viewer never waits on process teardown or falls
                                    // back to an old secure frame after the new desktop is visible.
                                    ctx.secureRetiring = true;
                                    ctx.secureRetireRequestedAt = now;
                                    SetEvent(ctx.secureStopEvent);
                                }
                                ctx.activeMode = DesktopMode::Normal;
                                ctx.unifiedSecureReady = false;
                                ctx.desktopReturnTickNs = 0;
                                ctx.desktopReturnAt = {};
                                LogI("active mode -> NORMAL session=" + ctx.sessionId + " raw-source=1");
                            }

                            const bool completedConsoleHandoff = ctx.consoleHandoffActive;
                            ctx.secureFallbackOwnsInput.store(false, std::memory_order_release);
                            ++framesForwarded;
                            const bool forceKf = wasSecure || completedConsoleHandoff || framesForwarded == 1;
                            if (gotNormalGpu) {
                                SharedGpuFrame mediaGpuFrame;
                                if (duplicateGpuHandleForMedia(normalGpuFrame, mediaGpuFrame)) {
                                    sent = ctx.mediaShmem.WriteSharedGpuFrame(mediaGpuFrame, normalTs, forceKf);
                                } else {
                                    ctx.gpuTransportFallbackRequested.store(true, std::memory_order_release);
                                    LogW("[gpu-transport] unable to duplicate shared texture; frame dropped pending raw fallback session=" +
                                        ctx.sessionId);
                                }
                            } else {
                                sent = ctx.mediaShmem.WriteRawI420Frame(normalFrame, normalTs, forceKf);
                            }

                            if (completedConsoleHandoff) {
                                ctx.consoleHandoffActive = false;
                                ctx.normalDesktopUnavailable = false;
                                ctx.normalRecoveryArmed = false;
                                ctx.consoleNormalReadyAfterTickNs = 0;
                                if (ctx.handoffStateAnnounced) {
                                    SendSessionState(ctx.sessionId, "desktop_handoff_ready");
                                    ctx.handoffStateAnnounced = false;
                                }
                                LogI("[session-handoff] ready generation=" + std::to_string(ctx.consoleGeneration) +
                                    " session=" + ctx.sessionId +
                                    " console=" + SessionIdToString(ctx.activeConsoleSessionId) +
                                    " input=normal");

                                if (ctx.bannerRebindPending && ctx.sessionMode == SessionMode::Console) {
                                    StartPresenceBanner(ctx.sessionId, ctx.technicianName, ctx.sessionJob, false);
                                    ctx.bannerConsoleSessionId = ctx.activeConsoleSessionId;
                                    ctx.bannerRebindPending = false;
                                    LogI("[session-handoff] presence rebound session=" + ctx.sessionId +
                                        " console=" + SessionIdToString(ctx.bannerConsoleSessionId));
                                }
                                SyncChatStateToContext(ctx);
                            }
                        }
                    }

                    if (now >= nextDiagnosticsPoll) {
                        if (staleFramesDropped > 0 && now >= nextStaleFrameLog) {
                            LogW("stale remote frames dropped session=" + ctx.sessionId +
                                " count=" + std::to_string(staleFramesDropped) +
                                " max_age_ms=" + std::to_string(maxFrameAgeNs / 1000000ull));
                            staleFramesDropped = 0;
                            nextStaleFrameLog = now + std::chrono::seconds(2);
                        }
                        hi5::StreamStats stats{};
                        if (ctx.normalInputPipe.ReadStreamStats(lastNormalStatsSeq, stats)) {
                            lastNormalStatsSeq = stats.seq;
                            SendMediaControl(ctx, json{
                                {"type", "stream_hint"},
                                {"stream_mode", stats.streamMode},
                                {"target_fps", stats.targetFps},
                                {"backstage", ctx.backstageMode},
                                {"secure", stats.secureDesktopActive != 0}
                            });
                            SendStreamDiagnostics(ctx.sessionId, "normal", stats, framesForwarded, ctx.activeMode, ctx.backstageMode);
                        }
                        if (ctx.secureInputPipe.ReadStreamStats(lastSecureStatsSeq, stats)) {
                            lastSecureStatsSeq = stats.seq;
                            SendMediaControl(ctx, json{
                                {"type", "stream_hint"},
                                {"stream_mode", stats.streamMode},
                                {"target_fps", stats.targetFps},
                                {"backstage", ctx.backstageMode},
                                {"secure", stats.secureDesktopActive != 0}
                            });
                            SendStreamDiagnostics(ctx.sessionId, "secure", stats, framesForwarded, ctx.activeMode, ctx.backstageMode);
                        }
                        nextDiagnosticsPoll = now + std::chrono::seconds(1);
                    }

                    if (now >= nextMemoryDiagnostics) {
                        LogProcessMemorySnapshot(ctx.sessionId, "agent-service", GetCurrentProcess());
                        if (ctx.mediaHostProcess) LogProcessMemorySnapshot(ctx.sessionId, "media-host", ctx.mediaHostProcess);
                        if (ctx.normalStreamerProcess) LogProcessMemorySnapshot(ctx.sessionId, "remote-host-normal", ctx.normalStreamerProcess);
                        if (ctx.secureStreamerProcess) LogProcessMemorySnapshot(ctx.sessionId, "remote-host-secure", ctx.secureStreamerProcess);
                        if (ctx.backstageHostProcess) LogProcessMemorySnapshot(ctx.sessionId, "remote-host-backstage", ctx.backstageHostProcess);
                        nextMemoryDiagnostics = now + std::chrono::seconds(5);
                    }

                    if (!sent) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }

                    if (now >= nextMonitorPoll) {
                        const auto displayGeometry = ReadDisplayGeometrySnapshot();
                        if (displayGeometry != ctx.lastDisplayGeometry) {
                            LogDisplayGeometry(ctx.sessionId, "changed_during_session", displayGeometry);
                            ctx.lastDisplayGeometry = displayGeometry;
                        }
                        const int count = ctx.normalInputPipe.GetMonitorCount();
                        if (count > 0) {
                            const int monitorIndex = std::clamp(ctx.displayIndex, 0, count - 1);
                            const auto captureMonitor = ctx.normalInputPipe.GetMonitorInfo(monitorIndex);
                            if (captureMonitor.w > 0 && captureMonitor.h > 0 &&
                                (captureMonitor.w != ctx.lastCaptureMonitorWidth || captureMonitor.h != ctx.lastCaptureMonitorHeight)) {
                                LogI("[display-state] phase=capture_monitor session=" + ctx.sessionId +
                                    " monitor=" + std::to_string(monitorIndex) +
                                    " geometry=" + std::to_string(captureMonitor.w) + "x" + std::to_string(captureMonitor.h));
                                ctx.lastCaptureMonitorWidth = captureMonitor.w;
                                ctx.lastCaptureMonitorHeight = captureMonitor.h;
                            }
                        }
                        if (count > 0 && signaling_) {
                            json msg;
                            msg["type"] = "monitor_info";
                            msg["session_id"] = ctx.sessionId;
                            msg["current"] = ctx.displayIndex;
                            msg["monitors"] = json::array();
                            for (int i = 0; i < count; ++i) {
                                const auto mi = ctx.normalInputPipe.GetMonitorInfo(i);
                                msg["monitors"].push_back({
                                    {"index", i}, {"name", std::string("Monitor ") + std::to_string(i + 1)},
                                    {"x", mi.x}, {"y", mi.y}, {"w", mi.w}, {"h", mi.h}, {"primary", mi.primary != 0}
                                    });
                            }
                            signaling_->send(msg.dump());
                        }
                        nextMonitorPoll = now + std::chrono::seconds(1);
                    }
                }

                LogI("frame pump stop session=" + ctx.sessionId);
            }

            void StopSession(const std::string& sessionId) {
                LogI("stopping session=" + sessionId);

                // Always clean user-facing helpers first. In the real world the viewer can
                // disconnect after the WebRTC/session map has already been removed, or the
                // control server can deliver a duplicate end message. The old code returned
                // early when the session context was missing, leaving the banner/chat helper
                // orphaned. UI cleanup must therefore not depend on sessions_ containing the id.
                StopChatOverlay(sessionId);
                StopPresenceBanner(sessionId);

                std::unique_ptr<SessionContext> ctx;

                {
                    std::lock_guard<std::mutex> lock(sessionsMu_);
                    auto it = sessions_.find(sessionId);
                    if (it != sessions_.end()) {
                        ctx = std::move(it->second);
                        sessions_.erase(it);
                    }
                }

                if (!ctx) {
                    LogW("StopSession: no active stream context for session=" + sessionId + "; cleaning possible legacy orphan");
                    CleanupOrphanStreamerProcesses(sessionId);
                    if (!HasActiveSessions()) {
                        CleanupOrphanUiHelperProcesses();
                        TrimIdleWorkingSet();
                    }
                    return;
                }

                ctx->framePumpStop.store(true);

                if (ctx->normalStopEvent) SetEvent(ctx->normalStopEvent);
                if (ctx->secureStopEvent) SetEvent(ctx->secureStopEvent);
                if (ctx->mediaStopEvent) SetEvent(ctx->mediaStopEvent);

                if (ctx->framePumpThread.joinable()) {
                    ctx->framePumpThread.join();
                }

                if (ctx->backstageHostProcess) {
                    const DWORD waitRc = WaitForSingleObject(ctx->backstageHostProcess, 3000);
                    if (waitRc == WAIT_TIMEOUT) {
                        TerminateProcess(ctx->backstageHostProcess, 0);
                        WaitForSingleObject(ctx->backstageHostProcess, 1000);
                        LogW("forced backstage host termination session=" + sessionId);
                    }
                    CloseHandle(ctx->backstageHostProcess);
                    ctx->backstageHostProcess = nullptr;
                }

                if (ctx->normalStreamerProcess) {
                    const DWORD waitRc = WaitForSingleObject(ctx->normalStreamerProcess, 3000);
                    if (waitRc == WAIT_TIMEOUT) {
                        TerminateProcess(ctx->normalStreamerProcess, 0);
                        WaitForSingleObject(ctx->normalStreamerProcess, 1000);
                        LogW("forced normal streamer termination session=" + sessionId);
                    }
                    CloseHandle(ctx->normalStreamerProcess);
                    ctx->normalStreamerProcess = nullptr;
                }

                if (ctx->secureStreamerProcess) {
                    const DWORD waitRc = WaitForSingleObject(ctx->secureStreamerProcess, 3000);
                    if (waitRc == WAIT_TIMEOUT) {
                        TerminateProcess(ctx->secureStreamerProcess, 0);
                        WaitForSingleObject(ctx->secureStreamerProcess, 1000);
                        LogW("forced secure streamer termination session=" + sessionId);
                    }
                    CloseHandle(ctx->secureStreamerProcess);
                    ctx->secureStreamerProcess = nullptr;
                }

                if (ctx->mediaHostProcess) {
                    const DWORD waitRc = WaitForSingleObject(ctx->mediaHostProcess, 3000);
                    if (waitRc == WAIT_TIMEOUT) {
                        LogW("media host did not exit before session job close session=" + sessionId);
                    }
                    CloseHandle(ctx->mediaHostProcess);
                    ctx->mediaHostProcess = nullptr;
                }

                ctx->mediaControlPipe.Close();
                if (ctx->mediaEventPipeServer) {
                    ctx->mediaEventPipeServer->Stop();
                    ctx->mediaEventPipeServer.reset();
                }

                // KILL_ON_JOB_CLOSE is the final OS-enforced boundary. Anything that ignored
                // its stop event (streamer, banner, chat or a child helper) cannot outlive the session.
                if (ctx->sessionJob) {
                    CloseHandle(ctx->sessionJob);
                    ctx->sessionJob = nullptr;
                    LogI("[session-job] closed kill boundary session=" + sessionId);
                }

                CleanupOrphanStreamerProcesses(sessionId);

                if (ctx->normalStopEvent) {
                    CloseHandle(ctx->normalStopEvent);
                    ctx->normalStopEvent = nullptr;
                }

                if (ctx->secureStopEvent) {
                    CloseHandle(ctx->secureStopEvent);
                    ctx->secureStopEvent = nullptr;
                }

                if (ctx->mediaStopEvent) {
                    CloseHandle(ctx->mediaStopEvent);
                    ctx->mediaStopEvent = nullptr;
                }

                if (ctx->chatPipeServer) {
                    ctx->chatPipeServer->Stop();
                    ctx->chatPipeServer.reset();
                }

                ctx->secureInputPipe.Close();
                ctx->normalInputPipe.Close();
                ctx->secureShmem.Close();
                ctx->mediaShmem.Close();
                ctx->normalShmem.Close();

                const auto displayGeometryAtEnd = ReadDisplayGeometrySnapshot();
                LogDisplayGeometry(sessionId, "session_end", displayGeometryAtEnd);
                if (displayGeometryAtEnd != ctx->displayGeometryAtStart) {
                    LogW("[display-state] endpoint geometry differs from remote-session start session=" + sessionId);
                }

                CancelFileUploadsForSession(sessionId);
                LogSupportEvent("Remote session ended");
                LogI("session stopped=" + sessionId);
                if (!HasActiveSessions()) {
                    CleanupOrphanUiHelperProcesses();
                    TrimIdleWorkingSet();
                }
            }

        private:
            std::string serviceName_;

            std::mutex terminalsMutex_;
            std::map<std::string, std::shared_ptr<TerminalSession>> terminals_;

            std::atomic<bool> stop_{ false };
            std::atomic<bool> signalingConnected_{ false };
            std::atomic<bool> signalingReconnectRequested_{ false };

            SessionBridge sessionBridge_;
            AgentPresenceController presenceController_;
            ChatController chatController_;
            std::unique_ptr<SignalingClient> signaling_;
            std::thread inventoryThread_;
            std::thread trayThread_;

            std::mutex sessionsMu_;
            std::unordered_map<std::string, std::unique_ptr<SessionContext>> sessions_;
            std::mutex fileUploadsMu_;
            std::unordered_map<std::string, std::unique_ptr<IncomingFileUpload>> fileUploads_;
        };

        static std::unique_ptr<Worker> g_worker;

        DWORD WINAPI ServiceCtrlHandlerEx(DWORD ctrlCode, DWORD eventType, LPVOID eventData, LPVOID) {
            switch (ctrlCode) {
            case SERVICE_CONTROL_STOP:
            case SERVICE_CONTROL_SHUTDOWN:
                LogI("service control -> stop/shutdown");
                SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 4000);
                g_stopRequested.store(true);
                if (g_worker) {
                    g_worker->RequestStop();
                }
                return NO_ERROR;
            case SERVICE_CONTROL_SESSIONCHANGE: {
                const auto* notification = static_cast<WTSSESSION_NOTIFICATION*>(eventData);
                const DWORD sessionId = notification ? notification->dwSessionId : 0xFFFFFFFF;
                g_sessionChangeType.store(eventType, std::memory_order_release);
                g_sessionChangeSessionId.store(sessionId, std::memory_order_release);
                g_sessionChangeSeq.fetch_add(1, std::memory_order_acq_rel);
                LogI("service session change event=" + std::to_string(eventType) +
                    " windows_session=" + SessionIdToString(sessionId));
                return NO_ERROR;
            }
            default:
                return NO_ERROR;
            }
        }

        void WINAPI ServiceMainThunk(DWORD argc, LPSTR* argv) {
            if (argc > 0 && argv && argv[0] && argv[0][0]) {
                g_serviceName = argv[0];
            }
            LogI("ServiceMainThunk enter service=" + g_serviceName);
            g_statusHandle = RegisterServiceCtrlHandlerExA(g_serviceName.c_str(), ServiceCtrlHandlerEx, nullptr);
            if (!g_statusHandle) {
                LogE("RegisterServiceCtrlHandlerEx failed err=" + std::to_string(GetLastError()));
                return;
            }

            SetServiceState(SERVICE_START_PENDING, NO_ERROR, 30000);

            try {
                g_worker = std::make_unique<Worker>(g_serviceName);
                SetServiceState(SERVICE_RUNNING);
                LogI("service state -> RUNNING service=" + g_serviceName);

                g_worker->Run();

                g_worker.reset();
                SetServiceState(SERVICE_STOPPED);
                LogI("service state -> STOPPED");
            }
            catch (const std::exception& ex) {
                LogE(std::string("service exception: ") + ex.what());
                SetServiceState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
            }
            catch (...) {
                LogE("service unknown exception");
                SetServiceState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
            }
        }

    } // namespace

    int InstallService(const std::string& serviceName) {
        LogI("install requested");

        char exePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);

        std::string binPath = "\"";
        binPath += exePath;
        binPath += "\" --service";

        SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm) {
            LogError("[service] OpenSCManager failed err=" + std::to_string(GetLastError()));
            return 1;
        }

        SC_HANDLE svc = CreateServiceA(
            scm,
            serviceName.c_str(),
            serviceName.c_str(),
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            binPath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);

        if (!svc) {
            const DWORD err = GetLastError();

            if (err == ERROR_SERVICE_EXISTS || err == ERROR_DUP_NAME) {
                LogWarn("[service] service already exists, updating config");

                svc = OpenServiceA(scm, serviceName.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS);
                if (!svc) {
                    CloseServiceHandle(scm);
                    LogError("[service] OpenService(existing) failed err=" + std::to_string(GetLastError()));
                    return 1;
                }

                const BOOL changed = ChangeServiceConfigA(
                    svc,
                    SERVICE_NO_CHANGE,
                    SERVICE_AUTO_START,
                    SERVICE_NO_CHANGE,
                    binPath.c_str(),
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    serviceName.c_str());

                if (!changed) {
                    const DWORD changeErr = GetLastError();
                    CloseServiceHandle(svc);
                    CloseServiceHandle(scm);
                    LogError("[service] ChangeServiceConfig failed err=" + std::to_string(changeErr));
                    return 1;
                }

                SERVICE_DESCRIPTIONA desc{};
                std::string descText = "Hi5Central Agent";
                desc.lpDescription = descText.data();
                ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                LogInfo("[service] existing service updated: " + serviceName + " binPath=" + binPath);
                return 0;
            }

            CloseServiceHandle(scm);
            LogError("[service] CreateService failed err=" + std::to_string(err));
            return 1;
        }

        SERVICE_DESCRIPTIONA desc{};
        std::string descText = "Hi5Central Agent";
        desc.lpDescription = descText.data();
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        LogInfo("[service] installed: " + serviceName + " binPath=" + binPath);
        return 0;
    }

    int UninstallService(const std::string& serviceName) {
        LogI("uninstall requested");

        SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            LogError("[service] OpenSCManager failed err=" + std::to_string(GetLastError()));
            return 1;
        }

        SC_HANDLE svc = OpenServiceA(scm, serviceName.c_str(), DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!svc) {
            const DWORD err = GetLastError();
            CloseServiceHandle(scm);
            LogError("[service] OpenService failed err=" + std::to_string(err));
            return 1;
        }

        SERVICE_STATUS status{};
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        DeleteService(svc);

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        LogInfo("[service] uninstalled: " + serviceName);
        return 0;
    }

    int RunServiceMain(const std::string& serviceName) {
        LogI("RunServiceMain enter");
        g_serviceName = serviceName;

        SERVICE_TABLE_ENTRYA table[] = {
            { const_cast<char*>(g_serviceName.c_str()), ServiceMainThunk },
            { nullptr, nullptr }
        };

        if (StartServiceCtrlDispatcherA(table)) {
            return 0;
        }

        const DWORD err = GetLastError();
        if (err != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            LogError("[service] StartServiceCtrlDispatcher failed err=" + std::to_string(err));
            return 1;
        }

        LogWarn("[service] not started by SCM, running in console fallback mode");

        try {
            g_worker = std::make_unique<Worker>(g_serviceName);

            SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL {
                switch (ctrlType) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                case CTRL_SHUTDOWN_EVENT:
                    LogInfo("[service] console stop signal received");
                    g_stopRequested.store(true);
                    if (g_worker) {
                        g_worker->RequestStop();
                    }
                    return TRUE;
                default:
                    return FALSE;
                }
                }, TRUE);

            g_worker->Run();
            g_worker.reset();
            return 0;
        }
        catch (const std::exception& ex) {
            LogError(std::string("[service] console fallback exception: ") + ex.what());
            return 1;
        }
        catch (...) {
            LogError("[service] console fallback unknown exception");
            return 1;
        }
    }

} // namespace hi5