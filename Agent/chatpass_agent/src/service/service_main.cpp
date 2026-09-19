#include "service_main.h"

#include "../agent_identity.h"
#include "../signaling_client.h"
#include "../webrtc_sender.h"
#include "../codec_capabilities.h"
#include "../ipc/input_pipe.h"
#include "../ipc/session_launcher.h"
#include "../ipc/shmem_ring.h"
#include "../ipc/named_pipe.h"
#include "../util/log.h"
#include "../session/session_bridge.h"
#include "../ui/agent_presence_controller.h"
#include "../ui/chat_controller.h"
#include "../inventory/inventory_snapshot.h"
#include "../patching/patch_worker.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
                : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);

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

            if (backup.hadValue && ((backup.oldValue & 0x1u) != 0)) {
                CadLog("temporary SoftwareSASGeneration already allows Services; leaving existing value unchanged");
                RegCloseKey(key);
                return true;
            }

            DWORD newValue = backup.hadValue ? (backup.oldValue | 0x1u) : 1u;
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
            CadLog("CAD_BUILD=Temporary-SoftwareSASGeneration-SendSAS active=true");
            CadLogSessionDiagnostics(sessionId);

            SoftwareSasGenerationBackup backup;
            const bool policyReady = EnsureTemporarySoftwareSasForServices(backup);
            CadLog("temporary SoftwareSASGeneration policy_ready=" + BoolText(policyReady));

            bool ok = false;
            if (policyReady) {
                ok = InvokeSendSas(sessionId, "service", FALSE);
            }
            else {
                CadLog("service SendSAS skipped because temporary SoftwareSASGeneration could not be enabled");
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
                L"Get-CimInstance Win32_Process -Filter \\\"Name='native_vp8_stream.exe'\\\" | "
                L"Where-Object { $_.CommandLine -match '--mode\\\\s+(banner|chat-overlay)' } | "
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
            // Last-resort cleanup for streamer child processes. A normal/secure streamer is
            // launched as native_vp8_stream.exe --mode streamer. If the viewer closes without
            // a clean control-server stop message, the child can remain alive and keep the
            // chat pipe visible because its command line contains --chat-pipe. This cleanup
            // intentionally targets only --mode streamer helpers, never the --service process.
            std::wstring ps =
                L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
                L"$ErrorActionPreference='SilentlyContinue'; "
                L"$procs = Get-CimInstance Win32_Process -Filter \\\"Name='native_vp8_stream.exe'\\\" | "
                L"Where-Object { $_.CommandLine -match '--mode\\\\s+streamer|--mode=streamer' }; ";

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

        static std::string ChatOverlayExePath() {
            return CurrentExePath();
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
        };

        static std::mutex g_chatOverlayMu;
        static std::unordered_map<std::string, std::unique_ptr<ChatOverlayState>> g_chatOverlays;
        using ChatOverlayReplyCallback = std::function<void(const std::string& sessionId, const std::string& body)>;


        struct PresenceBannerState {
            std::string sessionId;
            HANDLE process = nullptr;
            HANDLE stopEvent = nullptr;
            std::string stopEventName;
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
                const DWORD waitRc = WaitForSingleObject(state.process, 3000);
                if (waitRc == WAIT_TIMEOUT) {
                    LogW("[presence] banner did not exit on stop event, terminating session=" + sessionId);
                    TerminateProcess(state.process, 0);
                    WaitForSingleObject(state.process, 1000);
                }
                CloseHandle(state.process);
            }
            if (state.stopEvent) CloseHandle(state.stopEvent);
        }

        static void StopPresenceBanners() {
            std::vector<std::string> ids;
            {
                std::lock_guard<std::mutex> lock(g_presenceBannerMu);
                for (const auto& kv : g_presenceBanners) ids.push_back(kv.first);
            }
            for (const auto& id : ids) StopPresenceBanner(id);
        }

        static void StartPresenceBanner(const std::string& sessionId, const std::string& technicianName) {
            if (sessionId.empty()) return;

            StopPresenceBanner(sessionId);

            const std::string exe = CurrentExePath();
            if (exe.empty()) {
                LogE("[presence] cannot resolve current executable path");
                return;
            }

            const std::string safeTech = technicianName.empty() ? "Technician" : technicianName;
            const std::string stopEventName = SessionBannerStopEventName(sessionId);
            HANDLE stopEvent = CreateOrOpenManualResetEventA(stopEventName, false);
            if (stopEvent) ResetEvent(stopEvent);
            const std::string args = "--mode banner --session " + QuoteArg(sessionId) +
                " --technician " + QuoteArg(safeTech) +
                " --stop-event " + QuoteArg(stopEventName);

            LogI("[presence] launching connected banner session=" + sessionId + " technician=" + safeTech + " stop_event=" + stopEventName);
            HANDLE proc = LaunchInInteractiveSession(exe, args);
            if (!proc) {
                LogE("[presence] banner launch failed session=" + sessionId);
                if (stopEvent) CloseHandle(stopEvent);
                return;
            }

            PresenceBannerState state;
            state.sessionId = sessionId;
            state.process = proc;
            state.stopEvent = stopEvent;
            state.stopEventName = stopEventName;
            {
                std::lock_guard<std::mutex> lock(g_presenceBannerMu);
                g_presenceBanners[sessionId] = state;
            }
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
            return CreateNamedPipeW(
                wName.c_str(),
                openMode,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                1,
                65536,
                65536,
                0,
                nullptr);
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
                    else if (type == "closed") {
                        LogI("[chat-ui] overlay closed by user session=" + state->sessionId);
                    }
                }
            }
        }

        static ChatOverlayState* EnsureChatOverlay(const std::string& sessionId, ChatOverlayReplyCallback cb) {
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

            const std::string args = "--mode chat-overlay --session " + QuoteArg(sessionId) +
                " --pipe-in " + QuoteArg(state->inPipeName) +
                " --pipe-out " + QuoteArg(state->outPipeName) +
                " --stop-event " + QuoteArg(state->stopEventName);
            LogI("[chat-ui] launching same-exe WebView2 overlay session=" + sessionId +
                " in=" + state->inPipeName + " out=" + state->outPipeName);
            raw->process = LaunchInInteractiveSession(overlayExe, args);
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

            g_chatOverlays[sessionId] = std::move(state);
            return raw;
        }

        static bool SendChatToOverlay(const std::string& sessionId,
            const std::string& displayName,
            const std::string& body,
            ChatOverlayReplyCallback cb) {
            if (body.empty()) return false;
            ChatOverlayState* state = EnsureChatOverlay(sessionId, std::move(cb));
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

            std::unique_ptr<WebRtcSender> sender;

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
            std::string chatPipeName;

            HANDLE normalStopEvent = nullptr;
            HANDLE secureStopEvent = nullptr;

            HANDLE normalStreamerProcess = nullptr;
            HANDLE secureStreamerProcess = nullptr;
            HANDLE backstageHostProcess = nullptr;
            bool backstageMode = false;
            SessionMode sessionMode = SessionMode::Console;
            std::unique_ptr<NamedPipeServer> chatPipeServer;

            std::thread framePumpThread;
            std::atomic<bool> framePumpStop{ false };

            int displayIndex = 0;
            int fps = 30;
            DesktopMode activeMode = DesktopMode::Normal;

            bool uacRequested = false;
            bool secureLaunchInProgress = false;
            bool secureReady = false;
            bool secureStateAnnounced = false;
            bool handoffStateAnnounced = false;
            bool chatOpen = false;
            std::chrono::steady_clock::time_point lastNormalFrameAt{};
            std::chrono::steady_clock::time_point lastSecureFrameAt{};
            std::chrono::steady_clock::time_point lastNormalLaunchAttempt{};
            std::chrono::steady_clock::time_point lastSecureLaunchAttempt{};
            std::chrono::steady_clock::time_point lastConsoleSessionPoll{};
            std::chrono::steady_clock::time_point lastConsoleSwitchDetected{};

            DWORD activeConsoleSessionId = 0xFFFFFFFF;
            DWORD lastSeenConsoleSessionId = 0xFFFFFFFF;
            bool consoleSwitchInProgress = false;
        };


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

                rtc::InitLogger(rtc::LogLevel::Info);

                constexpr int width = 0;
                constexpr int height = 0;
                const int fps = ReadConfigInt("HI5_STREAM_FPS", 20, 5, 60);

                const std::string requestedCodec = ReadConfigString("HI5_CODEC", "auto");
                const bool requestedH264ForDefaults =
                    requestedCodec == "h264_hw" || requestedCodec == "h264" || requestedCodec == "h264_sw";
                const int bitrateKbps = ReadConfigInt("HI5_STREAM_BITRATE_KBPS",
                    requestedH264ForDefaults ? 6000 : 16000,
                    1000,
                    50000);

                hi5::CodecSelectionResult codecProbe = hi5::ProbeCodecCapabilitiesAndSelect(requestedCodec);
                LogI("codec mode requested=" + codecProbe.requestedMode +
                    " selected=" + codecProbe.selectedCodec +
                    " reason=" + codecProbe.selectedReason);
                for (const auto& line : SplitLines(hi5::CodecCapabilitiesToLogString(codecProbe))) {
                    LogI(line);
                }

                const bool h264ExperimentRequested =
                    requestedCodec == "h264_hw" || requestedCodec == "h264" || requestedCodec == "h264_sw";

                LogI("stream config codec=" + std::string(h264ExperimentRequested ? "h264_experimental" : "vp8") +
                    " fps=" + std::to_string(fps) +
                    " bitrate_kbps=" + std::to_string(bitrateKbps) +
                    " note=" + std::string(h264ExperimentRequested
                        ? "Media Foundation H.264 sender experiment requested; set HI5_CODEC=vp8 to return to stable VP8"
                        : "stable VP8 path"));

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

                auto sendFn = [this](const std::string& payload) {
                    if (signaling_) {
                        signaling_->send(payload);
                    }
                    };

                signaling_->onOpen([this, ident]() {
                    LogI("websocket connected");
                    if (signaling_) {
                        signaling_->send(R"({"type":"hello"})");
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
                        LogI("start_webrtc session=" + sessionId + " mode=" + SessionModeName(sessionMode));
                        sessionBridge_.SetActiveSession(sessionId);
                        if (sessionMode == SessionMode::Console) {
                            const std::string technicianName = msg.value("technician_name", msg.value("technician", std::string("Technician")));
                            presenceController_.ShowConnected(technicianName, true);
                            StartPresenceBanner(sessionId, technicianName);
                        }
                        else {
                            presenceController_.Hide();
                            StopPresenceBanner(sessionId);
                        }
                        auto iceServers = parseIceServers(msg);
                        StartStreamerSession(sessionId, iceServers, sendFn, width, height, fps, bitrateKbps, sessionMode);
                        // Keep the portal responsive without running expensive WMI/PowerShell
                        // collectors during live remote control. The WebSocket/session state
                        // is already live; full inventory can still be requested manually.
                        LogI("remote session active; full scheduled inventory throttled during stream session=" + sessionId);
                        FlushBridgeOutgoing();
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "webrtc_answer" || type == "ice_candidate" || type == "answer") {
                        std::lock_guard<std::mutex> lock(sessionsMu_);
                        auto it = sessions_.find(sessionId);
                        if (it != sessions_.end() && it->second->sender) {
                            it->second->sender->handleSignalingMessage(text);
                        }
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

                        const bool sent = SendChatToOverlay(sessionId, displayName, body,
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
                        if (IsSessionMode(sessionId, SessionMode::Console)) {
                            LogW("backstage_start rejected for locked console session=" + sessionId);
                        }
                        else {
                            HandleBackstageStart(sessionId);
                        }
                        FlushBridgeOutgoing();
                        return;
                    }

                    if (type == "backstage_stop" || type == "console_start") {
                        LogI("backstage_stop/console_start session=" + sessionId);
                        if (IsSessionMode(sessionId, SessionMode::Backstage)) {
                            LogW("console_start/backstage_stop rejected for locked backstage session=" + sessionId);
                        }
                        else {
                            HandleBackstageStop(sessionId);
                        }
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
                    LogW("websocket closed; stopping active remote sessions and streamer helpers");
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
                    stop_.store(true);
                    });

                signaling_->connect();
                StartInventoryLoop(ident);

                const std::string enablePatchWorker = ReadConfigValue("HI5_ENABLE_PATCH_WORKER");
                const bool patchWorkerEnabled =
                    enablePatchWorker == "1" ||
                    enablePatchWorker == "true" ||
                    enablePatchWorker == "TRUE" ||
                    enablePatchWorker == "yes" ||
                    enablePatchWorker == "YES";

                if (patchWorkerEnabled) {
                    PatchWorkerConfig patchConfig;
                    patchConfig.enabled = true;
                    patchConfig.apiBaseUrl = ReadConfigValue("HI5_PATCH_API_BASE_URL");
                    if (patchConfig.apiBaseUrl.empty()) {
                        patchConfig.apiBaseUrl = "https://api.hi5central.com";
                    }
                    patchConfig.apiKey = ReadConfigValue("HI5_PATCH_API_KEY");
                    patchConfig.deviceId = ident.deviceId;
                    patchConfig.pollSeconds = ReadConfigInt("HI5_PATCH_POLL_SECONDS", 300, 30, 3600);
                    LogI("[patch] worker enabled");
                    patchWorker_.Start(patchConfig);
                } else {
                    LogI("[patch] worker disabled by default; set HI5_ENABLE_PATCH_WORKER=1 to enable");
                }

                auto nextUiCleanupSweep = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!stop_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    const auto now = std::chrono::steady_clock::now();
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

                patchWorker_.Stop();
                StopInventoryLoop();
                StopChatOverlays();
                StopPresenceBanners();
                CleanupOrphanUiHelperProcesses();
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
    # Remove control characters that can break downstream JSON display/parsing.
    return ($s -replace '[\x00-\x08\x0B\x0C\x0E-\x1F]', '')
}

$items = Get-CimInstance Win32_Service | Sort-Object DisplayName | ForEach-Object {
    [pscustomobject]@{
        name = ConvertTo-Hi5SafeString $_.Name
        display_name = ConvertTo-Hi5SafeString $_.DisplayName
        state = ConvertTo-Hi5SafeString $_.State
        status = ConvertTo-Hi5SafeString $_.Status
        start_mode = ConvertTo-Hi5SafeString $_.StartMode
        start_name = ConvertTo-Hi5SafeString $_.StartName
        process_id = [int]$_.ProcessId
        path_name = ConvertTo-Hi5SafeString $_.PathName
        description = ConvertTo-Hi5SafeString $_.Description
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

function Add-Hi5Candidate([System.Collections.ArrayList]$list, [string]$strategy, [string]$command) {
    if ([string]::IsNullOrWhiteSpace($command)) { return }
    foreach ($item in $list) { if ($item.command -eq $command) { return } }
    [void]$list.Add([pscustomobject]@{ strategy=$strategy; command=$command })
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
    for ($probe = 0; $probe -lt 12; $probe++) {
        Start-Sleep -Seconds $(if ($probe -eq 0) { 2 } else { 3 })
        $stillInstalled = Test-Hi5StillInstalled
        if (-not $stillInstalled) { break }
    }

    return [pscustomobject]@{
        strategy = $candidate.strategy
        exit_code = $exitCode
        timed_out = $timedOut
        still_installed = $stillInstalled
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
                    if (ctx.sessionMode == SessionMode::Console) {
                        LogW("backstage_start rejected for locked console session=" + ctx.sessionId);
                    }
                    else {
                        HandleBackstageStart(ctx.sessionId);
                    }
                    return;
                }

                if (kind == "backstage_stop" || kind == "console_start" ||
                    type == "backstage_stop" || type == "console_start") {
                    LogI("backstage_stop/console_start via datachannel session=" + ctx.sessionId);
                    if (ctx.sessionMode == SessionMode::Backstage) {
                        LogW("console_start/backstage_stop rejected for locked backstage session=" + ctx.sessionId);
                    }
                    else {
                        HandleBackstageStop(ctx.sessionId);
                    }
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
                        const bool sasSent = TrySendSecureAttentionSequenceFromService(ctx.sessionId);
                        LogI("ctrl_alt_del service command requested session=" + ctx.sessionId +
                            " service_sas_attempted=" + std::string(sasSent ? "1" : "0") +
                            " kind=" + kind + " type=" + type);
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

            bool LaunchNormalStreamer(SessionContext& ctx) {
                const DWORD consoleSession = ActiveConsoleSessionId();
                if (consoleSession == 0xFFFFFFFF) {
                    LogW("launch normal streamer skipped, no active console session=" + ctx.sessionId);
                    return false;
                }

                ctx.activeConsoleSessionId = consoleSession;
                ctx.lastSeenConsoleSessionId = consoleSession;

                char exePath[MAX_PATH]{};
                GetModuleFileNameA(nullptr, exePath, MAX_PATH);

                ResetEvent(ctx.normalStopEvent);

                std::string cmdLine =
                    "--mode streamer"
                    " --session " + ctx.sessionId +
                    " --shmem " + ctx.normalShmemName +
                    " --input-pipe " + ctx.normalInputPipeName +
                    " --stop-event " + ctx.normalStopEventName +
                    " --chat-pipe " + ctx.chatPipeName +
                    " --fps " + std::to_string(ctx.fps) +
                    " --display " + std::to_string(ctx.displayIndex);

                LogI("launch normal streamer [NORMAL_SHMEM_EXPECTED_NO_UAC] session=" + ctx.sessionId + " cmd=" + cmdLine);

                ctx.normalStreamerProcess = LaunchInElevatedDefaultSession(std::string(exePath), cmdLine);
                if (!ctx.normalStreamerProcess) {
                    LogE("launch normal streamer FAILED session=" + ctx.sessionId);
                    return false;
                }

                LogI("launch normal streamer ok session=" + ctx.sessionId +
                    " pid=" + std::to_string(GetProcessId(ctx.normalStreamerProcess)) +
                    " console=" + SessionIdToString(ctx.activeConsoleSessionId));

                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                SyncChatStateToContext(ctx);
                return true;
            }

            bool LaunchSecureStreamer(SessionContext& ctx) {
                if (ctx.secureStreamerProcess) {
                    return true;
                }

                char exePath[MAX_PATH]{};
                GetModuleFileNameA(nullptr, exePath, MAX_PATH);

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

                ctx.secureStreamerProcess = LaunchOnSecureDesktop(std::string(exePath), cmdLine);
                if (!ctx.secureStreamerProcess) {
                    LogE("launch secure streamer FAILED session=" + ctx.sessionId);
                    ctx.secureLaunchInProgress = false;
                    return false;
                }

                ctx.secureReady = false;
                ctx.secureLaunchInProgress = true;
                ctx.lastSecureLaunchAttempt = std::chrono::steady_clock::now();

                LogI("launch secure streamer ok session=" + ctx.sessionId +
                    " pid=" + std::to_string(GetProcessId(ctx.secureStreamerProcess)));

                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                SyncChatStateToContext(ctx);
                return true;
            }


            bool LaunchBackstageHost(SessionContext& ctx) {
                if (ctx.backstageHostProcess) {
                    return true;
                }

                char exePath[MAX_PATH]{};
                GetModuleFileNameA(nullptr, exePath, MAX_PATH);

                ResetEvent(ctx.normalStopEvent);

                std::string cmdLine =
                    "--mode backstage-host"
                    " --session " + ctx.sessionId +
                    " --shmem " + ctx.normalShmemName +
                    " --input-pipe " + ctx.normalInputPipeName +
                    " --stop-event " + ctx.normalStopEventName +
                    " --fps 15"
                    " --width 1280"
                    " --height 720";

                LogI("launch backstage host session=" + ctx.sessionId + " cmd=" + cmdLine);

                ctx.backstageHostProcess = LaunchInElevatedDefaultSession(std::string(exePath), cmdLine);
                if (!ctx.backstageHostProcess) {
                    LogE("launch backstage host FAILED session=" + ctx.sessionId);
                    return false;
                }

                ctx.backstageMode = true;
                ctx.activeMode = DesktopMode::Normal;
                ctx.lastNormalFrameAt = {};
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

            bool IsSessionMode(const std::string& sessionId, SessionMode expected) {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                auto it = sessions_.find(sessionId);
                if (it == sessions_.end()) {
                    return false;
                }
                return it->second->sessionMode == expected;
            }

            void HandleBackstageStart(const std::string& sessionId) {
                std::lock_guard<std::mutex> lock(sessionsMu_);
                auto it = sessions_.find(sessionId);
                if (it == sessions_.end()) {
                    LogW("backstage_start ignored, session not found=" + sessionId);
                    return;
                }
                SessionContext& ctx = *it->second;
                ctx.backstageMode = true;
                StopNormalStreamer(ctx);
                StopSecureStreamer(ctx);
                ctx.uacRequested = false;
                ctx.secureStateAnnounced = false;
                ctx.handoffStateAnnounced = false;
                LaunchBackstageHost(ctx);
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
                    return;
                }
                if (ctx.backstageHostProcess) {
                    StopBackstageHost(ctx);
                }
                LaunchNormalStreamer(ctx);
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

                ctx.secureReady = false;
                ctx.secureLaunchInProgress = false;
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

                ctx.lastNormalFrameAt = {};
            }

            void StartStreamerSession(const std::string& sessionId,
                const std::vector<std::string>& iceServers,
                const WebRtcSender::SignalSendFn& sendFn,
                int width,
                int height,
                int fps,
                int bitrateKbps,
                SessionMode sessionMode) {
                StopSession(sessionId);

                auto ctx = std::make_unique<SessionContext>();
                ctx->sessionId = sessionId;
                ctx->iceServers = iceServers;
                ctx->displayIndex = 0;
                ctx->fps = fps;
                ctx->activeMode = DesktopMode::Normal;
                ctx->sessionMode = sessionMode;
                ctx->backstageMode = sessionMode == SessionMode::Backstage;
                ctx->uacRequested = false;
                ctx->secureReady = false;
                ctx->secureLaunchInProgress = false;
                ctx->activeConsoleSessionId = ActiveConsoleSessionId();
                ctx->lastSeenConsoleSessionId = ctx->activeConsoleSessionId;
                ctx->lastConsoleSessionPoll = std::chrono::steady_clock::now();

                const std::string prefix = sessionId.substr(0, std::min<size_t>(16, sessionId.size()));

                ctx->normalShmemName = "Global\\Hi5Stream_" + prefix;
                ctx->secureShmemName = "Global\\Hi5Stream_UAC_" + prefix;
                ctx->normalInputPipeName = "Global\\Hi5Input_" + prefix;
                ctx->secureInputPipeName = "Global\\Hi5Input_UAC_" + prefix;
                ctx->normalStopEventName = "Global\\Hi5Stop_" + prefix;
                ctx->secureStopEventName = "Global\\Hi5Stop_UAC_" + prefix;
                ctx->chatPipeName = "\\\\.\\pipe\\Hi5Chat_" + prefix;

                LogI("initial console session session=" + sessionId +
                    " console=" + SessionIdToString(ctx->activeConsoleSessionId));
                LogI("chat pipe name session=" + sessionId + " pipe=" + ctx->chatPipeName);

                if (!ctx->normalShmem.CreateProducer(ctx->normalShmemName)) {
                    LogE("create normal shmem FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                    return;
                }

                if (!ctx->secureShmem.CreateProducer(ctx->secureShmemName)) {
                    LogE("create secure shmem FAILED session=" + sessionId +
                        " err=" + std::to_string(GetLastError()));
                    ctx->normalShmem.Close();
                    return;
                }

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
                    ctx->normalShmem.Close();
                    return;
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

                ctx->sender = std::make_unique<WebRtcSender>(
                    sessionId,
                    iceServers,
                    sendFn,
                    width,
                    height,
                    fps,
                    bitrateKbps,
                    WebRtcSender::Mode::ExternalFeed
                );

                ctx->sender->setConnectionClosedHandler([this, sid = sessionId](const std::string& reason) {
                    LogW("WebRTC connection closed/failed session=" + sid + " reason=" + reason + "; scheduling cleanup");
                    std::thread([this, sid]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        StopSession(sid);
                        CleanupOrphanStreamerProcesses(sid);
                        }).detach();
                    });

                ctx->sender->setInputEventHandler([this, raw = ctx.get()](const json& msg) {
                    DispatchInputToPipe(*raw, msg);
                    });

                ctx->sender->start();

                const bool launched = (sessionMode == SessionMode::Backstage)
                    ? LaunchBackstageHost(*ctx)
                    : LaunchNormalStreamer(*ctx);

                if (!launched) {
                    StopPresenceBanner(sessionId);
                    StopChatOverlay(sessionId);
                    ctx->sender->stop();
                    ctx->sender.reset();
                    CloseHandle(ctx->secureStopEvent);
                    CloseHandle(ctx->normalStopEvent);
                    ctx->secureStopEvent = nullptr;
                    ctx->normalStopEvent = nullptr;
                    ctx->secureInputPipe.Close();
                    ctx->normalInputPipe.Close();
                    ctx->secureShmem.Close();
                    ctx->normalShmem.Close();
                    return;
                }

                ctx->framePumpThread = std::thread([this, raw = ctx.get()]() {
                    FramePumpLoop(*raw);
                    });

                {
                    std::lock_guard<std::mutex> lock(sessionsMu_);
                    sessions_[sessionId] = std::move(ctx);
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
                ctx.lastNormalLaunchAttempt = std::chrono::steady_clock::now();
                ctx.lastSecureLaunchAttempt = {};
                ctx.lastConsoleSessionPoll = std::chrono::steady_clock::now();

                while (!ctx.framePumpStop.load()) {
                    const auto now = std::chrono::steady_clock::now();

                    // Detect console-session churn, but do not block secure/winlogon bridging.
                    // Secure must be allowed to start immediately during sign-out, otherwise the
                    // viewer sits black at Winlogon.
                    if (now - ctx.lastConsoleSessionPoll >= std::chrono::milliseconds(500)) {
                        ctx.lastConsoleSessionPoll = now;
                        const DWORD currentConsole = ActiveConsoleSessionId();

                        if (currentConsole != ctx.lastSeenConsoleSessionId) {
                            LogW("console switch detected session=" + ctx.sessionId +
                                " old_seen=" + SessionIdToString(ctx.lastSeenConsoleSessionId) +
                                " new_seen=" + SessionIdToString(currentConsole));
                            ctx.lastSeenConsoleSessionId = currentConsole;
                            ctx.lastConsoleSwitchDetected = now;
                            ctx.consoleSwitchInProgress = true;
                        }

                        if (ctx.consoleSwitchInProgress &&
                            now - ctx.lastConsoleSwitchDetected >= std::chrono::milliseconds(1500)) {
                            ctx.consoleSwitchInProgress = false;

                            if (currentConsole != 0xFFFFFFFF && currentConsole != ctx.activeConsoleSessionId) {
                                LogW("console session stable, relaunch normal session=" + ctx.sessionId +
                                    " old=" + SessionIdToString(ctx.activeConsoleSessionId) +
                                    " new=" + SessionIdToString(currentConsole));

                                SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                ctx.handoffStateAnnounced = true;
                                ctx.activeConsoleSessionId = currentConsole;

                                StopNormalStreamer(ctx);
                                ctx.lastNormalLaunchAttempt = now - std::chrono::seconds(3);
                            }
                        }
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
                            ctx.lastNormalFrameAt = {};

                            if (!ctx.handoffStateAnnounced) {
                                SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                ctx.handoffStateAnnounced = true;
                            }

                            // Start Menu sign-out can kill the normal desktop before it sets UACActive.
                            // Do NOT throttle this path; otherwise the viewer stays black at Winlogon.
                            if (!ctx.secureStreamerProcess && !ctx.secureLaunchInProgress) {
                                LaunchSecureStreamer(ctx);
                            }
                        }
                    }

                    // Throttle normal desktop relaunches during session churn. This prevents
                    // process spam when switching between multiple users, but still allows the
                    // secure/winlogon helper to start immediately on sign-out.
                    if (!ctx.backstageMode &&
                        !ctx.normalStreamerProcess &&
                        !ctx.consoleSwitchInProgress &&
                        ActiveConsoleSessionId() != 0xFFFFFFFF &&
                        now - ctx.lastNormalLaunchAttempt >= std::chrono::seconds(3)) {
                        ctx.lastNormalLaunchAttempt = now;
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

                    if (ctx.secureStreamerProcess) {
                        const DWORD waitRc = WaitForSingleObject(ctx.secureStreamerProcess, 0);
                        if (waitRc == WAIT_OBJECT_0) {
                            DWORD exitCode = 0;
                            GetExitCodeProcess(ctx.secureStreamerProcess, &exitCode);
                            LogW("secure streamer exited session=" + ctx.sessionId +
                                " exitCode=" + std::to_string(exitCode));
                            CloseHandle(ctx.secureStreamerProcess);
                            ctx.secureStreamerProcess = nullptr;
                            ctx.secureReady = false;
                            ctx.secureLaunchInProgress = false;
                        }
                    }

                    if (now >= nextSecurePoll) {
                        if (ctx.backstageMode) {
                            ctx.uacRequested = false;
                            nextSecurePoll = now + std::chrono::milliseconds(250);
                        }
                        else {
                            ctx.uacRequested = ctx.normalInputPipe.GetUACActive();

                            if (ctx.uacRequested) {
                                if (!ctx.secureStateAnnounced) {
                                    SendSessionState(ctx.sessionId, "secure_desktop_entering");
                                    ctx.secureStateAnnounced = true;
                                }
                                if (!ctx.secureStreamerProcess && !ctx.secureLaunchInProgress) {
                                    LaunchSecureStreamer(ctx);
                                }
                            }

                            const bool normalFresh = ctx.lastNormalFrameAt.time_since_epoch().count() != 0 &&
                                (now - ctx.lastNormalFrameAt) <= std::chrono::milliseconds(800);

                            if (!ctx.uacRequested && ctx.secureStateAnnounced && !normalFresh) {
                                if (!ctx.handoffStateAnnounced) {
                                    SendSessionState(ctx.sessionId, "desktop_handoff_entering");
                                    ctx.handoffStateAnnounced = true;
                                }
                            }

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

                            nextSecurePoll = now + std::chrono::milliseconds(25);
                        }
                    }

                    I420Frame normalFrame;
                    I420Frame secureFrame;
                    I420Frame tmpFrame;
                    uint64_t normalTs = 0;
                    uint64_t secureTs = 0;
                    uint64_t tmpTs = 0;
                    bool gotNormal = false;
                    bool gotSecure = false;

                    while (ctx.normalShmem.ReadRawI420Frame(tmpFrame, tmpTs)) {
                        normalFrame = std::move(tmpFrame);
                        normalTs = tmpTs;
                        gotNormal = true;
                    }
                    if (gotNormal) {
                        ctx.lastNormalFrameAt = now;
                    }

                    if (ctx.secureStreamerProcess) {
                        while (ctx.secureShmem.ReadRawI420Frame(tmpFrame, tmpTs)) {
                            secureFrame = std::move(tmpFrame);
                            secureTs = tmpTs;
                            gotSecure = true;
                        }

                        if (gotSecure) {
                            ctx.lastSecureFrameAt = now;
                            if (!ctx.secureReady) {
                                ctx.secureReady = true;
                                ctx.secureLaunchInProgress = false;
                                ctx.activeMode = DesktopMode::Secure;
                                SendSessionState(ctx.sessionId, "secure_desktop_ready");
                                LogI("active mode -> SECURE session=" + ctx.sessionId);
                            }
                        }
                    }

                    bool sent = false;
                    const bool useSecure = ctx.uacRequested || (!gotNormal && ctx.secureStreamerProcess);

                    if (useSecure && gotSecure) {
                        ++framesForwarded;
                        const bool forceKf = ctx.activeMode != DesktopMode::Secure || !ctx.secureReady;
                        if (framesForwarded == 1 || (framesForwarded % 120) == 0) {
                            LogI("raw frame forwarded session=" + ctx.sessionId +
                                " frames=" + std::to_string(framesForwarded) +
                                " size=" + std::to_string(secureFrame.y.size() + secureFrame.u.size() + secureFrame.v.size()) +
                                " mode=secure force_kf=" + std::to_string(forceKf ? 1 : 0));
                        }
                        if (ctx.sender) {
                            ctx.sender->sendExternalRawI420(secureFrame, secureTs, forceKf);
                        }
                        sent = true;
                    }
                    else if (gotNormal) {
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
                            if (ctx.secureStreamerProcess) {
                                StopSecureStreamer(ctx);
                            }
                            ctx.activeMode = DesktopMode::Normal;
                            LogI("active mode -> NORMAL session=" + ctx.sessionId + " raw-source=1");
                        }

                        ++framesForwarded;
                        const bool forceKf = wasSecure || framesForwarded == 1;
                        if (framesForwarded == 1 || (framesForwarded % 120) == 0) {
                            LogI("raw frame forwarded session=" + ctx.sessionId +
                                " frames=" + std::to_string(framesForwarded) +
                                " size=" + std::to_string(normalFrame.y.size() + normalFrame.u.size() + normalFrame.v.size()) +
                                " mode=normal force_kf=" + std::to_string(forceKf ? 1 : 0));
                        }
                        if (ctx.sender) {
                            ctx.sender->sendExternalRawI420(normalFrame, normalTs, forceKf);
                        }
                        sent = true;
                    }

                    if (now >= nextDiagnosticsPoll) {
                        hi5::StreamStats stats{};
                        if (ctx.normalInputPipe.ReadStreamStats(lastNormalStatsSeq, stats)) {
                            lastNormalStatsSeq = stats.seq;
                            if (ctx.sender) {
                                ctx.sender->setExternalStreamHint(stats.streamMode, stats.targetFps, ctx.backstageMode, stats.secureDesktopActive != 0);
                            }
                            SendStreamDiagnostics(ctx.sessionId, "normal", stats, framesForwarded, ctx.activeMode, ctx.backstageMode);
                        }
                        if (ctx.secureInputPipe.ReadStreamStats(lastSecureStatsSeq, stats)) {
                            lastSecureStatsSeq = stats.seq;
                            if (ctx.sender) {
                                ctx.sender->setExternalStreamHint(stats.streamMode, stats.targetFps, ctx.backstageMode, stats.secureDesktopActive != 0);
                            }
                            SendStreamDiagnostics(ctx.sessionId, "secure", stats, framesForwarded, ctx.activeMode, ctx.backstageMode);
                        }
                        nextDiagnosticsPoll = now + std::chrono::seconds(1);
                    }

                    if (!sent) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }

                    if (now >= nextMonitorPoll) {
                        const int count = ctx.normalInputPipe.GetMonitorCount();
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
                CleanupOrphanUiHelperProcesses();

                std::unique_ptr<SessionContext> ctx;

                {
                    std::lock_guard<std::mutex> lock(sessionsMu_);
                    auto it = sessions_.find(sessionId);
                    if (it == sessions_.end()) {
                        LogW("StopSession: no active stream context for session=" + sessionId + "; UI cleanup already completed, cleaning possible streamer orphan");
                        CleanupOrphanStreamerProcesses(sessionId);
                        return;
                    }
                    ctx = std::move(it->second);
                    sessions_.erase(it);
                }

                ctx->framePumpStop.store(true);

                if (ctx->normalStopEvent) SetEvent(ctx->normalStopEvent);
                if (ctx->secureStopEvent) SetEvent(ctx->secureStopEvent);

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

                CleanupOrphanStreamerProcesses(sessionId);

                if (ctx->normalStopEvent) {
                    CloseHandle(ctx->normalStopEvent);
                    ctx->normalStopEvent = nullptr;
                }

                if (ctx->secureStopEvent) {
                    CloseHandle(ctx->secureStopEvent);
                    ctx->secureStopEvent = nullptr;
                }

                if (ctx->sender) {
                    ctx->sender->stop();
                    ctx->sender.reset();
                }

                if (ctx->chatPipeServer) {
                    ctx->chatPipeServer->Stop();
                    ctx->chatPipeServer.reset();
                }

                ctx->secureInputPipe.Close();
                ctx->normalInputPipe.Close();
                ctx->secureShmem.Close();
                ctx->normalShmem.Close();

                LogI("session stopped=" + sessionId);
            }

        private:
            std::string serviceName_;

            std::mutex terminalsMutex_;
            std::map<std::string, std::shared_ptr<TerminalSession>> terminals_;

            std::atomic<bool> stop_{ false };

            SessionBridge sessionBridge_;
            AgentPresenceController presenceController_;
            ChatController chatController_;
            std::unique_ptr<SignalingClient> signaling_;
            std::thread inventoryThread_;
            PatchWorker patchWorker_;

            std::mutex sessionsMu_;
            std::unordered_map<std::string, std::unique_ptr<SessionContext>> sessions_;
        };

        static std::unique_ptr<Worker> g_worker;

        void WINAPI ServiceCtrlHandler(DWORD ctrlCode) {
            switch (ctrlCode) {
            case SERVICE_CONTROL_STOP:
            case SERVICE_CONTROL_SHUTDOWN:
                LogI("service control -> stop/shutdown");
                SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 4000);
                g_stopRequested.store(true);
                if (g_worker) {
                    g_worker->RequestStop();
                }
                return;
            default:
                return;
            }
        }

        void WINAPI ServiceMainThunk(DWORD argc, LPSTR* argv) {
            if (argc > 0 && argv && argv[0] && argv[0][0]) {
                g_serviceName = argv[0];
            }
            LogI("ServiceMainThunk enter service=" + g_serviceName);
            g_statusHandle = RegisterServiceCtrlHandlerA(g_serviceName.c_str(), ServiceCtrlHandler);
            if (!g_statusHandle) {
                LogE("RegisterServiceCtrlHandler failed err=" + std::to_string(GetLastError()));
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

