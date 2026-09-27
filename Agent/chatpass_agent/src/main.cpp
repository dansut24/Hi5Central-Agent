#include "signaling_client.h"
#include "connect_file_browser.h"
#include "webrtc_sender.h"
#include "agent_identity.h"
#include "agent_version.h"
#include "service/service_main.h"
#include "util/log.h"
#include "platform/platform.h"
#include "platform/screen_provider.h"
#include "platform/input_provider.h"
#include "platform/inventory_provider.h"
#ifdef _WIN32
#include "ui/native_connect_window.h"
#endif

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using json = nlohmann::json;

namespace hi5 {
    int RunStreamerMain(int argc, char** argv);
    int RunChatOverlayMain(int argc, char** argv);
    int RunNativeChatMain(int argc, char** argv);
    int RunNativeBannerMain(int argc, char** argv);
    int RunNativeTrayMain(int argc, char** argv);
    int RunBackstageHostMain(int argc, char** argv);
    int RunBackstageBrowserMain(int argc, char** argv);
    int MaybeRunCefSubprocess(int argc, char** argv);
}

static std::atomic<bool> g_running{ true };

static std::string argValue(int argc, char** argv, const std::string& name, const std::string& fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return std::string(argv[i + 1]);
    }
    return fallback;
}

static int RunSasHelperMain(int argc, char** argv) {
#ifdef _WIN32
    const std::string sessionId = argValue(argc, argv, "--session", "unknown");
    LogInfo("[sas-helper] start session=" + sessionId);

    using SendSasFn = void (WINAPI*)(BOOL);
    HMODULE sas = LoadLibraryW(L"sas.dll");
    if (!sas) {
        LogError("[sas-helper] LoadLibrary(sas.dll) failed err=" + std::to_string(GetLastError()));
        return 2;
    }

    auto fn = reinterpret_cast<SendSasFn>(GetProcAddress(sas, "SendSAS"));
    if (!fn) {
        const DWORD err = GetLastError();
        FreeLibrary(sas);
        LogError("[sas-helper] GetProcAddress(SendSAS) failed err=" + std::to_string(err));
        return 3;
    }

    LogInfo("[sas-helper] calling SendSAS(TRUE) session=" + sessionId);
    fn(TRUE);
    Sleep(150);
    FreeLibrary(sas);
    LogInfo("[sas-helper] finished session=" + sessionId);
    return 0;
#else
    (void)argc; (void)argv;
    return 1;
#endif
}

static void signalHandler(int) {
    g_running = false;
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

static void LogPlatformInfo() {
    const auto platform = hi5::getPlatformInfo();

    std::cout
        << "[INFO] [platform] os=" << platform.osName
        << " version=" << platform.osVersion
        << " arch=" << platform.architecture
        << " elevated=" << (platform.isElevated ? "true" : "false")
        << " desktop=" << (platform.hasDesktopSession ? "true" : "false")
        << " server=" << (platform.isServer ? "true" : "false")
        << std::endl;

    LogInfo(
        "[platform] os=" + platform.osName +
        " version=" + platform.osVersion +
        " arch=" + platform.architecture +
        " elevated=" + std::string(platform.isElevated ? "true" : "false") +
        " desktop=" + std::string(platform.hasDesktopSession ? "true" : "false") +
        " server=" + std::string(platform.isServer ? "true" : "false")
    );

    auto screenProvider = hi5::CreateScreenCaptureProvider();
    auto inputProvider = hi5::CreateInputProvider();
    auto inventoryProvider = hi5::CreateInventoryProvider();

    const auto screenCaps = screenProvider->capabilities();
    const auto inputCaps = inputProvider->capabilities();
    const auto inventoryCaps = inventoryProvider->capabilities();

    LogInfo(
        "[providers] screen=" + screenCaps.providerName +
        " available=" + std::string(screenCaps.available ? "true" : "false") +
        " input=" + inputCaps.providerName +
        " available=" + std::string(inputCaps.available ? "true" : "false") +
        " inventory=" + inventoryCaps.providerName +
        " available=" + std::string(inventoryCaps.available ? "true" : "false")
    );
}

static std::string ConnectTicketFromArgs(int argc, char** argv) {
    const std::string explicitTicket = argValue(argc, argv, "--connect-ticket");
    if (!explicitTicket.empty()) return explicitTicket;
    if (argc <= 0 || !argv || !argv[0]) return {};

    try {
        const std::string stem = std::filesystem::path(argv[0]).stem().string();
        const std::string prefix = "Hi5CentralConnect-";
        if (stem.rfind(prefix, 0) == 0 && stem.size() > prefix.size()) {
            return stem.substr(prefix.size());
        }
    }
    catch (...) {
    }
    return {};
}

static std::string ConnectHostName() {
#ifdef _WIN32
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(sizeof(name));
    if (GetComputerNameA(name, &size) && size > 0) return std::string(name, size);
#endif
    return "Customer computer";
}

#ifdef _WIN32
static std::wstring ConnectWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), count);
    return out;
}

static std::wstring ConnectExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    buffer.resize(count);
    return buffer;
}

static bool ConnectPromptCustomer(const std::wstring& title, const std::wstring& message) {
    return MessageBoxW(
        nullptr,
        message.c_str(),
        title.c_str(),
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND
    ) == IDYES;
}

static bool ConnectSetRestartResume(bool enabled, const std::string& ticket = {}) {
    HKEY key = nullptr;
    const LONG open = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (open != ERROR_SUCCESS || !key) return false;

    constexpr wchar_t valueName[] = L"Hi5CentralConnectResume";
    LONG result = ERROR_SUCCESS;
    if (!enabled) {
        result = RegDeleteValueW(key, valueName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    } else {
        const std::wstring exe = ConnectExecutablePath();
        if (exe.empty()) {
            RegCloseKey(key);
            return false;
        }
        const std::wstring wideTicket = ConnectWide(ticket);
        if (wideTicket.empty()) {
            RegCloseKey(key);
            return false;
        }
        const std::wstring command =
            L"\"" + exe + L"\" --connect-ticket \"" + wideTicket +
            L"\" --connect-held-resume";
        result = RegSetValueExW(
            key, valueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static bool ConnectLaunchElevatedCopy(const std::string& ticket) {
    const std::wstring exe = ConnectExecutablePath();
    const std::wstring wideTicket = ConnectWide(ticket);
    if (exe.empty() || wideTicket.empty()) return false;
    const std::wstring parameters =
        L"--connect-ticket \"" + wideTicket + L"\" --connect-elevated-handoff";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = nullptr;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) return false;
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}
#endif

static int RunConnectHost(const std::string& ticket) {
    if (ticket.empty()) {
        std::cerr << "[connect] This support download is missing its one-time ticket.\n";
        std::cerr << "[connect] Return to https://connect.hi5central.com and download it again.\n";
        return 2;
    }

    g_running = true;
#ifdef _WIN32
    if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
#endif
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    rtc::InitLogger(rtc::LogLevel::Info);

#ifdef _WIN32
    SetConsoleTitleW(L"Hi5Central Connect - Remote Support");
#endif

    std::cout << "\n";
    std::cout << "============================================================\n";
    std::cout << " Hi5Central Connect - one-time remote support\n";
    std::cout << " Close this window at any time to end remote access.\n";
    std::cout << " No managed Hi5Central Agent is being installed.\n";
    std::cout << "============================================================\n\n";
    std::cout << "[connect] Preparing secure support session...\n";

    constexpr int width = 0;
    constexpr int height = 0;
    constexpr int fps = 30;
    constexpr int bitrateKbps = 6000;
    const std::string wsUrl =
        "wss://rmm.hi5central.com/connect/host/ws?ticket=" + ticket;

#ifdef _WIN32
    hi5::NativeConnectWindow supportWindow;
    std::atomic<bool> supportUiStarted{ false };
    std::atomic<bool> fileAccessGranted{ false };
    std::atomic<bool> elevationHandoffRequested{ false };
    std::atomic<bool> sessionExplicitlyEnded{ false };
    std::string connectSessionId;
    const bool connectElevated = hi5::getPlatformInfo().isElevated;
#endif

    try {
        SignalingClient signaling(wsUrl);
        std::mutex sessionsMu;
        std::unordered_map<std::string, std::unique_ptr<WebRtcSender>> sessions;

        auto sendFn = [&signaling](const std::string& payload) {
            signaling.send(payload);
        };

        std::atomic<bool> signalingConnected{ false };
        std::atomic<bool> reconnectRequested{ false };

#ifdef _WIN32
        hi5::ConnectFileBrowser connectFiles([&signaling](const json& payload) {
            signaling.send(payload.dump());
        });
        auto startSupportUi = [&](const std::string& sessionId,
            const std::string& technician,
            const std::string& organisation) {
            connectSessionId = sessionId;
            if (supportUiStarted.exchange(true)) {
                supportWindow.SetIdentity(technician, organisation);
                return;
            }

            if (!supportWindow.Start(
                sessionId,
                technician,
                organisation,
                [&](const std::string& body) {
                    signaling.send(json{
                        {"type", "chat_message"},
                        {"session_id", connectSessionId},
                        {"sender", "user"},
                        {"display_name", "Customer"},
                        {"body", body},
                        {"message", body},
                        {"text", body}
                    }.dump());
                },
                [&]() {
                    sessionExplicitlyEnded.store(true);
                    if (!connectSessionId.empty()) {
                        signaling.send(json{
                            {"type", "session_ended"},
                            {"session_id", connectSessionId},
                            {"reason", "customer_ended_session"}
                        }.dump());
                    }
                    ConnectSetRestartResume(false, ticket);
                    g_running = false;
                })) {
                LogWarn("[connect-ui] failed to create centred customer support window session=" + sessionId);
                supportUiStarted.store(false);
                signaling.send(json{
                    {"type", "host_closed"},
                    {"session_id", connectSessionId}
                }.dump());
                g_running = false;
                return;
            }

            LogInfo("[connect-ui] centred customer support window started session=" + sessionId);
        };
#endif

        signaling.onOpen([&]() {
            signalingConnected.store(true);
            reconnectRequested.store(false);
            std::cout << "[connect] Secure connection established. Waiting for technician...\n";
            signaling.send(json{
                {"type", "connect_hello"},
                {"host_name", ConnectHostName()},
                {"platform", "Windows"},
                {"version", hi5::kAgentVersion},
#ifdef _WIN32
                {"elevated", connectElevated}
#else
                {"elevated", false}
#endif
            }.dump());
#ifdef _WIN32
            if (supportUiStarted.load()) {
                supportWindow.SetConnectionState("Waiting for technician", false);
            }
#endif
        });

        signaling.onMessage([&](const std::string& text) {
            const auto msg = json::parse(text, nullptr, false);
            if (msg.is_discarded()) return;

            const std::string type = msg.value("type", "");
            std::string sessionId = msg.value("session_id", "");
            if (sessionId.empty()) sessionId = msg.value("session", "");

            if (type == "connect_ready") {
                const std::string technician = msg.value("technician_name", std::string("Hi5Central technician"));
                const std::string organisation = msg.value("organisation_name", std::string("Hi5Central"));
                const std::string heldUntil = msg.value("held_until", std::string());
                std::cout << "[connect] Support request verified.\n";
                std::cout << "[connect] Organisation: " << organisation << "\n";
                std::cout << "[connect] Technician: " << technician << "\n";
                std::cout << "[connect] Waiting for the technician to open the remote session...\n";
#ifdef _WIN32
                startSupportUi(sessionId, technician, organisation);
                if (!heldUntil.empty() && supportUiStarted.load()) {
                    supportWindow.SetConnectionState("Session on hold", false);
                }
#endif
                return;
            }

            if (type == "start_webrtc") {
                if (sessionId.empty()) return;
                std::cout << "[connect] Technician connected. Starting remote desktop...\n";
                auto iceServers = parseIceServers(msg);
                auto sender = std::make_unique<WebRtcSender>(
                    sessionId, iceServers, sendFn,
                    width, height, fps, bitrateKbps
                );
                sender->start();
                signaling.send(sender->buildMonitorInfoMessage().dump());
#ifdef _WIN32
                if (supportUiStarted.load()) supportWindow.SetConnectionState("Connected", true);
#endif
                std::lock_guard<std::mutex> lock(sessionsMu);
                auto prior = sessions.find(sessionId);
                if (prior != sessions.end()) prior->second->stop();
                sessions[sessionId] = std::move(sender);
                return;
            }

            if (type == "webrtc_answer" || type == "ice_candidate" ||
                type == "answer" || type == "candidate" || type == "viewer_answer") {
                std::lock_guard<std::mutex> lock(sessionsMu);
                auto it = sessions.find(sessionId);
                if (it != sessions.end()) it->second->handleSignalingMessage(text);
                return;
            }

            if (type == "switch_monitor") {
                const int requested = msg.value("monitor_index", 0);
                std::lock_guard<std::mutex> lock(sessionsMu);
                auto it = sessions.find(sessionId);
                if (it != sessions.end() && it->second->switchMonitor(requested)) {
                    signaling.send(it->second->buildMonitorInfoMessage().dump());
                }
                return;
            }

            if (type == "input_event") {
                std::lock_guard<std::mutex> lock(sessionsMu);
                auto it = sessions.find(sessionId);
                if (it != sessions.end()) it->second->handleInputEvent(msg);
                return;
            }

#ifdef _WIN32
            if (type == "connect_permission_request") {
                const std::string permission = msg.value("permission", std::string());
                bool approved = false;
                std::string reason;

                if (permission == "files") {
                    if (fileAccessGranted.load()) {
                        approved = true;
                    } else {
                        approved = ConnectPromptCustomer(
                            L"Hi5Central Connect - File access",
                            L"Your technician is requesting access to browse and transfer files on this computer.\n\n"
                            L"If you allow this, they can view folders and upload, download, rename, create or delete files for this support session.\n\n"
                            L"Allow file access?"
                        );
                        fileAccessGranted.store(approved);
                        reason = approved ? "customer_approved" : "customer_denied";
                    }
                }
                else if (permission == "elevation") {
                    if (connectElevated) {
                        approved = true;
                        reason = "already_elevated";
                    } else {
                        const bool customerApproved = ConnectPromptCustomer(
                            L"Hi5Central Connect - Administrator access",
                            L"Your technician is requesting administrator access for this support session.\n\n"
                            L"Windows will show a User Account Control prompt next. Only approve that Windows prompt if you want the technician to continue with administrator access.\n\n"
                            L"Continue?"
                        );
                        if (!customerApproved) {
                            reason = "customer_denied";
                        } else if (ConnectLaunchElevatedCopy(ticket)) {
                            approved = true;
                            reason = "elevation_started";
                            elevationHandoffRequested.store(true);
                            if (supportUiStarted.load()) {
                                supportWindow.SetConnectionState("Elevating session...", false);
                            }
                        } else {
                            reason = "uac_cancelled_or_failed";
                        }
                    }
                }
                else {
                    reason = "unsupported_permission";
                }

                signaling.send(json{
                    {"type", "connect_permission_response"},
                    {"session_id", sessionId},
                    {"permission", permission},
                    {"approved", approved},
                    {"reason", reason},
                    {"elevated", connectElevated}
                }.dump());
                return;
            }

            if (type == "connect_hold_request") {
                const int requestedMinutes = std::clamp(
                    msg.value("duration_minutes", 24 * 60), 30, 24 * 60);
                const bool approved = ConnectPromptCustomer(
                    L"Hi5Central Connect - Keep session available",
                    L"Your technician would like to keep this support session available so they can return later.\n\n"
                    L"If you allow this, Hi5Central Connect may start once after your next Windows sign-in so the same support session can reconnect after a restart. The session will still expire automatically.\n\n"
                    L"Allow this session to remain available?"
                );
                const bool restartRegistered = approved && ConnectSetRestartResume(true, ticket);
                if (approved) {
                    std::lock_guard<std::mutex> lock(sessionsMu);
                    auto it = sessions.find(sessionId);
                    if (it != sessions.end()) {
                        it->second->stop();
                        sessions.erase(it);
                    }
                }
                signaling.send(json{
                    {"type", "connect_hold_response"},
                    {"session_id", sessionId},
                    {"approved", approved},
                    {"duration_minutes", requestedMinutes},
                    {"restart_registered", restartRegistered}
                }.dump());
                if (approved && supportUiStarted.load()) {
                    supportWindow.SetConnectionState("Session on hold", false);
                }
                return;
            }

            if (type == "connect_hold_released") {
                ConnectSetRestartResume(false, ticket);
                if (supportUiStarted.load()) {
                    supportWindow.SetConnectionState("Waiting for technician", false);
                }
                return;
            }

            if (type.rfind("remote_file_", 0) == 0) {
                if (!fileAccessGranted.load()) {
                    signaling.send(json{
                        {"type", "connect_permission_response"},
                        {"session_id", sessionId},
                        {"permission", "files"},
                        {"approved", false},
                        {"reason", "permission_required"}
                    }.dump());
                    return;
                }
                connectFiles.Handle(sessionId, msg);
                return;
            }

            if (type == "chat_message") {
                hi5::ConnectChatMessage chat{};
                chat.sender = msg.value("sender", std::string("tech"));
                chat.displayName = msg.value("display_name", msg.value("displayName", std::string("Technician")));
                chat.body = msg.value("body", msg.value("message", msg.value("text", std::string())));
                if (!chat.body.empty() && supportUiStarted.load()) {
                    supportWindow.AppendMessage(chat);
                }
                return;
            }

            if (type == "chat_close") {
                return;
            }
#endif

            if (type == "viewer_disconnected" || type == "viewer_closed" ||
                type == "viewer_left" || type == "stop_webrtc") {
                std::lock_guard<std::mutex> lock(sessionsMu);
                auto it = sessions.find(sessionId);
                if (it != sessions.end()) {
                    it->second->stop();
                    sessions.erase(it);
                }
                std::cout << "[connect] Technician viewer disconnected. Support app remains ready.\n";
#ifdef _WIN32
                if (supportUiStarted.load()) supportWindow.SetConnectionState("Waiting for technician", false);
#endif
                return;
            }

            if (type == "end_session" || type == "session_terminated") {
                std::cout << "[connect] Support session ended. You can close this window.\n";
#ifdef _WIN32
                sessionExplicitlyEnded.store(true);
                ConnectSetRestartResume(false, ticket);
                connectFiles.CancelAll();
#endif
                g_running = false;
                return;
            }
        });

        signaling.onClosed([&]() {
            signalingConnected.store(false);
            std::cout << "[connect] Secure support connection closed.\n";

            {
                std::lock_guard<std::mutex> lock(sessionsMu);
                for (auto& [_, sender] : sessions) sender->stop();
                sessions.clear();
            }

#ifdef _WIN32
            if (elevationHandoffRequested.load()) {
                std::cout << "[connect] Elevated Connect instance is taking over the session.\n";
                g_running = false;
                return;
            }
            if (supportUiStarted.load()) {
                supportWindow.SetConnectionState("Reconnecting...", false);
            }
#endif
            if (g_running) reconnectRequested.store(true);
        });

        reconnectRequested.store(false);
        signalingConnected.store(false);
        signaling.connect();

        auto nextReconnect = std::chrono::steady_clock::now();
        int reconnectBackoffSeconds = 2;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = std::chrono::steady_clock::now();

            if (signalingConnected.load()) {
                reconnectBackoffSeconds = 2;
            }
            else if (reconnectRequested.load() && now >= nextReconnect) {
                reconnectRequested.store(false);
                try {
                    std::cout << "[connect] Reconnecting secure support channel...\n";
                    signaling.connect();
                }
                catch (const std::exception& ex) {
                    LogWarn(std::string("[connect] signaling reconnect failed: ") + ex.what());
                    reconnectRequested.store(true);
                }
                nextReconnect = now + std::chrono::seconds(reconnectBackoffSeconds);
                reconnectBackoffSeconds = std::min(reconnectBackoffSeconds * 2, 30);
            }
        }

#ifdef _WIN32
        connectFiles.CancelAll();
        if (sessionExplicitlyEnded.load()) ConnectSetRestartResume(false, ticket);
        supportWindow.Stop();
#endif

        {
            std::lock_guard<std::mutex> lock(sessionsMu);
            for (auto& [_, sender] : sessions) sender->stop();
            sessions.clear();
        }

        std::cout << "[connect] Remote access has ended.\n";
        return 0;
    }
    catch (const std::exception& ex) {
#ifdef _WIN32
        supportWindow.Stop();
        const std::string detail = std::string("Unable to start the secure support session.\n\n") + ex.what() +
            "\n\nReturn to connect.hi5central.com and ask your technician for a new support code.";
        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, detail.c_str(), static_cast<int>(detail.size()), nullptr, 0);
        std::wstring wideDetail(static_cast<size_t>(std::max(0, wideLength)), L'\0');
        if (wideLength > 0) MultiByteToWideChar(CP_UTF8, 0, detail.c_str(), static_cast<int>(detail.size()), wideDetail.data(), wideLength);
        MessageBoxW(nullptr, wideDetail.c_str(), L"Hi5Central Connect", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
#endif
        std::cerr << "[connect] Unable to start support session: " << ex.what() << "\n";
        std::cerr << "[connect] Return to https://connect.hi5central.com and ask your technician for a new code.\n";
        return 1;
    }
}

static int RunDirectAgent(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    rtc::InitLogger(rtc::LogLevel::Info);

    constexpr int width = 0;
    constexpr int height = 0;
    constexpr int fps = 30;
    constexpr int bitrateKbps = 6000;

    const std::string defaultAgentWsBase = "wss://rmm.hi5central.com/agent/ws";
    std::wstring configDir = L"C:\\ProgramData\\Hi5Central\\Agent";

    try {
        if (argc >= 2) {
            const std::string a = argv[1];
            if (!a.empty() && a.rfind("--", 0) != 0) {
                std::wstring arg;
                arg.assign(a.begin(), a.end());
                configDir = arg;
            }
        }

        AgentIdentity ident = loadAgentIdentityFromDir(configDir, defaultAgentWsBase);

        const std::string agentWsUrl =
            ident.agentWsBaseUrl +
            "?device_id=" + ident.deviceId +
            "&device_key=" + ident.deviceKey;

        std::cout << "[app] config dir: ";
        std::wcout << configDir << L"\n";
        std::cout << "[app] device_id: " << ident.deviceId << "\n";
        std::cout << "[app] agent ws: " << ident.agentWsBaseUrl << "\n";

        std::cout << "[main] loaded identity device_id=" << ident.deviceId << "\n";
        std::cout << "[main] connecting websocket " << ident.agentWsBaseUrl << "\n";

        SignalingClient signaling(agentWsUrl);

        std::mutex sessionsMu;
        std::unordered_map<std::string, std::unique_ptr<WebRtcSender>> sessions;

        auto sendFn = [&signaling](const std::string& payload) {
            signaling.send(payload);
        };

        signaling.onOpen([&]() {
            std::cout << "[main] websocket connected\n";
            std::cout << "[ws] connected\n";
            signaling.send(R"({"type":"hello"})");
        });

        signaling.onMessage([&](const std::string& text) {
            const auto msg = json::parse(text, nullptr, false);
            if (msg.is_discarded()) {
                std::cerr << "[ws] invalid json\n";
                return;
            }

            const std::string type = msg.value("type", "");
            std::string sessionId = msg.value("session_id", "");
            if (sessionId.empty()) {
                sessionId = msg.value("session", "");
            }

            if (type == "start_webrtc") {
                std::cout << "[main] start_webrtc session=" << sessionId << "\n";
                std::cout << "[ws] start_webrtc session=" << sessionId << "\n";

                auto iceServers = parseIceServers(msg);

                auto sender = std::make_unique<WebRtcSender>(
                    sessionId,
                    iceServers,
                    sendFn,
                    width,
                    height,
                    fps,
                    bitrateKbps
                );
                sender->start();

                std::lock_guard<std::mutex> lock(sessionsMu);
                sessions[sessionId] = std::move(sender);
                return;
            }

            if (
                type == "webrtc_answer" ||
                type == "ice_candidate" ||
                type == "answer" ||
                type == "candidate" ||
                type == "viewer_answer"
            ) {
                std::cout << "[ws] route signaling type=" << type << " session=" << sessionId << "\n";

                std::lock_guard<std::mutex> lock(sessionsMu);
                auto it = sessions.find(sessionId);
                if (it != sessions.end()) {
                    it->second->handleSignalingMessage(text);
                } else {
                    std::cout
                        << "[ws] no active session for signaling type=" << type
                        << " session=" << sessionId
                        << " active_sessions=" << sessions.size()
                        << "\n";
                }
                return;
            }

            if (type == "viewer_disconnected") {
                std::cout << "[ws] viewer_disconnected session=" << sessionId << "\n";
                std::lock_guard<std::mutex> lock(sessionsMu);
                auto it = sessions.find(sessionId);
                if (it != sessions.end()) {
                    it->second->stop();
                    sessions.erase(it);
                }
                return;
            }
        });

        signaling.onClosed([&]() {
            std::cout << "[ws] closed\n";
            g_running = false;
        });

        signaling.connect();

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        {
            std::lock_guard<std::mutex> lock(sessionsMu);
            for (auto& [_, s] : sessions) {
                s->stop();
            }
            sessions.clear();
        }

        std::cout << "[main] stopped\n";
        std::cout << "[app] stopped\n";
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "[app] fatal: " << ex.what() << "\n";
        return 1;
    }
}

int main(int argc, char** argv) {
    const int cefExitCode = hi5::MaybeRunCefSubprocess(argc, argv);
    if (cefExitCode >= 0) return cefExitCode;

    std::cout << "[main] process start\n";
    LogInfo("[main] process start");

    const std::string connectTicket = ConnectTicketFromArgs(argc, argv);
    if (!connectTicket.empty()) {
        std::cout << "[main] mode=connect-host\n";
        LogInfo("[main] mode=connect-host");
        return RunConnectHost(connectTicket);
    }

    std::string mode = "direct-agent";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--install-service") {
            std::cout << "[main] mode=install-service\n";
            LogInfo("[main] mode=install-service");
            return hi5::InstallService("Hi5CentralAgent");
        }

        if (arg == "--uninstall-service") {
            std::cout << "[main] mode=uninstall-service\n";
            LogInfo("[main] mode=uninstall-service");
            return hi5::UninstallService("Hi5CentralAgent");
        }

        if (arg == "--service") {
            std::cout << "[main] mode=service\n";
            LogInfo("[main] mode=service");
            return hi5::RunServiceMain("Hi5CentralAgent");
        }

        if (arg == "--mode" && i + 1 < argc) {
            const std::string next = argv[i + 1];

            if (next == "streamer") {
                std::cout << "[main] mode=streamer\n";
                return hi5::RunStreamerMain(argc, argv);
            }

            if (next == "chat-overlay") {
                std::cout << "[main] mode=chat-overlay\n";
                LogInfo("[main] mode=chat-overlay");
                return hi5::RunChatOverlayMain(argc, argv);
            }

            if (next == "native-chat") {
                std::cout << "[main] mode=native-chat\n";
                LogInfo("[main] mode=native-chat");
                return hi5::RunNativeChatMain(argc, argv);
            }

            if (next == "banner") {
                std::cout << "[main] mode=banner\n";
                LogInfo("[main] mode=banner");
                return hi5::RunNativeBannerMain(argc, argv);
            }

            if (next == "tray") {
                std::cout << "[main] mode=tray\n";
                LogInfo("[main] mode=tray");
                return hi5::RunNativeTrayMain(argc, argv);
            }

            if (next == "sas-helper") {
                std::cout << "[main] mode=sas-helper\n";
                LogInfo("[main] mode=sas-helper");
                return RunSasHelperMain(argc, argv);
            }

            if (next == "backstage-host") {
                std::cout << "[main] mode=backstage-host\n";
                LogInfo("[main] mode=backstage-host");
                return hi5::RunBackstageHostMain(argc, argv);
            }

            if (next == "backstage-browser") {
                std::cout << "[main] mode=backstage-browser\n";
                LogInfo("[main] mode=backstage-browser");
                return hi5::RunBackstageBrowserMain(argc, argv);
            }
        }
    }

    std::cout << "[main] mode=direct-agent\n";
    LogPlatformInfo();
    return RunDirectAgent(argc, argv);
}