#ifdef _WIN32

#include "connect_capture_bridge.h"

#include "ipc/session_launcher.h"
#include "util/log.h"
#include "webrtc_sender.h"

#include <windows.h>
#include <winsvc.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>

namespace hi5 {
namespace {

struct PermissiveSecurity {
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};

    PermissiveSecurity() {
        InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&descriptor, TRUE, nullptr, FALSE);
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;
    }
};

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string Narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string CurrentExePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) return {};
    buffer.resize(count);
    return Narrow(buffer);
}

std::string ArgValue(
    int argc, char** argv, const std::string& name, const std::string& fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && name == argv[i]) return argv[i + 1] ? argv[i + 1] : fallback;
    }
    return fallback;
}

std::string QuoteArg(const std::string& value) {
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '\"') out += '\\';
        out += ch;
    }
    out += "\"";
    return out;
}

bool DomCodeToVk(const std::string& code, WORD& vk, bool& extended) {
    extended = false;
    if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        const char c = code[3];
        if (c >= 'A' && c <= 'Z') { vk = static_cast<WORD>(c); return true; }
    }
    if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
        const char c = code[5];
        if (c >= '0' && c <= '9') { vk = static_cast<WORD>(c); return true; }
    }

    if (code == "Enter") { vk = VK_RETURN; return true; }
    if (code == "Escape" || code == "Esc") { vk = VK_ESCAPE; return true; }
    if (code == "Backspace") { vk = VK_BACK; return true; }
    if (code == "Tab") { vk = VK_TAB; return true; }
    if (code == "Space") { vk = VK_SPACE; return true; }
    if (code == "Backquote") { vk = VK_OEM_3; return true; }
    if (code == "Minus") { vk = VK_OEM_MINUS; return true; }
    if (code == "Equal") { vk = VK_OEM_PLUS; return true; }
    if (code == "BracketLeft") { vk = VK_OEM_4; return true; }
    if (code == "BracketRight") { vk = VK_OEM_6; return true; }
    if (code == "Backslash") { vk = VK_OEM_5; return true; }
    if (code == "IntlBackslash") { vk = VK_OEM_102; return true; }
    if (code == "Semicolon") { vk = VK_OEM_1; return true; }
    if (code == "Quote") { vk = VK_OEM_7; return true; }
    if (code == "Comma") { vk = VK_OEM_COMMA; return true; }
    if (code == "Period") { vk = VK_OEM_PERIOD; return true; }
    if (code == "Slash") { vk = VK_OEM_2; return true; }

    if (code.size() == 7 && code.rfind("Numpad", 0) == 0 &&
        code[6] >= '0' && code[6] <= '9') {
        vk = static_cast<WORD>(VK_NUMPAD0 + (code[6] - '0'));
        return true;
    }
    if (code == "NumpadMultiply") { vk = VK_MULTIPLY; return true; }
    if (code == "NumpadAdd") { vk = VK_ADD; return true; }
    if (code == "NumpadSubtract") { vk = VK_SUBTRACT; return true; }
    if (code == "NumpadDecimal") { vk = VK_DECIMAL; return true; }
    if (code == "NumpadDivide") { vk = VK_DIVIDE; extended = true; return true; }
    if (code == "NumpadEnter") { vk = VK_RETURN; extended = true; return true; }

    if (code == "CapsLock") { vk = VK_CAPITAL; return true; }
    if (code == "NumLock") { vk = VK_NUMLOCK; extended = true; return true; }
    if (code == "ScrollLock") { vk = VK_SCROLL; return true; }
    if (code == "Pause") { vk = VK_PAUSE; return true; }
    if (code == "PrintScreen") { vk = VK_SNAPSHOT; extended = true; return true; }
    if (code == "ContextMenu") { vk = VK_APPS; extended = true; return true; }

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
    if (code == "Delete" || code == "Del") { vk = VK_DELETE; extended = true; return true; }
    if (code == "Insert") { vk = VK_INSERT; extended = true; return true; }
    if (code == "Home") { vk = VK_HOME; extended = true; return true; }
    if (code == "End") { vk = VK_END; extended = true; return true; }
    if (code == "PageUp") { vk = VK_PRIOR; extended = true; return true; }
    if (code == "PageDown") { vk = VK_NEXT; extended = true; return true; }

    if (code.size() >= 2 && code[0] == 'F') {
        try {
            const int fn = std::stoi(code.substr(1));
            if (fn >= 1 && fn <= 24) {
                vk = static_cast<WORD>(VK_F1 + (fn - 1));
                return true;
            }
        } catch (...) {}
    }
    return false;
}

uint8_t MouseButtonFromJson(const nlohmann::json& msg) {
    if (!msg.contains("button")) return 0;
    if (msg["button"].is_number_integer()) {
        return static_cast<uint8_t>(std::clamp(msg.value("button", 0), 0, 2));
    }
    const std::string button = msg.value("button", std::string("left"));
    if (button == "right") return 1;
    if (button == "middle") return 2;
    return 0;
}

bool IsNearBlackTransitionFrame(const I420Frame& frame) {
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
    const double avgY =
        static_cast<double>(sum) / static_cast<double>(count);
    return maxY <= 30 && avgY <= 21.0;
}

ShortcutAction ShortcutFromString(const std::string& action) {
    if (action == "ctrl_alt_del" || action == "ctrl_alt_del_service" ||
        action == "cad" || action == "sas" ||
        action == "secure_attention") return ShortcutAction::CtrlAltDel;
    if (action == "start_menu" || action == "windows_key" || action == "win")
        return ShortcutAction::StartMenu;
    if (action == "win_d" || action == "show_desktop") return ShortcutAction::WinD;
    if (action == "win_r" || action == "run_dialog") return ShortcutAction::WinR;
    if (action == "win_e" || action == "file_explorer") return ShortcutAction::WinE;
    if (action == "win_tab" || action == "task_view") return ShortcutAction::WinTab;
    if (action == "alt_tab") return ShortcutAction::AltTab;
    if (action == "alt_tab_begin") return ShortcutAction::AltTabBegin;
    if (action == "alt_tab_next") return ShortcutAction::AltTabNext;
    if (action == "alt_tab_end") return ShortcutAction::AltTabEnd;
    if (action == "alt_f4" || action == "close_window") return ShortcutAction::AltF4;
    if (action == "ctrl_shift_esc") return ShortcutAction::CtrlShiftEsc;
    if (action == "ctrl_esc") return ShortcutAction::CtrlEsc;
    if (action == "lock" || action == "lock_workstation") return ShortcutAction::LockWorkstation;
    if (action == "task_manager" || action == "taskmgr") return ShortcutAction::TaskManager;
    if (action == "explorer") return ShortcutAction::Explorer;
    return ShortcutAction::None;
}

bool InteractiveUserSessionReady(DWORD sessionId) {
    if (sessionId == 0xFFFFFFFF) return false;
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(sessionId, &token) || !token) return false;
    CloseHandle(token);
    return true;
}

struct SoftwareSasGenerationBackup {
    bool changed{ false };
    bool hadValue{ false };
    DWORD oldValue{ 0 };
};

bool EnsureTemporarySoftwareSasForServices(
    SoftwareSasGenerationBackup& backup) {
    constexpr const wchar_t* kPath =
        LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System)";
    constexpr const wchar_t* kName = L"SoftwareSASGeneration";

    backup = {};
    HKEY key = nullptr;
    DWORD disposition = 0;
    LONG rc = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, kPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, &disposition);
    if (rc != ERROR_SUCCESS || !key) {
        LogError("[connect-cad] temporary SoftwareSASGeneration open failed err=" +
            std::to_string(static_cast<DWORD>(rc)));
        return false;
    }

    DWORD value = 0;
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    rc = RegQueryValueExW(
        key, kName, nullptr, &type,
        reinterpret_cast<LPBYTE>(&value), &bytes);
    if (rc == ERROR_SUCCESS && type == REG_DWORD) {
        backup.hadValue = true;
        backup.oldValue = value;
    }

    if (backup.hadValue && ((backup.oldValue & 0x1u) != 0)) {
        RegCloseKey(key);
        return true;
    }

    // Match the managed Agent exactly: enable the Services bit only for the
    // SendSAS call, preserving every pre-existing policy bit, then restore it.
    const DWORD newValue =
        backup.hadValue ? (backup.oldValue | 0x1u) : 1u;
    rc = RegSetValueExW(
        key, kName, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&newValue), sizeof(newValue));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        LogError("[connect-cad] temporary SoftwareSASGeneration set failed err=" +
            std::to_string(static_cast<DWORD>(rc)));
        return false;
    }
    backup.changed = true;
    return true;
}

void RestoreTemporarySoftwareSasGeneration(
    const SoftwareSasGenerationBackup& backup) {
    if (!backup.changed) return;

    constexpr const wchar_t* kPath =
        LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System)";
    constexpr const wchar_t* kName = L"SoftwareSASGeneration";
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, kPath, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS || !key) {
        LogWarn("[connect-cad] temporary SoftwareSASGeneration restore open failed err=" +
            std::to_string(static_cast<DWORD>(rc)));
        return;
    }

    if (backup.hadValue) {
        DWORD oldValue = backup.oldValue;
        rc = RegSetValueExW(
            key, kName, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&oldValue), sizeof(oldValue));
    } else {
        rc = RegDeleteValueW(key, kName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        LogWarn("[connect-cad] temporary SoftwareSASGeneration restore failed err=" +
            std::to_string(static_cast<DWORD>(rc)));
    }
}

bool SendSecureAttentionSequenceFromBroker(
    const std::string& sessionId) {
    SoftwareSasGenerationBackup backup;
    const bool policyReady =
        EnsureTemporarySoftwareSasForServices(backup);

    bool sent = false;
    if (policyReady) {
        using SendSasFn = void (WINAPI*)(BOOL);
        HMODULE sas = LoadLibraryW(L"sas.dll");
        if (sas) {
            auto fn = reinterpret_cast<SendSasFn>(
                GetProcAddress(sas, "SendSAS"));
            if (fn) {
                LogInfo("[connect-cad] LocalSystem SendSAS(FALSE) session=" +
                    sessionId);
                fn(FALSE);
                Sleep(150);
                sent = true;
            } else {
                LogWarn("[connect-cad] GetProcAddress(SendSAS) failed err=" +
                    std::to_string(GetLastError()));
            }
            FreeLibrary(sas);
        } else {
            LogWarn("[connect-cad] LoadLibrary(sas.dll) failed err=" +
                std::to_string(GetLastError()));
        }
    }

    RestoreTemporarySoftwareSasGeneration(backup);
    LogInfo("[connect-cad] request complete session=" + sessionId +
        " sent=" + std::string(sent ? "true" : "false"));
    return sent;
}

} // namespace

ConnectCaptureBridge::~ConnectCaptureBridge() {
    Stop(ownsBroker_);
}

bool ConnectCaptureBridge::CreateSharedObjects(const std::string& prefix) {
    // Only derive the object names here. The temporary LocalSystem broker creates
    // every Global\\ mapping/event so Connect does not depend on the interactive
    // admin token holding SeCreateGlobalPrivilege.
    normalShmemName_ = "Global\\Hi5ConnectStream_" + prefix;
    secureShmemName_ = "Global\\Hi5ConnectStream_UAC_" + prefix;
    normalInputName_ = "Global\\Hi5ConnectInput_" + prefix;
    secureInputName_ = "Global\\Hi5ConnectInput_UAC_" + prefix;
    normalStopName_ = "Global\\Hi5ConnectStop_" + prefix;
    secureStopName_ = "Global\\Hi5ConnectStop_UAC_" + prefix;
    brokerStopName_ = "Global\\Hi5ConnectBrokerStop_" + prefix;
    loginDesktopName_ = "Global\\Hi5ConnectLoginDesktop_" + prefix;
    cadRequestName_ = "Global\\Hi5ConnectCadRequest_" + prefix;
    cadSuccessName_ = "Global\\Hi5ConnectCadSuccess_" + prefix;
    cadFailureName_ = "Global\\Hi5ConnectCadFailure_" + prefix;
    return true;
}

bool ConnectCaptureBridge::OpenSharedObjects() {
    constexpr int kAttempts = 160;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (!normalShmem_.IsOpen()) {
            normalShmem_.OpenConsumer(normalShmemName_);
        }
        if (!secureShmem_.IsOpen()) {
            secureShmem_.OpenConsumer(secureShmemName_);
        }
        if (!normalInput_.IsOpen()) {
            normalInput_.Open(normalInputName_);
        }
        if (!secureInput_.IsOpen()) {
            secureInput_.Open(secureInputName_);
        }
        if (!normalStopEvent_) {
            normalStopEvent_ = OpenEventA(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE, normalStopName_.c_str());
        }
        if (!secureStopEvent_) {
            secureStopEvent_ = OpenEventA(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE, secureStopName_.c_str());
        }
        if (!brokerStopEvent_) {
            brokerStopEvent_ = OpenEventA(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE, brokerStopName_.c_str());
        }
        if (!loginDesktopEvent_) {
            loginDesktopEvent_ = OpenEventA(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE, loginDesktopName_.c_str());
        }
        if (!cadRequestEvent_) {
            cadRequestEvent_ = OpenEventA(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE, cadRequestName_.c_str());
        }
        if (!cadSuccessEvent_) {
            cadSuccessEvent_ = OpenEventA(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE, cadSuccessName_.c_str());
        }
        if (!cadFailureEvent_) {
            cadFailureEvent_ = OpenEventA(
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                FALSE, cadFailureName_.c_str());
        }

        const bool ready =
            normalShmem_.IsOpen() &&
            secureShmem_.IsOpen() &&
            normalInput_.IsOpen() &&
            secureInput_.IsOpen() &&
            normalStopEvent_ &&
            secureStopEvent_ &&
            brokerStopEvent_ &&
            loginDesktopEvent_ &&
            cadRequestEvent_ &&
            cadSuccessEvent_ &&
            cadFailureEvent_;

        // A successful normal monitor publication is the authoritative proof
        // that the broker and its session-bound LocalSystem streamer are live.
        if (ready) {
            for (int monitorAttempt = 0;
                 monitorAttempt < 100;
                 ++monitorAttempt) {
                if (normalInput_.GetMonitorCount() > 0 ||
                    secureInput_.GetMonitorCount() > 0) return true;
                Sleep(20);
            }
            LogError(
                "[connect-broker] LocalSystem streamer opened but did not publish normal/login monitor geometry");
            return false;
        }

        Sleep(25);
    }

    LogError("[connect-broker] timed out opening LocalSystem shared objects err=" +
        std::to_string(GetLastError()));
    return false;
}

bool ConnectCaptureBridge::InstallAndStartBroker() {
    const std::string exe = CurrentExePath();
    if (exe.empty()) return false;

    const DWORD pid = GetCurrentProcessId();
    std::ostringstream command;
    command
        << QuoteArg(exe)
        << " --connect-capture-broker"
        << " --service-name " << QuoteArg(serviceName_)
        << " --session " << QuoteArg(sessionId_)
        << " --normal-shmem " << QuoteArg(normalShmemName_)
        << " --secure-shmem " << QuoteArg(secureShmemName_)
        << " --normal-input " << QuoteArg(normalInputName_)
        << " --secure-input " << QuoteArg(secureInputName_)
        << " --normal-stop " << QuoteArg(normalStopName_)
        << " --secure-stop " << QuoteArg(secureStopName_)
        << " --broker-stop " << QuoteArg(brokerStopName_)
        << " --login-desktop " << QuoteArg(loginDesktopName_)
        << " --cad-request " << QuoteArg(cadRequestName_)
        << " --cad-success " << QuoteArg(cadSuccessName_)
        << " --cad-failure " << QuoteArg(cadFailureName_)
        << " --connect-ticket " << QuoteArg(connectTicket_)
        << " --fps " << fps_
        << " --display " << displayIndex_.load()
        << " --parent-pid " << pid;

    const std::wstring wideService = Wide(serviceName_);
    const std::wstring wideDisplay =
        L"Hi5Central Connect Temporary Capture Broker";
    const std::wstring wideCommand = Wide(command.str());

    SC_HANDLE scm = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        LogError("[connect-broker] OpenSCManager failed err=" +
            std::to_string(GetLastError()));
        return false;
    }

    SC_HANDLE service = CreateServiceW(
        scm,
        wideService.c_str(),
        wideDisplay.c_str(),
        SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL,
        wideCommand.c_str(),
        nullptr, nullptr, nullptr,
        nullptr, nullptr);

    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(
            scm, wideService.c_str(),
            SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    }

    if (!service) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        LogError("[connect-broker] Create/Open service failed err=" +
            std::to_string(err));
        return false;
    }

    BOOL started = StartServiceW(service, 0, nullptr);
    if (!started && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        const DWORD err = GetLastError();
        DeleteService(service);
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        LogError("[connect-broker] StartService failed err=" +
            std::to_string(err));
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    bool running = false;
    for (int i = 0; i < 80; ++i) {
        if (QueryServiceStatusEx(
                service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status), &needed)) {
            if (status.dwCurrentState == SERVICE_RUNNING) {
                running = true;
                break;
            }
            if (status.dwCurrentState == SERVICE_STOPPED) break;
        }
        Sleep(25);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (!running) {
        LogError("[connect-broker] temporary capture service did not reach RUNNING");
        return false;
    }

    LogInfo("[connect-broker] temporary LocalSystem capture service started name=" +
        serviceName_);
    return true;
}

void ConnectCaptureBridge::RemoveBrokerService() {
    if (serviceName_.empty()) return;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    const std::wstring wideService = Wide(serviceName_);
    SC_HANDLE service = OpenServiceW(
        scm, wideService.c_str(),
        SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (service) {
        SERVICE_STATUS status{};
        ControlService(service, SERVICE_CONTROL_STOP, &status);
        DeleteService(service);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
}

bool ConnectCaptureBridge::Start(
    const std::string& sessionId, int fps, int displayIndex,
    const std::string& connectTicket) {
    if (running_.load(std::memory_order_acquire) &&
        sessionId_ == sessionId) {
        return true;
    }

    Stop();

    sessionId_ = sessionId;
    connectTicket_ = connectTicket;
    ownsBroker_ = true;
    fps_ = std::clamp(fps, 1, 60);
    displayIndex_.store(std::max(0, displayIndex));

    const DWORD pid = GetCurrentProcessId();
    std::string shortId = sessionId;
    shortId.erase(
        std::remove_if(shortId.begin(), shortId.end(),
            [](char c) {
                return !(c >= 'a' && c <= 'z') &&
                       !(c >= 'A' && c <= 'Z') &&
                       !(c >= '0' && c <= '9');
            }),
        shortId.end());
    if (shortId.size() > 10) shortId.resize(10);
    if (shortId.empty()) shortId = "session";

    const std::string prefix =
        shortId + "_" + std::to_string(pid);
    serviceName_ =
        "Hi5CentralConnectCapture_" + std::to_string(pid);

    if (!CreateSharedObjects(prefix)) {
        Stop();
        return false;
    }

    if (!InstallAndStartBroker()) {
        Stop();
        return false;
    }

    if (!OpenSharedObjects()) {
        Stop();
        return false;
    }

    running_.store(true, std::memory_order_release);
    LogInfo("[connect-broker] capture bridge ready session=" + sessionId_);
    return true;
}

bool ConnectCaptureBridge::AttachExisting(
    const std::string& sessionId,
    const std::string& serviceName,
    const std::string& normalShmem,
    const std::string& secureShmem,
    const std::string& normalInput,
    const std::string& secureInput,
    const std::string& normalStop,
    const std::string& secureStop,
    const std::string& brokerStop,
    const std::string& loginDesktop,
    const std::string& cadRequest,
    const std::string& cadSuccess,
    const std::string& cadFailure) {
    Stop(false);

    sessionId_ = sessionId;
    serviceName_ = serviceName;
    normalShmemName_ = normalShmem;
    secureShmemName_ = secureShmem;
    normalInputName_ = normalInput;
    secureInputName_ = secureInput;
    normalStopName_ = normalStop;
    secureStopName_ = secureStop;
    brokerStopName_ = brokerStop;
    loginDesktopName_ = loginDesktop;
    cadRequestName_ = cadRequest;
    cadSuccessName_ = cadSuccess;
    cadFailureName_ = cadFailure;
    ownsBroker_ = false;

    if (!OpenSharedObjects()) {
        Stop(false);
        return false;
    }

    running_.store(true, std::memory_order_release);
    LogInfo("[connect-broker] continuity host attached to existing broker session=" +
        sessionId_);
    return true;
}

void ConnectCaptureBridge::Stop(bool stopBroker) {
    StopPump();
    running_.store(false, std::memory_order_release);

    if (stopBroker) {
        if (brokerStopEvent_) SetEvent(brokerStopEvent_);
        if (normalStopEvent_) SetEvent(normalStopEvent_);
        if (secureStopEvent_) SetEvent(secureStopEvent_);
        RemoveBrokerService();
    }

    if (normalStopEvent_) {
        CloseHandle(normalStopEvent_);
        normalStopEvent_ = nullptr;
    }
    if (secureStopEvent_) {
        CloseHandle(secureStopEvent_);
        secureStopEvent_ = nullptr;
    }
    if (brokerStopEvent_) {
        CloseHandle(brokerStopEvent_);
        brokerStopEvent_ = nullptr;
    }
    if (loginDesktopEvent_) {
        CloseHandle(loginDesktopEvent_);
        loginDesktopEvent_ = nullptr;
    }
    if (cadRequestEvent_) {
        CloseHandle(cadRequestEvent_);
        cadRequestEvent_ = nullptr;
    }
    if (cadSuccessEvent_) {
        CloseHandle(cadSuccessEvent_);
        cadSuccessEvent_ = nullptr;
    }
    if (cadFailureEvent_) {
        CloseHandle(cadFailureEvent_);
        cadFailureEvent_ = nullptr;
    }

    normalShmem_.Close();
    secureShmem_.Close();
    normalInput_.Close();
    secureInput_.Close();

    secureDesktopActive_.store(false);
    secureFallbackReady_.store(false);
    sessionId_.clear();
    serviceName_.clear();
    connectTicket_.clear();
    ownsBroker_ = false;
}

bool ConnectCaptureBridge::StartPump(
    WebRtcSender* sender, StateCallback stateCallback) {
    if (!running_.load(std::memory_order_acquire) || !sender) return false;
    StopPump();

    {
        std::lock_guard<std::mutex> lock(callbackMu_);
        sender_ = sender;
        stateCallback_ = std::move(stateCallback);
    }

    pumpRunning_.store(true, std::memory_order_release);
    pumpThread_ = std::thread([this]() { PumpLoop(); });
    return true;
}

void ConnectCaptureBridge::StopPump() {
    pumpRunning_.store(false, std::memory_order_release);
    if (pumpThread_.joinable()) pumpThread_.join();

    std::lock_guard<std::mutex> lock(callbackMu_);
    sender_ = nullptr;
    stateCallback_ = nullptr;
}

InputPipeWriter& ConnectCaptureBridge::ActiveInputPipe() {
    const bool loginDesktopActive =
        loginDesktopEvent_ &&
        WaitForSingleObject(loginDesktopEvent_, 0) == WAIT_OBJECT_0;

    // Winlogon owns input as soon as Windows enters lock/sign-in mode. Do not
    // wait for the first usable secure video frame before routing keyboard and
    // mouse: the Windows lock wallpaper itself may not be capturable, and a
    // remote key press is what dismisses it to the credential UI.
    if (loginDesktopActive ||
        (secureDesktopActive_.load(std::memory_order_acquire) &&
         secureFallbackReady_.load(std::memory_order_acquire))) {
        return secureInput_;
    }
    return normalInput_;
}

const InputPipeWriter& ConnectCaptureBridge::ActiveInputPipe() const {
    const bool loginDesktopActive =
        loginDesktopEvent_ &&
        WaitForSingleObject(loginDesktopEvent_, 0) == WAIT_OBJECT_0;

    if (loginDesktopActive ||
        (secureDesktopActive_.load(std::memory_order_acquire) &&
         secureFallbackReady_.load(std::memory_order_acquire))) {
        return secureInput_;
    }
    return normalInput_;
}

void ConnectCaptureBridge::PumpLoop() {
    bool lastSecure = false;
    bool secureReadyAnnounced = false;
    bool normalReturnPending = false;
    uint64_t desktopTransitionTickNs = 0;
    uint64_t lastStatsSeq = 0;
    std::chrono::steady_clock::time_point secureEnteredAt{};
    std::chrono::steady_clock::time_point normalReturnAt{};

    while (pumpRunning_.load(std::memory_order_acquire)) {
        const bool loginDesktopActive =
            loginDesktopEvent_ &&
            WaitForSingleObject(loginDesktopEvent_, 0) == WAIT_OBJECT_0;
        const bool secureNow =
            loginDesktopActive || normalInput_.GetUACActive();

        if (secureNow != lastSecure) {
            lastSecure = secureNow;
            desktopTransitionTickNs = normalInput_.GetDesktopTransitionTickNs();
            secureDesktopActive_.store(secureNow, std::memory_order_release);
            secureFallbackReady_.store(false, std::memory_order_release);
            secureReadyAnnounced = false;

            StateCallback state;
            {
                std::lock_guard<std::mutex> lock(callbackMu_);
                state = stateCallback_;
            }

            if (secureNow) {
                normalReturnPending = false;
                secureEnteredAt = std::chrono::steady_clock::now();
                normalReturnAt = {};
                if (state) {
                    state(loginDesktopActive
                        ? "login_desktop_entering"
                        : "secure_desktop_entering");
                }
                LogInfo(std::string("[connect-broker] ") +
                    (loginDesktopActive ? "Windows login desktop" : "secure desktop") +
                    " detected session=" + sessionId_);
            } else {
                normalReturnPending = true;
                normalReturnAt = std::chrono::steady_clock::now();
                secureEnteredAt = {};
                if (state) state("desktop_handoff_entering");
                LogInfo("[connect-broker] default desktop return detected session=" +
                    sessionId_);
            }
        }

        I420Frame normalFrame;
        I420Frame secureFrame;
        uint64_t normalTs = 0;
        uint64_t secureTs = 0;
        bool normalForce = false;
        bool secureForce = false;
        bool gotNormal = false;
        bool gotSecure = false;

        while (normalShmem_.ReadRawI420Frame(
            normalFrame, normalTs, &normalForce)) {
            gotNormal = true;
        }
        while (secureShmem_.ReadRawI420Frame(
            secureFrame, secureTs, &secureForce)) {
            gotSecure = true;
        }

        WebRtcSender* sender = nullptr;
        StateCallback state;
        {
            std::lock_guard<std::mutex> lock(callbackMu_);
            sender = sender_;
            state = stateCallback_;
        }

        if (sender) {
            const auto frameIsPostTransition =
                [desktopTransitionTickNs](uint64_t timestampNs) {
                    return desktopTransitionTickNs == 0 ||
                        timestampNs == 0 ||
                        timestampNs >= desktopTransitionTickNs;
                };

            if (secureNow) {
                // Match the managed Agent's transition fence: never release the
                // Viewer onto a frame captured before Windows actually switched
                // desktops. Prefer the dedicated winsta0\Winlogon fallback once
                // it has a usable frame, otherwise accept the LocalSystem
                // dynamic-desktop stream after it follows the secure desktop.
                const auto now = std::chrono::steady_clock::now();
                const bool transitionWindow =
                    secureEnteredAt.time_since_epoch().count() != 0 &&
                    now - secureEnteredAt < std::chrono::milliseconds(250);
                const bool secureNearBlack =
                    gotSecure && IsNearBlackTransitionFrame(secureFrame);
                const bool secureUsable =
                    gotSecure &&
                    frameIsPostTransition(secureTs) &&
                    // Winlogon/login is a steady-state desktop, not a brief UAC
                    // transition. Never publish a protected black capture as
                    // "login_desktop_ready"; hold the last visible frame until
                    // the secure GDI helper produces a real sign-in frame.
                    !(loginDesktopActive && secureNearBlack) &&
                    !(transitionWindow && secureNearBlack);
                const bool normalUsable =
                    !loginDesktopActive &&
                    gotNormal &&
                    frameIsPostTransition(normalTs) &&
                    !(transitionWindow &&
                      IsNearBlackTransitionFrame(normalFrame));

                if (secureUsable) {
                    secureFallbackReady_.store(true, std::memory_order_release);
                    sender->sendExternalRawI420(
                        secureFrame, secureTs,
                        secureForce || !secureReadyAnnounced);
                    if (!secureReadyAnnounced) {
                        secureReadyAnnounced = true;
                        if (state) {
                            state(loginDesktopActive
                                ? "login_desktop_ready"
                                : "secure_desktop_ready");
                        }
                    }
                } else if (normalUsable) {
                    sender->sendExternalRawI420(
                        normalFrame, normalTs,
                        normalForce || !secureReadyAnnounced);
                    if (!secureReadyAnnounced) {
                        secureReadyAnnounced = true;
                        if (state) {
                            state(loginDesktopActive
                                ? "login_desktop_ready"
                                : "secure_desktop_ready");
                        }
                    }
                }
            } else if (gotNormal && frameIsPostTransition(normalTs)) {
                const auto now = std::chrono::steady_clock::now();
                const bool transitionWindow =
                    normalReturnAt.time_since_epoch().count() != 0 &&
                    now - normalReturnAt < std::chrono::milliseconds(250);
                if (!(transitionWindow &&
                      IsNearBlackTransitionFrame(normalFrame))) {
                    sender->sendExternalRawI420(
                        normalFrame, normalTs,
                        normalForce || normalReturnPending);
                    if (normalReturnPending) {
                        normalReturnPending = false;
                        if (state) {
                            state("desktop_handoff_ready");
                            state("secure_desktop_exited");
                        }
                    }
                }
            }

            StreamStats stats{};
            if (normalInput_.ReadStreamStats(lastStatsSeq, stats)) {
                lastStatsSeq = stats.seq;
                sender->setExternalStreamHint(
                    stats.streamMode,
                    stats.targetFps,
                    false,
                    secureNow);
            }
        }

        Sleep(2);
    }
}

bool ConnectCaptureBridge::WriteText(
    InputPipeWriter& pipe,
    const std::string& text,
    InputCmdType type) {
    if (text.empty()) return false;
    uint32_t offset = 0;
    if (!pipe.WriteClipboard(
            text.data(), static_cast<uint32_t>(text.size()), offset)) {
        return false;
    }
    InputCmd cmd{};
    cmd.type = type;
    cmd.clipboard.offsetInClip = offset;
    cmd.clipboard.length = static_cast<uint32_t>(text.size());
    return pipe.Write(cmd);
}

bool ConnectCaptureBridge::WriteKey(
    InputPipeWriter& pipe,
    const std::string& code,
    bool down) {
    WORD vk = 0;
    bool extended = false;
    if (!DomCodeToVk(code, vk, extended)) return false;

    InputCmd cmd{};
    cmd.type = InputCmdType::KeyEvent;
    cmd.key.vk = vk;
    cmd.key.scanCode =
        static_cast<uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    cmd.key.down = down ? 1 : 0;
    cmd.key.isExtended = extended ? 1 : 0;
    return pipe.Write(cmd);
}

bool ConnectCaptureBridge::WriteShortcut(
    InputPipeWriter& pipe,
    ShortcutAction action) {
    if (action == ShortcutAction::None) return false;
    InputCmd cmd{};
    cmd.type = InputCmdType::Shortcut;
    cmd.shortcut.action = static_cast<uint16_t>(action);
    return pipe.Write(cmd);
}

bool ConnectCaptureBridge::WriteMouseMove(
    InputPipeWriter& pipe,
    double xNorm,
    double yNorm) {
    const int count = pipe.GetMonitorCount();
    if (count <= 0) return false;

    const int index =
        std::clamp(displayIndex_.load(std::memory_order_acquire), 0, count - 1);
    const auto monitor = pipe.GetMonitorInfo(index);
    if (monitor.w <= 0 || monitor.h <= 0) return false;

    const double x = std::clamp(xNorm, 0.0, 1.0);
    const double y = std::clamp(yNorm, 0.0, 1.0);

    InputCmd cmd{};
    cmd.type = InputCmdType::MouseMove;
    cmd.mouseMove.x =
        monitor.x + static_cast<int32_t>(
            x * static_cast<double>(std::max(1, monitor.w - 1)));
    cmd.mouseMove.y =
        monitor.y + static_cast<int32_t>(
            y * static_cast<double>(std::max(1, monitor.h - 1)));
    cmd.mouseMove.remoteW = monitor.w;
    cmd.mouseMove.remoteH = monitor.h;
    cmd.mouseMove.monitorIndex = index;
    return pipe.Write(cmd);
}

bool ConnectCaptureBridge::WriteMouseButton(
    InputPipeWriter& pipe,
    uint8_t button,
    bool down) {
    InputCmd cmd{};
    cmd.type = InputCmdType::MouseButton;
    cmd.mouseButton.button = button;
    cmd.mouseButton.down = down ? 1 : 0;
    return pipe.Write(cmd);
}

bool ConnectCaptureBridge::RequestSecureAttention() {
    if (!cadRequestEvent_ || !cadSuccessEvent_ || !cadFailureEvent_) return false;

    ResetEvent(cadSuccessEvent_);
    ResetEvent(cadFailureEvent_);
    if (!SetEvent(cadRequestEvent_)) return false;

    HANDLE resultEvents[2] = { cadSuccessEvent_, cadFailureEvent_ };
    const DWORD wait = WaitForMultipleObjects(2, resultEvents, FALSE, 2000);
    if (wait == WAIT_OBJECT_0) return true;
    if (wait == WAIT_OBJECT_0 + 1) return false;

    LogWarn("[connect-broker] Ctrl+Alt+Del broker result timed out session=" +
        sessionId_);
    return false;
}

bool ConnectCaptureBridge::HandleInputEvent(const nlohmann::json& msg) {
    InputPipeWriter& pipe = ActiveInputPipe();
    const std::string kind = msg.value("kind", std::string());
    const std::string type = msg.value("type", std::string());

    if (kind == "mouse_move") {
        return WriteMouseMove(
            pipe,
            msg.value("x_norm", 0.0),
            msg.value("y_norm", 0.0));
    }

    if (kind == "mouse_click") {
        if (msg.contains("x_norm") && msg.contains("y_norm")) {
            WriteMouseMove(
                pipe,
                msg.value("x_norm", 0.0),
                msg.value("y_norm", 0.0));
        }
        const uint8_t button = MouseButtonFromJson(msg);
        const int clicks = std::clamp(msg.value("click_count", 1), 1, 2);
        bool ok = true;
        for (int i = 0; i < clicks; ++i) {
            ok = WriteMouseButton(pipe, button, true) && ok;
            ok = WriteMouseButton(pipe, button, false) && ok;
        }
        return ok;
    }

    if (kind == "mouse_down" || kind == "mouse_up") {
        return WriteMouseButton(
            pipe,
            MouseButtonFromJson(msg),
            kind == "mouse_down");
    }

    if (kind == "wheel" || kind == "mouse_wheel") {
        InputCmd cmd{};
        cmd.type = InputCmdType::MouseWheel;
        cmd.mouseWheel.deltaX =
            msg.value("delta_x", msg.value("deltaX", 0));
        cmd.mouseWheel.deltaY =
            msg.value("delta_y", msg.value("deltaY", 0));
        return pipe.Write(cmd);
    }

    if (kind == "key_down" || kind == "key_up") {
        const std::string code =
            msg.value("code", msg.value("key", std::string()));
        return WriteKey(pipe, code, kind == "key_down");
    }

    if (kind == "key_press") {
        const std::string code =
            msg.value("code", msg.value("key", std::string()));
        return WriteKey(pipe, code, true) &&
            WriteKey(pipe, code, false);
    }

    if (kind == "text_input" || kind == "text" ||
        kind == "insert_text" || kind == "key_text") {
        const std::string value =
            msg.value("text", msg.value("key", std::string()));
        return WriteText(pipe, value, InputCmdType::PasteText);
    }

    if (kind == "clipboard_set" || kind == "clipboard_paste") {
        const std::string value = msg.value("text", std::string());
        return WriteText(
            pipe, value,
            kind == "clipboard_paste"
                ? InputCmdType::ClipboardPaste
                : InputCmdType::ClipboardSet);
    }

    if (kind == "shortcut" || type == "shortcut" ||
        kind == "system_shortcut" || type == "system_shortcut" ||
        kind == "service_shortcut" || type == "service_shortcut" ||
        kind == "service_command" || type == "service_command") {
        const std::string action =
            msg.value("action",
                msg.value("shortcut", std::string()));
        const ShortcutAction shortcut = ShortcutFromString(action);

        if (shortcut == ShortcutAction::CtrlAltDel) {
            // The installed Agent sends CAD from its LocalSystem service using
            // the temporary SoftwareSASGeneration policy. Do the same here,
            // while retaining the streamer's normal shortcut as a VM fallback.
            const bool sasSent = RequestSecureAttention();
            (void)WriteShortcut(pipe, ShortcutAction::CtrlAltDel);
            return sasSent;
        }

        return WriteShortcut(pipe, shortcut);
    }

    return false;
}

bool ConnectCaptureBridge::HandleFastMouse(
    double xNorm,
    double yNorm,
    uint64_t seq,
    double clientTsMs) {
    InputPipeWriter& pipe = ActiveInputPipe();
    const int count = pipe.GetMonitorCount();
    if (count <= 0) return false;

    const int index =
        std::clamp(displayIndex_.load(std::memory_order_acquire), 0, count - 1);
    const auto monitor = pipe.GetMonitorInfo(index);
    if (monitor.w <= 0 || monitor.h <= 0) return false;

    const double x = std::clamp(xNorm, 0.0, 1.0);
    const double y = std::clamp(yNorm, 0.0, 1.0);
    const int32_t px =
        monitor.x + static_cast<int32_t>(
            x * static_cast<double>(std::max(1, monitor.w - 1)));
    const int32_t py =
        monitor.y + static_cast<int32_t>(
            y * static_cast<double>(std::max(1, monitor.h - 1)));

    return pipe.PublishFastMouseTarget(
        px, py, index, seq,
        clientTsMs > 0.0
            ? static_cast<uint64_t>(clientTsMs)
            : 0);
}

bool ConnectCaptureBridge::SwitchMonitor(int index) {
    const int normalCount = normalInput_.GetMonitorCount();
    const int secureCount = secureInput_.GetMonitorCount();
    const int count = normalCount > 0 ? normalCount : secureCount;
    if (count <= 0) return false;
    const int wanted = std::clamp(index, 0, count - 1);
    displayIndex_.store(wanted, std::memory_order_release);

    InputCmd cmd{};
    cmd.type = InputCmdType::SwitchMonitor;
    cmd.switchMonitor.monitorIndex = wanted;
    const bool normal = normalInput_.Write(cmd);
    const bool secure = secureInput_.Write(cmd);
    return normal || secure;
}

nlohmann::json ConnectCaptureBridge::BuildMonitorInfoMessage(
    const std::string& sessionId) const {
    nlohmann::json out{
        {"type", "monitor_info"},
        {"session_id", sessionId},
        {"current", displayIndex_.load(std::memory_order_acquire)},
        {"monitors", nlohmann::json::array()}
    };

    const int normalCount = normalInput_.GetMonitorCount();
    const int secureCount = secureInput_.GetMonitorCount();
    const bool useSecure = normalCount <= 0 && secureCount > 0;
    const int count = useSecure ? secureCount : normalCount;
    for (int i = 0; i < count && i < 8; ++i) {
        const auto monitor = useSecure
            ? secureInput_.GetMonitorInfo(i)
            : normalInput_.GetMonitorInfo(i);
        out["monitors"].push_back({
            {"index", i},
            {"name", "Display " + std::to_string(i + 1)},
            {"x", monitor.x},
            {"y", monitor.y},
            {"w", monitor.w},
            {"h", monitor.h},
            {"primary", monitor.primary != 0}
        });
    }
    return out;
}

namespace {

struct BrokerConfig {
    std::string serviceName;
    std::string sessionId;
    std::string normalShmem;
    std::string secureShmem;
    std::string normalInput;
    std::string secureInput;
    std::string normalStop;
    std::string secureStop;
    std::string brokerStop;
    std::string loginDesktop;
    std::string cadRequest;
    std::string cadSuccess;
    std::string cadFailure;
    std::string connectTicket;
    int fps{ 30 };
    int display{ 0 };
    DWORD parentPid{ 0 };
};

BrokerConfig gBrokerConfig;
SERVICE_STATUS_HANDLE gBrokerStatusHandle = nullptr;
SERVICE_STATUS gBrokerStatus{};
HANDLE gBrokerScmStopEvent = nullptr;
std::atomic<uint64_t> gBrokerSessionChangeSeq{ 0 };
std::atomic<DWORD> gBrokerSessionChangeType{ 0 };
std::atomic<DWORD> gBrokerSessionChangeSessionId{ 0xFFFFFFFF };

void SetBrokerServiceState(
    DWORD state,
    DWORD win32ExitCode = NO_ERROR,
    DWORD waitHint = 0) {
    if (!gBrokerStatusHandle) return;
    gBrokerStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gBrokerStatus.dwCurrentState = state;
    gBrokerStatus.dwWin32ExitCode = win32ExitCode;
    gBrokerStatus.dwWaitHint = waitHint;
    gBrokerStatus.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? (SERVICE_ACCEPT_STOP |
               SERVICE_ACCEPT_SHUTDOWN |
               SERVICE_ACCEPT_SESSIONCHANGE)
            : 0;
    SetServiceStatus(gBrokerStatusHandle, &gBrokerStatus);
}

DWORD WINAPI ConnectBrokerControlHandler(
    DWORD control,
    DWORD eventType,
    LPVOID eventData,
    LPVOID) {
    if (control == SERVICE_CONTROL_STOP ||
        control == SERVICE_CONTROL_SHUTDOWN) {
        SetBrokerServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        if (gBrokerScmStopEvent) SetEvent(gBrokerScmStopEvent);
        return NO_ERROR;
    }

    if (control == SERVICE_CONTROL_SESSIONCHANGE) {
        DWORD sessionId = 0xFFFFFFFF;
        if (eventData) {
            const auto* note =
                static_cast<const WTSSESSION_NOTIFICATION*>(eventData);
            sessionId = note->dwSessionId;
        }
        gBrokerSessionChangeSessionId.store(
            sessionId, std::memory_order_release);
        gBrokerSessionChangeType.store(
            eventType, std::memory_order_release);
        gBrokerSessionChangeSeq.fetch_add(
            1, std::memory_order_acq_rel);
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

void CloseChildProcess(
    HANDLE& process,
    HANDLE stopEvent,
    const char* label) {
    if (!process) return;
    if (stopEvent) SetEvent(stopEvent);
    if (WaitForSingleObject(process, 1200) != WAIT_OBJECT_0) {
        LogWarn(std::string("[connect-broker-service] forcing ") +
            label + " helper shutdown");
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 500);
    }
    CloseHandle(process);
    process = nullptr;
}

HANDLE LaunchBrokerNormalStreamer(
    const BrokerConfig& config,
    DWORD consoleSession) {
    if (consoleSession == 0xFFFFFFFF) return nullptr;
    const std::string exe = CurrentExePath();
    if (exe.empty()) return nullptr;

    std::string command =
        "--mode streamer"
        " --session " + QuoteArg(config.sessionId) +
        " --shmem " + QuoteArg(config.normalShmem) +
        " --input-pipe " + QuoteArg(config.normalInput) +
        " --stop-event " + QuoteArg(config.normalStop) +
        " --fps " + std::to_string(config.fps) +
        " --display " + std::to_string(config.display) +
        " --dynamic-desktop"
        " --disable-gpu-transport";

    LogInfo("[connect-broker-service] launch LocalSystem dynamic streamer session=" +
        config.sessionId);
    return LaunchInElevatedDefaultSessionForSession(
        exe, command, consoleSession);
}

HANDLE LaunchBrokerSecureStreamer(
    const BrokerConfig& config,
    DWORD consoleSession) {
    if (consoleSession == 0xFFFFFFFF) return nullptr;
    const std::string exe = CurrentExePath();
    if (exe.empty()) return nullptr;

    std::string command =
        "--mode streamer"
        " --session " + QuoteArg(config.sessionId) +
        " --shmem " + QuoteArg(config.secureShmem) +
        " --input-pipe " + QuoteArg(config.secureInput) +
        " --stop-event " + QuoteArg(config.secureStop) +
        " --fps " + std::to_string(config.fps) +
        " --display " + std::to_string(config.display);

    LogInfo("[connect-broker-service] launch Winlogon secure helper session=" +
        config.sessionId);
    return LaunchOnSecureDesktopForSession(
        exe, command, consoleSession);
}


HANDLE LaunchContinuityHost(const BrokerConfig& config) {
    const std::string exe = CurrentExePath();
    if (exe.empty() || config.connectTicket.empty()) return nullptr;

    const std::string command =
        QuoteArg(exe) +
        " --connect-continuity-host" +
        " --connect-ticket " + QuoteArg(config.connectTicket) +
        " --session " + QuoteArg(config.sessionId) +
        " --service-name " + QuoteArg(config.serviceName) +
        " --normal-shmem " + QuoteArg(config.normalShmem) +
        " --secure-shmem " + QuoteArg(config.secureShmem) +
        " --normal-input " + QuoteArg(config.normalInput) +
        " --secure-input " + QuoteArg(config.secureInput) +
        " --normal-stop " + QuoteArg(config.normalStop) +
        " --secure-stop " + QuoteArg(config.secureStop) +
        " --broker-stop " + QuoteArg(config.brokerStop) +
        " --login-desktop " + QuoteArg(config.loginDesktop) +
        " --cad-request " + QuoteArg(config.cadRequest) +
        " --cad-success " + QuoteArg(config.cadSuccess) +
        " --cad-failure " + QuoteArg(config.cadFailure);

    std::wstring wideExe = Wide(exe);
    std::wstring wideCommand = Wide(command);
    if (wideExe.empty() || wideCommand.empty()) return nullptr;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(
        wideExe.c_str(),
        wideCommand.data(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &startup, &process);
    if (!ok) {
        LogError("[connect-broker-service] continuity host launch failed err=" +
            std::to_string(GetLastError()));
        return nullptr;
    }

    CloseHandle(process.hThread);
    LogInfo("[connect-broker-service] LocalSystem continuity host launched session=" +
        config.sessionId +
        " pid=" + std::to_string(process.dwProcessId));
    return process.hProcess;
}

void DeleteOwnBrokerService(const std::string& serviceName) {
    const std::wstring wideName = Wide(serviceName);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE service = OpenServiceW(scm, wideName.c_str(), DELETE);
    if (service) {
        DeleteService(service);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
}

void WINAPI ConnectBrokerServiceMain(DWORD, LPWSTR*) {
    const std::wstring serviceName = Wide(gBrokerConfig.serviceName);
    gBrokerStatusHandle = RegisterServiceCtrlHandlerExW(
        serviceName.c_str(),
        &ConnectBrokerControlHandler,
        nullptr);
    if (!gBrokerStatusHandle) return;

    SetBrokerServiceState(SERVICE_START_PENDING, NO_ERROR, 4000);
    gBrokerScmStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gBrokerScmStopEvent) {
        SetBrokerServiceState(
            SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // The LocalSystem broker owns creation of every Global\\ object. This is
    // deliberately the same authority boundary as the installed Agent service:
    // the interactive Connect process only opens these mappings after creation.
    ShmemRing normalFrames;
    ShmemRing secureFrames;
    InputPipeWriter normalInput;
    InputPipeWriter secureInput;
    PermissiveSecurity sharedSecurity;

    const bool normalFramesReady =
        normalFrames.CreateProducer(gBrokerConfig.normalShmem);
    const bool secureFramesReady =
        secureFrames.CreateProducer(gBrokerConfig.secureShmem);
    const bool normalInputReady =
        normalInput.Create(gBrokerConfig.normalInput);
    const bool secureInputReady =
        secureInput.Create(gBrokerConfig.secureInput);

    HANDLE brokerStop = CreateEventA(
        &sharedSecurity.attributes, TRUE, FALSE,
        gBrokerConfig.brokerStop.c_str());
    HANDLE normalStop = CreateEventA(
        &sharedSecurity.attributes, TRUE, FALSE,
        gBrokerConfig.normalStop.c_str());
    HANDLE secureStop = CreateEventA(
        &sharedSecurity.attributes, TRUE, FALSE,
        gBrokerConfig.secureStop.c_str());
    HANDLE loginDesktop = CreateEventA(
        &sharedSecurity.attributes, TRUE, FALSE,
        gBrokerConfig.loginDesktop.c_str());
    HANDLE cadRequest = CreateEventA(
        &sharedSecurity.attributes, TRUE, FALSE,
        gBrokerConfig.cadRequest.c_str());
    HANDLE cadSuccess = CreateEventA(
        &sharedSecurity.attributes, TRUE, FALSE,
        gBrokerConfig.cadSuccess.c_str());
    HANDLE cadFailure = CreateEventA(
        &sharedSecurity.attributes, TRUE, FALSE,
        gBrokerConfig.cadFailure.c_str());
    HANDLE parent = gBrokerConfig.parentPid
        ? OpenProcess(SYNCHRONIZE, FALSE, gBrokerConfig.parentPid)
        : nullptr;

    if (!normalFramesReady || !secureFramesReady ||
        !normalInputReady || !secureInputReady ||
        !brokerStop || !normalStop || !secureStop ||
        !loginDesktop || !cadRequest || !cadSuccess || !cadFailure) {
        const DWORD err = GetLastError();
        LogError("[connect-broker-service] Global shared-object create failed err=" +
            std::to_string(err));
        if (parent) CloseHandle(parent);
        if (brokerStop) CloseHandle(brokerStop);
        if (normalStop) CloseHandle(normalStop);
        if (secureStop) CloseHandle(secureStop);
        if (loginDesktop) CloseHandle(loginDesktop);
        if (cadRequest) CloseHandle(cadRequest);
        if (cadSuccess) CloseHandle(cadSuccess);
        if (cadFailure) CloseHandle(cadFailure);
        normalInput.Close();
        secureInput.Close();
        normalFrames.Close();
        secureFrames.Close();
        CloseHandle(gBrokerScmStopEvent);
        gBrokerScmStopEvent = nullptr;
        SetBrokerServiceState(SERVICE_STOPPED, err, 0);
        DeleteOwnBrokerService(gBrokerConfig.serviceName);
        return;
    }

    DWORD consoleSession = WTSGetActiveConsoleSessionId();
    bool interactiveReady =
        InteractiveUserSessionReady(consoleSession);
    ResetEvent(normalStop);
    ResetEvent(secureStop);
    ResetEvent(cadRequest);
    ResetEvent(cadSuccess);
    ResetEvent(cadFailure);

    HANDLE normalProcess = nullptr;
    HANDLE secureProcess = nullptr;
    HANDLE continuityProcess = nullptr;
    bool continuityTakeover = false;
    bool parentExitPending = false;
    std::chrono::steady_clock::time_point parentExitedAt{};
    const auto brokerStartedAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastContinuityLaunchAt{};

    if (interactiveReady) {
        ResetEvent(loginDesktop);
        normalProcess =
            LaunchBrokerNormalStreamer(gBrokerConfig, consoleSession);
        if (!normalProcess) {
            LogError("[connect-broker-service] normal LocalSystem streamer failed");
            SetEvent(gBrokerScmStopEvent);
        }
    } else {
        // Same Winlogon boundary used by the managed Agent: a real console id
        // without a WTS user token means Windows is showing the sign-in screen.
        SetEvent(loginDesktop);
        secureProcess =
            LaunchBrokerSecureStreamer(gBrokerConfig, consoleSession);
        if (!secureProcess) {
            LogError("[connect-broker-service] Winlogon login streamer failed");
            SetEvent(gBrokerScmStopEvent);
        }
    }

    bool lastUac = false;
    bool sessionLocked = false;
    uint64_t lastSessionChangeSeq =
        gBrokerSessionChangeSeq.load(std::memory_order_acquire);
    std::chrono::steady_clock::time_point uacEnteredAt{};
    std::chrono::steady_clock::time_point secureStopRequestedAt{};

    SetBrokerServiceState(SERVICE_RUNNING);
    LogInfo("[connect-broker-service] running session=" +
        gBrokerConfig.sessionId +
        " console=" + std::to_string(consoleSession) +
        " interactive=" + std::string(interactiveReady ? "1" : "0"));

    while (WaitForSingleObject(gBrokerScmStopEvent, 0) != WAIT_OBJECT_0 &&
           WaitForSingleObject(brokerStop, 0) != WAIT_OBJECT_0) {
        const auto now = std::chrono::steady_clock::now();

        // Fail-safe: a temporary attended broker must never become permanent.
        if (now - brokerStartedAt >= std::chrono::hours(24)) {
            LogWarn("[connect-broker-service] 24h safety lifetime reached session=" +
                gBrokerConfig.sessionId);
            break;
        }

        const uint64_t sessionChangeSeq =
            gBrokerSessionChangeSeq.load(std::memory_order_acquire);
        if (sessionChangeSeq != lastSessionChangeSeq) {
            lastSessionChangeSeq = sessionChangeSeq;
            const DWORD changeType =
                gBrokerSessionChangeType.load(std::memory_order_acquire);
            const DWORD changeSession =
                gBrokerSessionChangeSessionId.load(std::memory_order_acquire);
            const bool relevant =
                changeSession == consoleSession ||
                changeSession == 0xFFFFFFFF;

            if (relevant &&
                (changeType == WTS_SESSION_LOCK ||
                 changeType == WTS_SESSION_LOGOFF)) {
                sessionLocked = true;
                SetEvent(loginDesktop);
                ResetEvent(secureStop);
                secureStopRequestedAt = {};
                if (!secureProcess) {
                    secureProcess =
                        LaunchBrokerSecureStreamer(
                            gBrokerConfig, consoleSession);
                }
                LogInfo("[connect-broker-service] Winlogon/sign-in desktop entered session=" +
                    gBrokerConfig.sessionId +
                    " event=" + std::to_string(changeType));
            }
            else if (relevant &&
                (changeType == WTS_SESSION_UNLOCK ||
                 changeType == WTS_SESSION_LOGON)) {
                sessionLocked = false;
                if (interactiveReady) ResetEvent(loginDesktop);
                if (interactiveReady && !normalProcess) {
                    ResetEvent(normalStop);
                    normalProcess =
                        LaunchBrokerNormalStreamer(
                            gBrokerConfig, consoleSession);
                }
                if (secureProcess) {
                    SetEvent(secureStop);
                    secureStopRequestedAt = now;
                }
                LogInfo("[connect-broker-service] interactive desktop session event=" +
                    std::to_string(changeType) +
                    " session=" + gBrokerConfig.sessionId);
            }
        }

        if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
            CloseHandle(parent);
            parent = nullptr;
            parentExitPending = true;
            parentExitedAt = now;
            LogInfo("[connect-broker-service] attended host process exited; checking for Windows sign-out session=" +
                gBrokerConfig.sessionId);
        }

        if (parentExitPending) {
            // SERVICE_CONTROL_SESSIONCHANGE and WTS token removal can trail the
            // user process by a few hundred milliseconds. Give Windows a short
            // grace window before deciding that a process exit was intentional.
            const bool loginBoundary =
                sessionLocked || !InteractiveUserSessionReady(consoleSession);
            if (loginBoundary) {
                parentExitPending = false;
                continuityTakeover = true;
                lastContinuityLaunchAt = now;
                continuityProcess = LaunchContinuityHost(gBrokerConfig);
                if (!continuityProcess) {
                    LogError("[connect-broker-service] failed to take over Connect signaling after sign-out session=" +
                        gBrokerConfig.sessionId);
                } else {
                    LogInfo("[connect-broker-service] continuity takeover armed session=" +
                        gBrokerConfig.sessionId);
                }
            } else if (now - parentExitedAt >= std::chrono::seconds(2)) {
                parentExitPending = false;
                LogInfo("[connect-broker-service] attended host exited outside sign-out; stopping session=" +
                    gBrokerConfig.sessionId);
                SetEvent(gBrokerScmStopEvent);
                continue;
            }
        }

        if (continuityProcess &&
            WaitForSingleObject(continuityProcess, 0) == WAIT_OBJECT_0) {
            CloseHandle(continuityProcess);
            continuityProcess = nullptr;
            LogWarn("[connect-broker-service] continuity host exited session=" +
                gBrokerConfig.sessionId);
        }

        if (continuityTakeover && !continuityProcess &&
            now - lastContinuityLaunchAt >= std::chrono::seconds(2)) {
            lastContinuityLaunchAt = now;
            continuityProcess = LaunchContinuityHost(gBrokerConfig);
        }

        if (WaitForSingleObject(cadRequest, 0) == WAIT_OBJECT_0) {
            ResetEvent(cadRequest);
            ResetEvent(cadSuccess);
            ResetEvent(cadFailure);
            const bool sent =
                SendSecureAttentionSequenceFromBroker(gBrokerConfig.sessionId);
            SetEvent(sent ? cadSuccess : cadFailure);
            LogInfo("[connect-broker-service] Ctrl+Alt+Del result session=" +
                gBrokerConfig.sessionId +
                " ok=" + std::string(sent ? "1" : "0"));
        }

        const DWORD currentConsole = WTSGetActiveConsoleSessionId();
        if (currentConsole != 0xFFFFFFFF &&
            currentConsole != consoleSession) {
            consoleSession = currentConsole;
            CloseChildProcess(normalProcess, normalStop, "normal");
            CloseChildProcess(secureProcess, secureStop, "secure");
            ResetEvent(normalStop);
            ResetEvent(secureStop);
            normalInput.ResetConsumerState();
            secureInput.ResetConsumerState();

            interactiveReady =
                InteractiveUserSessionReady(consoleSession);
            sessionLocked = false;
            if (interactiveReady) {
                ResetEvent(loginDesktop);
                normalProcess =
                    LaunchBrokerNormalStreamer(gBrokerConfig, consoleSession);
            } else {
                SetEvent(loginDesktop);
                secureProcess =
                    LaunchBrokerSecureStreamer(gBrokerConfig, consoleSession);
            }

            lastUac = false;
            uacEnteredAt = {};
            secureStopRequestedAt = {};
            LogInfo("[connect-broker-service] console handoff session=" +
                gBrokerConfig.sessionId +
                " console=" + std::to_string(consoleSession) +
                " interactive=" + std::string(interactiveReady ? "1" : "0"));
        }

        const bool interactiveNow =
            InteractiveUserSessionReady(consoleSession);
        if (interactiveNow != interactiveReady) {
            interactiveReady = interactiveNow;
            lastUac = false;
            uacEnteredAt = {};
            secureStopRequestedAt = {};

            if (!interactiveReady) {
                // Winlogon becomes authoritative. Retire the normal/default
                // helper and keep the dedicated secure helper alive.
                SetEvent(loginDesktop);
                CloseChildProcess(normalProcess, normalStop, "normal");
                ResetEvent(normalStop);
                ResetEvent(secureStop);
                normalInput.ResetConsumerState();
                secureInput.ResetConsumerState();
                if (!secureProcess) {
                    secureProcess =
                        LaunchBrokerSecureStreamer(
                            gBrokerConfig, consoleSession);
                }
                LogInfo("[connect-broker-service] Windows sign-in desktop active session=" +
                    gBrokerConfig.sessionId);
            } else {
                // A user token appeared. Warm the normal desktop while the
                // Winlogon feed remains available for the handoff. If Windows
                // still reports the console as locked, Winlogon remains the
                // authoritative visible desktop until the UNLOCK event.
                if (!sessionLocked) ResetEvent(loginDesktop);
                ResetEvent(normalStop);
                normalInput.ResetConsumerState();
                if (!normalProcess) {
                    normalProcess =
                        LaunchBrokerNormalStreamer(
                            gBrokerConfig, consoleSession);
                }
                if (secureProcess) {
                    SetEvent(secureStop);
                    secureStopRequestedAt = now;
                }
                LogInfo("[connect-broker-service] interactive desktop restored session=" +
                    gBrokerConfig.sessionId);
            }
        }

        if (normalProcess &&
            WaitForSingleObject(normalProcess, 0) == WAIT_OBJECT_0) {
            CloseHandle(normalProcess);
            normalProcess = nullptr;
            if (interactiveReady && !sessionLocked) {
                ResetEvent(normalStop);
                normalProcess =
                    LaunchBrokerNormalStreamer(
                        gBrokerConfig, consoleSession);
            }
        }

        if (secureProcess &&
            WaitForSingleObject(secureProcess, 0) == WAIT_OBJECT_0) {
            CloseHandle(secureProcess);
            secureProcess = nullptr;
            secureStopRequestedAt = {};
            if (!interactiveReady || sessionLocked) {
                ResetEvent(secureStop);
                secureProcess =
                    LaunchBrokerSecureStreamer(
                        gBrokerConfig, consoleSession);
            }
        }

        const bool uac =
            interactiveReady && !sessionLocked &&
            normalInput.GetUACActive();
        if (uac != lastUac) {
            lastUac = uac;
            if (uac) {
                uacEnteredAt = now;
                secureStopRequestedAt = {};
                ResetEvent(secureStop);
                LogInfo("[connect-broker-service] secure desktop requested session=" +
                    gBrokerConfig.sessionId);
            } else {
                uacEnteredAt = {};
                if (interactiveReady && !sessionLocked && secureProcess) {
                    SetEvent(secureStop);
                    secureStopRequestedAt = now;
                }
            }
        }

        // Mirror the installed Agent. Winlogon is immediate when no interactive
        // user exists; UAC gives the dynamic LocalSystem streamer 220 ms before
        // warming the dedicated winsta0\\Winlogon helper.
        if ((!interactiveReady || sessionLocked) && !secureProcess) {
            ResetEvent(secureStop);
            secureProcess =
                LaunchBrokerSecureStreamer(
                    gBrokerConfig, consoleSession);
        } else if (uac && !secureProcess &&
            uacEnteredAt.time_since_epoch().count() != 0 &&
            now - uacEnteredAt >= std::chrono::milliseconds(220)) {
            ResetEvent(secureStop);
            secureProcess =
                LaunchBrokerSecureStreamer(
                    gBrokerConfig, consoleSession);
        }

        if (interactiveReady && !sessionLocked && !uac && secureProcess &&
            secureStopRequestedAt.time_since_epoch().count() != 0 &&
            now - secureStopRequestedAt >= std::chrono::milliseconds(1200) &&
            normalInput.GetMonitorCount() > 0) {
            CloseChildProcess(secureProcess, secureStop, "secure");
            secureStopRequestedAt = {};
        }

        Sleep(25);
    }

    SetBrokerServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    CloseChildProcess(secureProcess, secureStop, "secure");
    CloseChildProcess(normalProcess, normalStop, "normal");
    if (continuityProcess) {
        if (WaitForSingleObject(continuityProcess, 600) != WAIT_OBJECT_0) {
            TerminateProcess(continuityProcess, 0);
            WaitForSingleObject(continuityProcess, 500);
        }
        CloseHandle(continuityProcess);
        continuityProcess = nullptr;
    }

    normalInput.Close();
    secureInput.Close();
    normalFrames.Close();
    secureFrames.Close();
    if (parent) CloseHandle(parent);
    CloseHandle(brokerStop);
    CloseHandle(normalStop);
    CloseHandle(secureStop);
    CloseHandle(loginDesktop);
    CloseHandle(cadRequest);
    CloseHandle(cadSuccess);
    CloseHandle(cadFailure);
    CloseHandle(gBrokerScmStopEvent);
    gBrokerScmStopEvent = nullptr;

    SetBrokerServiceState(SERVICE_STOPPED);
    DeleteOwnBrokerService(gBrokerConfig.serviceName);
    LogInfo("[connect-broker-service] stopped session=" +
        gBrokerConfig.sessionId);
}

int ParseIntArg(
    int argc, char** argv,
    const std::string& name,
    int fallback) {
    const std::string value = ArgValue(argc, argv, name);
    if (value.empty()) return fallback;
    try { return std::stoi(value); }
    catch (...) { return fallback; }
}

} // namespace

int RunConnectCaptureBrokerService(int argc, char** argv) {
    gBrokerConfig = {};
    gBrokerConfig.serviceName =
        ArgValue(argc, argv, "--service-name");
    gBrokerConfig.sessionId =
        ArgValue(argc, argv, "--session");
    gBrokerConfig.normalShmem =
        ArgValue(argc, argv, "--normal-shmem");
    gBrokerConfig.secureShmem =
        ArgValue(argc, argv, "--secure-shmem");
    gBrokerConfig.normalInput =
        ArgValue(argc, argv, "--normal-input");
    gBrokerConfig.secureInput =
        ArgValue(argc, argv, "--secure-input");
    gBrokerConfig.normalStop =
        ArgValue(argc, argv, "--normal-stop");
    gBrokerConfig.secureStop =
        ArgValue(argc, argv, "--secure-stop");
    gBrokerConfig.brokerStop =
        ArgValue(argc, argv, "--broker-stop");
    gBrokerConfig.loginDesktop =
        ArgValue(argc, argv, "--login-desktop");
    gBrokerConfig.cadRequest =
        ArgValue(argc, argv, "--cad-request");
    gBrokerConfig.cadSuccess =
        ArgValue(argc, argv, "--cad-success");
    gBrokerConfig.cadFailure =
        ArgValue(argc, argv, "--cad-failure");
    gBrokerConfig.connectTicket =
        ArgValue(argc, argv, "--connect-ticket");
    gBrokerConfig.fps =
        std::clamp(ParseIntArg(argc, argv, "--fps", 30), 1, 60);
    gBrokerConfig.display =
        std::max(0, ParseIntArg(argc, argv, "--display", 0));
    gBrokerConfig.parentPid =
        static_cast<DWORD>(std::max(
            0, ParseIntArg(argc, argv, "--parent-pid", 0)));

    if (gBrokerConfig.serviceName.empty() ||
        gBrokerConfig.sessionId.empty() ||
        gBrokerConfig.normalShmem.empty() ||
        gBrokerConfig.secureShmem.empty() ||
        gBrokerConfig.normalInput.empty() ||
        gBrokerConfig.secureInput.empty() ||
        gBrokerConfig.normalStop.empty() ||
        gBrokerConfig.secureStop.empty() ||
        gBrokerConfig.brokerStop.empty() ||
        gBrokerConfig.loginDesktop.empty() ||
        gBrokerConfig.cadRequest.empty() ||
        gBrokerConfig.cadSuccess.empty() ||
        gBrokerConfig.cadFailure.empty() ||
        gBrokerConfig.connectTicket.empty()) {
        LogError("[connect-broker-service] missing required arguments");
        return 2;
    }

    std::wstring serviceName = Wide(gBrokerConfig.serviceName);
    SERVICE_TABLE_ENTRYW table[] = {
        {
            serviceName.data(),
            &ConnectBrokerServiceMain
        },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD err = GetLastError();
        LogError("[connect-broker-service] dispatcher failed err=" +
            std::to_string(err));
        return static_cast<int>(err ? err : 3);
    }
    return 0;
}

} // namespace hi5

#endif // _WIN32
