#include "frame_source.h"
#include "ipc/shmem_ring.h"
#include "ipc/input_pipe.h"
#include "util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <csignal>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

    std::atomic<bool> g_running{ true };

    void SignalHandler(int) {
        g_running = false;
    }

    struct Args {
        std::string sessionId;
        std::string shmemName;
        std::string inputPipeName;
        std::string stopEventName;
        int fps = 45;
        int display = 0;
        bool dynamicDesktop = false;
    };

    std::optional<std::string> GetArgValue(int argc, char** argv, const std::string& key) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == key) {
                return std::string(argv[i + 1]);
            }
        }
        return std::nullopt;
    }

    bool HasArg(int argc, char** argv, const std::string& key) {
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == key) return true;
        }
        return false;
    }

    Args ParseArgs(int argc, char** argv) {
        Args a;
        if (auto v = GetArgValue(argc, argv, "--session")) a.sessionId = *v;
        if (auto v = GetArgValue(argc, argv, "--shmem")) a.shmemName = *v;
        if (auto v = GetArgValue(argc, argv, "--input-pipe")) a.inputPipeName = *v;
        if (auto v = GetArgValue(argc, argv, "--stop-event")) a.stopEventName = *v;
        if (auto v = GetArgValue(argc, argv, "--fps")) a.fps = std::max(1, std::stoi(*v));
        if (auto v = GetArgValue(argc, argv, "--display")) a.display = std::stoi(*v);
        a.dynamicDesktop = HasArg(argc, argv, "--dynamic-desktop");
        return a;
    }

    bool IsSecureDesktopActive() {
        HDESK hdesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
        if (!hdesk) {
            return true;
        }

        char name[256]{};
        DWORD needed = 0;
        bool secure = true;
        if (GetUserObjectInformationA(hdesk, UOI_NAME, name, sizeof(name), &needed)) {
            const std::string deskName = name;
            secure = !(deskName == "Default" || deskName == "default");
        }
        CloseDesktop(hdesk);
        return secure;
    }

    std::string DesktopName(HDESK desktop) {
        if (!desktop) return {};
        char name[256]{};
        DWORD needed = 0;
        if (!GetUserObjectInformationA(desktop, UOI_NAME, name, sizeof(name), &needed)) return {};
        return std::string(name);
    }

    bool IsSecureDesktopName(const std::string& name) {
        return !(name == "Default" || name == "default");
    }

    bool SyncThreadToActiveInputDesktop(HDESK& ownedDesktop,
        std::string& attachedDesktopName,
        bool& secureOut,
        bool& changedOut) {
        changedOut = false;
        HDESK inputDesktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!inputDesktop) return false;

        const std::string inputName = DesktopName(inputDesktop);
        secureOut = inputName.empty() ? true : IsSecureDesktopName(inputName);
        if (!inputName.empty() && inputName == attachedDesktopName) {
            CloseDesktop(inputDesktop);
            return true;
        }

        if (!SetThreadDesktop(inputDesktop)) {
            CloseDesktop(inputDesktop);
            return false;
        }

        HDESK previousOwnedDesktop = ownedDesktop;
        ownedDesktop = inputDesktop;
        attachedDesktopName = inputName;
        changedOut = true;
        if (previousOwnedDesktop) CloseDesktop(previousOwnedDesktop);
        return true;
    }

    bool IsSecureHelperArgs(const Args& args) {
        return args.shmemName.find("_UAC_") != std::string::npos ||
            args.inputPipeName.find("_UAC_") != std::string::npos ||
            args.stopEventName.find("_UAC_") != std::string::npos;
    }

    void PublishMonitorInfo(hi5::InputPipeReader& pipe, DesktopFrameSource& source) {
        const auto displays = source.listDisplays();
        hi5::InputRingHeader::MonitorInfo infos[8]{};
        const int n = static_cast<int>(std::min<size_t>(displays.size(), 8));
        for (int i = 0; i < n; ++i) {
            infos[i].x = displays[i].x;
            infos[i].y = displays[i].y;
            infos[i].w = displays[i].width;
            infos[i].h = displays[i].height;
            infos[i].primary = displays[i].primary ? 1 : 0;
        }
        pipe.SetMonitorInfo(n, infos);
    }

    void PrimeInputQueue() {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(INPUT));
    }

    uint64_t UnixMsNow() {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    const char* StreamModeName(int mode) {
        switch (mode) {
        case 2: return "motion";
        case 1: return "active";
        default: return "idle";
        }
    }

    DWORD MouseButtonDownFlag(uint8_t button) {
        switch (button) {
        case 0: return MOUSEEVENTF_LEFTDOWN;
        case 1: return MOUSEEVENTF_MIDDLEDOWN;
        case 2: return MOUSEEVENTF_RIGHTDOWN;
        default: return 0;
        }
    }

    DWORD MouseButtonUpFlag(uint8_t button) {
        switch (button) {
        case 0: return MOUSEEVENTF_LEFTUP;
        case 1: return MOUSEEVENTF_MIDDLEUP;
        case 2: return MOUSEEVENTF_RIGHTUP;
        default: return 0;
        }
    }

    void SendMouseAbsolute(int x, int y) {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 0 || vh <= 0) return;

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = static_cast<LONG>(((static_cast<double>(x - vx) * 65535.0) / std::max(1, vw - 1)));
        in.mi.dy = static_cast<LONG>(((static_cast<double>(y - vy) * 65535.0) / std::max(1, vh - 1)));
        SendInput(1, &in, sizeof(INPUT));
    }


    bool ApplyFastMouseTarget(const hi5::FastMouseTarget& target,
        const std::string& sessionId,
        std::chrono::steady_clock::time_point& nextLog,
        uint64_t& appliedCount) {
        const auto before = std::chrono::steady_clock::now();

        // The streamer runs in the interactive user's desktop/session, so this
        // is the correct place for native cursor movement. SetCursorPos is used
        // first because it is direct and avoids the normal SendInput queue. If
        // Windows rejects it for any reason, fall back to absolute SendInput.
        BOOL ok = SetCursorPos(target.x, target.y);

        if (!ok) {
            SendMouseAbsolute(target.x, target.y);
            ok = TRUE; // SendInput has no reliable per-call cursor success result.
        }

        const auto after = std::chrono::steady_clock::now();
        ++appliedCount;

        const auto now = std::chrono::steady_clock::now();
        if (nextLog.time_since_epoch().count() == 0 || now >= nextLog) {
            const auto applyUs = std::chrono::duration_cast<std::chrono::microseconds>(after - before).count();
            const uint64_t nowMs = static_cast<uint64_t>(GetTickCount64());
            const uint64_t serviceToApplyMs = (target.unixMs > 0 && nowMs >= target.unixMs) ? (nowMs - target.unixMs) : 0;
            LogInfo("[streamer] mouse fast apply session=" + sessionId +
                " seq=" + std::to_string(target.seq) +
                " monitor=" + std::to_string(target.monitorIndex) +
                " x=" + std::to_string(target.x) +
                " y=" + std::to_string(target.y) +
                " ok=" + std::string(ok ? "1" : "0") +
                " apply_us=" + std::to_string(applyUs) +
                " service_to_apply_ms=" + std::to_string(serviceToApplyMs) +
                " count=" + std::to_string(appliedCount) +
                " client_ts_ms=" + std::to_string(target.clientTsMs));
            nextLog = now + std::chrono::seconds(2);
        }

        return ok != FALSE;
    }

    void SendMouseButton(uint8_t button, bool down) {
        DWORD flag = down ? MouseButtonDownFlag(button) : MouseButtonUpFlag(button);
        if (!flag) return;
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = flag;
        SendInput(1, &in, sizeof(INPUT));
    }

    void SendMouseWheel(int deltaX, int deltaY) {
        if (deltaY != 0) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            int clicks = deltaY / 120;
            if (clicks == 0) clicks = (deltaY > 0) ? 1 : -1;
            in.mi.mouseData = static_cast<DWORD>(-clicks * WHEEL_DELTA);
            SendInput(1, &in, sizeof(INPUT));
        }
        if (deltaX != 0) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            int clicks = deltaX / 120;
            if (clicks == 0) clicks = (deltaX > 0) ? 1 : -1;
            in.mi.mouseData = static_cast<DWORD>(clicks * WHEEL_DELTA);
            SendInput(1, &in, sizeof(INPUT));
        }
    }

    void SendKeyEvent(const hi5::InputCmd& cmd) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = cmd.key.scanCode;
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (!cmd.key.down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
        if (cmd.key.isExtended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof(INPUT));
    }

    std::wstring Utf8ToWide(const std::string& value) {
        if (value.empty()) return {};
        int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
        return out;
    }

    std::string WideToUtf8(const wchar_t* value, int length) {
        if (!value || length <= 0) return {};
        int needed = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, length, out.data(), needed, nullptr, nullptr);
        return out;
    }

    bool SetRemoteClipboardTextUtf8(const std::string& text) {
        const std::wstring wide = Utf8ToWide(text);
        if (wide.empty() && !text.empty()) return false;

        for (int attempt = 0; attempt < 25; ++attempt) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (!h) {
                    CloseClipboard();
                    return false;
                }
                void* dst = GlobalLock(h);
                if (!dst) {
                    GlobalFree(h);
                    CloseClipboard();
                    return false;
                }
                std::memcpy(dst, wide.c_str(), bytes);
                GlobalUnlock(h);
                if (!SetClipboardData(CF_UNICODETEXT, h)) {
                    GlobalFree(h);
                    CloseClipboard();
                    return false;
                }
                CloseClipboard();
                return true;
            }
            Sleep(10);
        }
        return false;
    }

    bool GetRemoteClipboardTextUtf8(std::string& out) {
        out.clear();
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (OpenClipboard(nullptr)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
                    if (w) {
                        const int len = static_cast<int>(wcslen(w));
                        out = WideToUtf8(w, len);
                        GlobalUnlock(h);
                        CloseClipboard();
                        return true;
                    }
                }
                h = GetClipboardData(CF_TEXT);
                if (h) {
                    const char* a = static_cast<const char*>(GlobalLock(h));
                    if (a) {
                        out.assign(a);
                        GlobalUnlock(h);
                        CloseClipboard();
                        return true;
                    }
                }
                CloseClipboard();
                return false;
            }
            Sleep(10);
        }
        return false;
    }

    void SendCtrlC() {
        INPUT in[4]{};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'C';
        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'C'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
    }

    void SendCtrlV() {
        INPUT in[4]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = 'V';
        in[2].type = INPUT_KEYBOARD;
        in[2].ki.wVk = 'V';
        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD;
        in[3].ki.wVk = VK_CONTROL;
        in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
    }

    void SendCtrlAltDelFallback() {
        INPUT in[6]{};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_MENU;
        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = VK_DELETE; in[2].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_DELETE; in[3].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        in[4].type = INPUT_KEYBOARD; in[4].ki.wVk = VK_MENU; in[4].ki.dwFlags = KEYEVENTF_KEYUP;
        in[5].type = INPUT_KEYBOARD; in[5].ki.wVk = VK_CONTROL; in[5].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(6, in, sizeof(INPUT));
    }

    void SendStartMenuToggle() {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_LWIN;
        in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LWIN, MAPVK_VK_TO_VSC));
        in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = VK_LWIN;
        in[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LWIN, MAPVK_VK_TO_VSC));
        in[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }

    bool IsExtendedVk(WORD vk) {
        switch (vk) {
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
            return true;
        default:
            return false;
        }
    }

    void FillKey(INPUT& in, WORD vk, bool down) {
        in = INPUT{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        DWORD flags = 0;
        if (!down) flags |= KEYEVENTF_KEYUP;
        if (IsExtendedVk(vk)) flags |= KEYEVENTF_EXTENDEDKEY;
        in.ki.dwFlags = flags;
    }

    void SendKeyCombo(const std::vector<WORD>& keys, DWORD holdMs = 80) {
        if (keys.empty()) return;
        std::vector<INPUT> inputs;
        inputs.resize(keys.size() * 2);
        for (size_t i = 0; i < keys.size(); ++i) {
            FillKey(inputs[i], keys[i], true);
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            const size_t src = keys.size() - 1 - i;
            FillKey(inputs[keys.size() + i], keys[src], false);
        }
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        if (holdMs) Sleep(holdMs);
    }

    void SendWinCombo(WORD vk) { SendKeyCombo({ VK_LWIN, vk }); }
    void SendAltCombo(WORD vk) { SendKeyCombo({ VK_MENU, vk }, 140); }

    void SendSingleKey(WORD vk) {
        INPUT inputs[2]{};
        FillKey(inputs[0], vk, true);
        FillKey(inputs[1], vk, false);
        SendInput(2, inputs, sizeof(INPUT));
    }

    void SendAltDown() {
        INPUT input{};
        FillKey(input, VK_MENU, true);
        SendInput(1, &input, sizeof(INPUT));
    }

    void SendAltUp() {
        INPUT input{};
        FillKey(input, VK_MENU, false);
        SendInput(1, &input, sizeof(INPUT));
    }

    bool gRemoteAltTabActive = false;

    void BeginRemoteAltTab() {
        if (!gRemoteAltTabActive) {
            SendAltDown();
            Sleep(40);
            gRemoteAltTabActive = true;
        }
        SendSingleKey(VK_TAB);
    }

    void NextRemoteAltTab() {
        if (!gRemoteAltTabActive) {
            BeginRemoteAltTab();
            return;
        }
        SendSingleKey(VK_TAB);
    }

    void EndRemoteAltTab() {
        if (gRemoteAltTabActive) {
            SendAltUp();
            gRemoteAltTabActive = false;
        }
    }

    void SendCtrlShiftEsc() { SendKeyCombo({ VK_CONTROL, VK_SHIFT, VK_ESCAPE }); }
    void SendCtrlEsc() { SendKeyCombo({ VK_CONTROL, VK_ESCAPE }); }

    bool EnableSoftwareSasForServicesFromStreamer() {
        HKEY key = nullptr;
        constexpr const wchar_t* kPath = L"SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System";
        DWORD disp = 0;
        LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kPath, 0, nullptr, 0, KEY_READ | KEY_SET_VALUE, nullptr, &key, &disp);
        if (rc != ERROR_SUCCESS) return false;
        DWORD value = 0; DWORD cb = sizeof(value); DWORD type = REG_DWORD;
        rc = RegQueryValueExW(key, L"SoftwareSASGeneration", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &cb);
        if (rc != ERROR_SUCCESS || type != REG_DWORD) value = 0;
        const DWORD desired = value | 0x3;
        bool ok = true;
        if (desired != value || rc != ERROR_SUCCESS) {
            ok = RegSetValueExW(key, L"SoftwareSASGeneration", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&desired), sizeof(desired)) == ERROR_SUCCESS;
        }
        RegCloseKey(key);
        return ok;
    }

    bool TrySendSecureAttentionSequenceFromStreamer(const std::string& sessionId) {
        EnableSoftwareSasForServicesFromStreamer();
        using SendSasFn = void (WINAPI *)(BOOL);
        HMODULE sas = LoadLibraryW(L"sas.dll");
        if (!sas) {
            LogWarn("[streamer] ctrl_alt_del sas.dll unavailable session=" + sessionId +
                " err=" + std::to_string(GetLastError()));
            return false;
        }
        auto fn = reinterpret_cast<SendSasFn>(GetProcAddress(sas, "SendSAS"));
        if (!fn) {
            const DWORD err = GetLastError();
            FreeLibrary(sas);
            LogWarn("[streamer] ctrl_alt_del SendSAS unavailable session=" + sessionId +
                " err=" + std::to_string(err));
            return false;
        }
        LogInfo("[streamer] ctrl_alt_del trying SendSAS(FALSE) session=" + sessionId);
        fn(FALSE);
        Sleep(120);
        LogInfo("[streamer] ctrl_alt_del trying SendSAS(TRUE) session=" + sessionId);
        fn(TRUE);
        FreeLibrary(sas);
        return true;
    }

    void LaunchTaskManager() {
        ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }

    void LaunchExplorer() {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }

    void HandleShortcut(const hi5::InputCmd& cmd, const std::string& sessionId) {
        const auto action = static_cast<hi5::ShortcutAction>(cmd.shortcut.action);
        switch (action) {
        case hi5::ShortcutAction::CtrlAltDel: {
            const bool sasAttempted = TrySendSecureAttentionSequenceFromStreamer(sessionId);
            // Normal injection is usually blocked by Windows for SAS, but it is a harmless fallback
            // and helps in nested/VM sessions where Ctrl+Alt+Del may be accepted as normal input.
            SendCtrlAltDelFallback();
            LogInfo("[streamer] shortcut ctrl_alt_del session=" + sessionId +
                " sas_attempted=" + std::string(sasAttempted ? "1" : "0") + " fallback_input=1");
            break;
        }
        case hi5::ShortcutAction::LockWorkstation: {
            const BOOL ok = LockWorkStation();
            LogInfo("[streamer] shortcut lock_workstation session=" + sessionId +
                " ok=" + std::string(ok ? "1" : "0") +
                " err=" + std::to_string(ok ? 0 : GetLastError()));
            break;
        }
        case hi5::ShortcutAction::TaskManager:
            LaunchTaskManager();
            LogInfo("[streamer] shortcut taskmgr session=" + sessionId);
            break;
        case hi5::ShortcutAction::Explorer:
            LaunchExplorer();
            LogInfo("[streamer] shortcut explorer session=" + sessionId);
            break;
        case hi5::ShortcutAction::StartMenu:
            SendStartMenuToggle();
            LogInfo("[streamer] shortcut start_menu session=" + sessionId);
            break;
        case hi5::ShortcutAction::WinD:
            SendWinCombo('D');
            LogInfo("[streamer] shortcut win_d session=" + sessionId);
            break;
        case hi5::ShortcutAction::WinR:
            SendWinCombo('R');
            LogInfo("[streamer] shortcut win_r session=" + sessionId);
            break;
        case hi5::ShortcutAction::WinE:
            SendWinCombo('E');
            LogInfo("[streamer] shortcut win_e session=" + sessionId);
            break;
        case hi5::ShortcutAction::WinTab:
            SendWinCombo(VK_TAB);
            LogInfo("[streamer] shortcut win_tab session=" + sessionId);
            break;
        case hi5::ShortcutAction::AltTab:
            BeginRemoteAltTab();
            Sleep(400);
            EndRemoteAltTab();
            LogInfo("[streamer] shortcut alt_tab legacy tap session=" + sessionId);
            break;
        case hi5::ShortcutAction::AltTabBegin:
            BeginRemoteAltTab();
            LogInfo("[streamer] shortcut alt_tab_begin session=" + sessionId);
            break;
        case hi5::ShortcutAction::AltTabNext:
            NextRemoteAltTab();
            LogInfo("[streamer] shortcut alt_tab_next session=" + sessionId);
            break;
        case hi5::ShortcutAction::AltTabEnd:
            EndRemoteAltTab();
            LogInfo("[streamer] shortcut alt_tab_end session=" + sessionId);
            break;
        case hi5::ShortcutAction::AltF4:
            SendAltCombo(VK_F4);
            LogInfo("[streamer] shortcut alt_f4 session=" + sessionId);
            break;
        case hi5::ShortcutAction::CtrlShiftEsc:
            SendCtrlShiftEsc();
            LogInfo("[streamer] shortcut ctrl_shift_esc session=" + sessionId);
            break;
        case hi5::ShortcutAction::CtrlEsc:
            SendCtrlEsc();
            LogInfo("[streamer] shortcut ctrl_esc session=" + sessionId);
            break;
        default:
            LogWarn("[streamer] unknown shortcut action session=" + sessionId +
                " action=" + std::to_string(static_cast<unsigned>(cmd.shortcut.action)));
            break;
        }
    }

    void SendUnicodeTextUtf8(const std::string& text) {
        const std::wstring wide = Utf8ToWide(text);
        for (wchar_t ch : wide) {
            if (ch == L'\0') continue;
            INPUT in[2]{};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wScan = ch;
            in[0].ki.dwFlags = KEYEVENTF_UNICODE;
            in[1].type = INPUT_KEYBOARD;
            in[1].ki.wScan = ch;
            in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            Sleep(1);
        }
    }

    bool HandleInputPipe(hi5::InputPipeReader& pipe,
        DesktopFrameSource& source,
        int& currentDisplay,
        const std::string& sessionId) {
        // Low-latency input: mouse movement is latest-state only. Collapse
        // runs of MouseMove commands so clicks/keys are not delayed behind a
        // backlog of stale pointer locations.
        hi5::InputCmd cmd{};
        hi5::InputCmd pendingMove{};
        bool havePendingMove = false;
        int drained = 0;
        constexpr int kMaxDrainPerTick = 1024;

        auto flushMove = [&]() {
            if (havePendingMove) {
                SendMouseAbsolute(pendingMove.mouseMove.x, pendingMove.mouseMove.y);
                havePendingMove = false;
            }
        };

        bool hadInput = false;
        while (drained < kMaxDrainPerTick && pipe.Read(cmd)) {
            ++drained;
            hadInput = true;
            if (cmd.type == hi5::InputCmdType::MouseMove) {
                pendingMove = cmd;
                havePendingMove = true;
                continue;
            }

            flushMove();

            switch (cmd.type) {
            case hi5::InputCmdType::MouseButton:
                SendMouseButton(cmd.mouseButton.button, cmd.mouseButton.down != 0);
                break;
            case hi5::InputCmdType::MouseWheel:
                SendMouseWheel(cmd.mouseWheel.deltaX, cmd.mouseWheel.deltaY);
                break;
            case hi5::InputCmdType::KeyEvent:
                SendKeyEvent(cmd);
                break;
            case hi5::InputCmdType::ClipboardSet:
            case hi5::InputCmdType::ClipboardPaste:
            case hi5::InputCmdType::PasteText: {
                std::string text;
                if (!pipe.ReadClipboard(cmd.clipboard.offsetInClip, cmd.clipboard.length, text)) {
                    LogWarn("[streamer] clipboard command failed to read text session=" + sessionId);
                    break;
                }
                if (cmd.type == hi5::InputCmdType::PasteText) {
                    SendUnicodeTextUtf8(text);
                    LogInfo("[streamer] paste_text injected session=" + sessionId + " bytes=" + std::to_string(text.size()));
                } else {
                    const bool ok = SetRemoteClipboardTextUtf8(text);
                    LogInfo("[streamer] clipboard set session=" + sessionId + " bytes=" + std::to_string(text.size()) + " ok=" + std::string(ok ? "1" : "0"));
                    if (ok && cmd.type == hi5::InputCmdType::ClipboardPaste) {
                        Sleep(20);
                        SendCtrlV();
                    }
                }
                break;
            }
            case hi5::InputCmdType::ClipboardGet: {
                SendCtrlC();
                Sleep(80);
                std::string text;
                const bool ok = GetRemoteClipboardTextUtf8(text);
                pipe.PublishClipboardResponse(text, ok);
                LogInfo("[streamer] clipboard_get session=" + sessionId + " ok=" + std::string(ok ? "1" : "0") + " bytes=" + std::to_string(text.size()));
                break;
            }
            case hi5::InputCmdType::Shortcut:
                HandleShortcut(cmd, sessionId);
                break;
            case hi5::InputCmdType::SwitchMonitor: {
                const int requested = cmd.switchMonitor.monitorIndex;
                if (source.setDisplayIndex(requested)) {
                    currentDisplay = requested;
                    PublishMonitorInfo(pipe, source);
                    LogInfo("[streamer] switched monitor session=" + sessionId +
                        " display=" + std::to_string(requested));
                }
                else {
                    LogWarn("[streamer] failed monitor switch session=" + sessionId +
                        " display=" + std::to_string(requested));
                }
                break;
            }
            default:
                break;
            }
        }

        flushMove();

        if (drained >= kMaxDrainPerTick) {
            LogWarn("[streamer] input drain capped session=" + sessionId +
                " drained=" + std::to_string(drained));
        }

        return hadInput;
    }

    int ReadEnvInt(const char* name, int fallbackValue, int minValue, int maxValue) {
        char buf[32]{};
        DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf)) return fallbackValue;
        try {
            int v = std::stoi(std::string(buf, buf + n));
            return std::max(minValue, std::min(maxValue, v));
        }
        catch (...) {
            return fallbackValue;
        }
    }

} // namespace

namespace hi5 {

    int RunStreamerMain(int argc, char** argv) {
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        const Args args = ParseArgs(argc, argv);
        if (args.sessionId.empty() || args.shmemName.empty()) {
            LogError("[streamer] missing required args");
            return 1;
        }

        LogInfo("[streamer] start RAW-I420 session=" + args.sessionId +
            " shmem=" + args.shmemName +
            " input=" + args.inputPipeName +
            " fps=" + std::to_string(args.fps) +
            " display=" + std::to_string(args.display));

        HANDLE stopEvent = nullptr;
        if (!args.stopEventName.empty()) {
            stopEvent = OpenEventA(SYNCHRONIZE, FALSE, args.stopEventName.c_str());
            if (!stopEvent) {
                LogWarn("[streamer] failed to open stop event err=" + std::to_string(GetLastError()));
            }
        }

        ShmemRing shmem;
        if (!shmem.OpenProducer(args.shmemName)) {
            LogError("[streamer] failed to open shmem producer err=" + std::to_string(GetLastError()));
            if (stopEvent) CloseHandle(stopEvent);
            return 1;
        }
        LogInfo("[streamer] raw shmem ring ready");

        hi5::InputPipeReader inputPipe;
        bool inputPipeOk = false;
        if (!args.inputPipeName.empty()) {
            inputPipeOk = inputPipe.Open(args.inputPipeName);
            if (!inputPipeOk) {
                LogWarn("[streamer] failed to open input pipe err=" + std::to_string(GetLastError()));
            }
            else {
                LogInfo("[streamer] input pipe open");
            }
        }

        PrimeInputQueue();
        LogInfo("[streamer] input queue primed");

        DesktopFrameSource source;
        source.setDisplayIndex(args.display);
        if (inputPipeOk) {
            PublishMonitorInfo(inputPipe, source);
        }

        int currentDisplay = args.display;
        bool lastSecureState = false;
        const bool isSecureHelper = IsSecureHelperArgs(args);
        const bool dynamicDesktop = args.dynamicDesktop && !isSecureHelper;
        HDESK ownedDynamicDesktop = nullptr;
        std::string attachedDesktopName = DesktopName(GetThreadDesktop(GetCurrentThreadId()));
        bool secureDesktopEnding = false;
        std::chrono::steady_clock::time_point secureDesktopEndedAt{};
        std::chrono::steady_clock::time_point nextDesktopSwitchErrorLog{};
        int consecutiveResetFailures = 0;

        auto nextMonitorPublish = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        auto nextSecurePoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);

        const int requestedMaxFps = std::max(1, args.fps);
        const int activeFps = std::min(requestedMaxFps, ReadEnvInt("HI5_STREAM_ACTIVE_FPS", 20, 1, 60));
        const int idleFps = std::min(activeFps, ReadEnvInt("HI5_STREAM_IDLE_FPS", 2, 1, 15));
        const int motionFps = std::min(requestedMaxFps, ReadEnvInt("HI5_STREAM_MOTION_FPS", 30, 1, 60));
        const auto activeHold = std::chrono::milliseconds(ReadEnvInt("HI5_STREAM_ACTIVE_HOLD_MS", 900, 100, 5000));
        const auto statsEvery = std::chrono::seconds(ReadEnvInt("HI5_STREAM_STATS_SECONDS", 5, 1, 60));
        auto nextCaptureAt = std::chrono::steady_clock::now();
        auto nextStatsAt = std::chrono::steady_clock::now() + statsEvery;
        auto lastInputAt = std::chrono::steady_clock::now() - std::chrono::seconds(60);
        auto lastChangedFrameAt = std::chrono::steady_clock::now() - std::chrono::seconds(60);
        uint64_t captureAttempts = 0;
        uint64_t changedFrames = 0;
        uint64_t skippedFrames = 0;
        uint64_t writtenFrames = 0;
        uint64_t inputEvents = 0;
        uint64_t fastMouseApplied = 0;
        uint64_t fastMouseLastSeq = 0;
        uint64_t cursorOnlyFrames = 0; // intentionally disabled; keep log field at 0
        std::chrono::steady_clock::time_point fastMouseNextLog{};
        const int cursorRefreshFps = 0; // disabled: mouse movement must not force video frames
        uint64_t resetCount = 0;
        int lastTargetFps = idleFps;
        int lastStreamMode = 0;

        LogInfo("[streamer] low-cpu mode session=" + args.sessionId +
            " requested_fps=" + std::to_string(requestedMaxFps) +
            " active_fps=" + std::to_string(activeFps) +
            " idle_fps=" + std::to_string(idleFps) +
            " motion_fps=" + std::to_string(motionFps) +
            " cursor_refresh=disabled");

        while (g_running.load()) {
            if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                LogInfo("[streamer] stop event signaled");
                break;
            }

            const auto loopStart = std::chrono::steady_clock::now();

            bool hadInput = false;
            bool hadFastMouse = false;
            if (inputPipeOk) {
                hi5::FastMouseTarget fastTarget{};
                while (inputPipe.ReadFastMouseTarget(fastMouseLastSeq, fastTarget)) {
                    hadFastMouse = true;
                    ApplyFastMouseTarget(fastTarget, args.sessionId, fastMouseNextLog, fastMouseApplied);
                }

                hadInput = HandleInputPipe(inputPipe, source, currentDisplay, args.sessionId);
                if (hadFastMouse) {
                    // Fast mouse movement is applied directly in the interactive streamer.
                    // Do not treat mouse-only movement as video activity: forcing captures
                    // for cursor motion was the main reason CPU climbed after the cursor experiments.
                }

                if (hadInput) {
                    ++inputEvents;
                    lastInputAt = std::chrono::steady_clock::now();
                    nextCaptureAt = lastInputAt;
                }
            }

            const auto now = std::chrono::steady_clock::now();

            if (inputPipeOk && now >= nextMonitorPublish) {
                PublishMonitorInfo(inputPipe, source);
                nextMonitorPublish = now + std::chrono::seconds(1);
            }

            if (inputPipeOk && now >= nextSecurePoll) {
                bool secureNow = IsSecureDesktopActive();
                if (dynamicDesktop) {
                    bool switchedDesktop = false;
                    bool attachedSecure = secureNow;
                    if (SyncThreadToActiveInputDesktop(ownedDynamicDesktop, attachedDesktopName, attachedSecure, switchedDesktop)) {
                        secureNow = attachedSecure;
                        if (switchedDesktop) {
                            try {
                                DesktopFrameSource resetSource;
                                resetSource.setDisplayIndex(currentDisplay);
                                source = std::move(resetSource);
                                PublishMonitorInfo(inputPipe, source);
                                nextCaptureAt = now;
                                consecutiveResetFailures = 0;
                                PrimeInputQueue();
                                LogInfo("[streamer] capture thread switched input desktop session=" + args.sessionId +
                                    " desktop=" + attachedDesktopName +
                                    " secure=" + std::to_string(secureNow ? 1 : 0));
                            }
                            catch (const std::exception& ex) {
                                LogWarn("[streamer] capture reset after desktop switch failed session=" + args.sessionId +
                                    ": " + ex.what());
                            }
                        }
                    }
                    else if (nextDesktopSwitchErrorLog.time_since_epoch().count() == 0 || now >= nextDesktopSwitchErrorLog) {
                        LogWarn("[streamer] unable to attach capture thread to active input desktop session=" + args.sessionId +
                            " err=" + std::to_string(GetLastError()));
                        nextDesktopSwitchErrorLog = now + std::chrono::seconds(1);
                    }
                }

                inputPipe.SetUACActive(secureNow);
                if (secureNow != lastSecureState) {
                    lastSecureState = secureNow;
                    consecutiveResetFailures = 0;
                    PrimeInputQueue();

                    if (isSecureHelper && !secureNow) {
                        secureDesktopEnding = true;
                        secureDesktopEndedAt = now;
                    }
                    else if (secureNow) {
                        secureDesktopEnding = false;
                    }

                    LogInfo("[streamer] desktop transition session=" + args.sessionId +
                        " secure=" + std::to_string(secureNow ? 1 : 0) +
                        " dynamic=" + std::to_string(dynamicDesktop ? 1 : 0) +
                        " input-primed=1");
                }
                nextSecurePoll = now + std::chrono::milliseconds(25);
            }

            if (isSecureHelper && secureDesktopEnding &&
                now >= secureDesktopEndedAt + std::chrono::milliseconds(250)) {
                LogInfo("[streamer] secure desktop ended; exiting helper session=" + args.sessionId);
                break;
            }

            try {
                if (now >= nextCaptureAt) {
                    ++captureAttempts;
                    FrameCaptureResult captured = source.nextFrameEx();
                    const bool recentInput = (now - lastInputAt) <= activeHold;
                    const bool recentMotion = (now - lastChangedFrameAt) <= std::chrono::milliseconds(ReadEnvInt("HI5_STREAM_MOTION_HOLD_MS", 300, 100, 2000));
                    const int targetFps = recentInput ? motionFps : (recentMotion ? activeFps : idleFps);
                    lastTargetFps = targetFps;
                    lastStreamMode = recentInput ? 2 : (recentMotion ? 1 : 0);
                    const auto frameInterval = std::chrono::milliseconds(1000 / std::max(1, targetFps));
                    nextCaptureAt = now + frameInterval;

                    if (captured.hasFrame && captured.changed) {
                        ++changedFrames;
                        lastChangedFrameAt = now;
                        const uint64_t tsNs = static_cast<uint64_t>(GetTickCount64()) * 1000000ull;
                        if (!shmem.WriteRawI420Frame(captured.frame, tsNs)) {
                            LogWarn("[streamer] WriteRawI420Frame failed session=" + args.sessionId +
                                " bytes=" + std::to_string(captured.frame.y.size() + captured.frame.u.size() + captured.frame.v.size()));
                        }
                        else {
                            ++writtenFrames;
                        }
                    }
                    else {
                        ++skippedFrames;
                    }
                    consecutiveResetFailures = 0;
                }
            }
            catch (const std::exception& ex) {
                LogWarn("[streamer] capture error session=" + args.sessionId + ": " + ex.what());

                if (isSecureHelper && (!lastSecureState || secureDesktopEnding)) {
                    LogInfo("[streamer] secure helper exiting after capture failure session=" + args.sessionId);
                    break;
                }

                ++resetCount;
                try {
                    DesktopFrameSource resetSource;
                    resetSource.setDisplayIndex(currentDisplay);
                    source = std::move(resetSource);
                    consecutiveResetFailures = 0;
                    if (inputPipeOk) {
                        PublishMonitorInfo(inputPipe, source);
                    }
                    LogInfo("[streamer] capture reset succeeded session=" + args.sessionId);
                }
                catch (const std::exception& rex) {
                    ++consecutiveResetFailures;
                    LogWarn("[streamer] capture reset failed session=" + args.sessionId + ": " + rex.what());
                    if (isSecureHelper && consecutiveResetFailures >= 3) {
                        LogInfo("[streamer] secure helper exiting after repeated reset failure session=" + args.sessionId);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            const auto currentTime = std::chrono::steady_clock::now();
            if (currentTime >= nextStatsAt) {
                LogInfo("[streamer] low-cpu stats session=" + args.sessionId +
                    " attempts=" + std::to_string(captureAttempts) +
                    " changed=" + std::to_string(changedFrames) +
                    " cursor_only=" + std::to_string(cursorOnlyFrames) +
                    " skipped=" + std::to_string(skippedFrames) +
                    " written=" + std::to_string(writtenFrames) +
                    " input=" + std::to_string(inputEvents) +
                    " fps=" + std::to_string(lastTargetFps) +
                    " mode=" + std::string(StreamModeName(lastStreamMode)));

                if (inputPipeOk) {
                    hi5::StreamStats stats{};
                    stats.unixMs = UnixMsNow();
                    stats.captureAttempts = captureAttempts;
                    stats.changedFrames = changedFrames;
                    stats.skippedFrames = skippedFrames;
                    stats.writtenFrames = writtenFrames;
                    stats.inputEvents = inputEvents;
                    stats.resetCount = resetCount;
                    stats.targetFps = lastTargetFps;
                    stats.streamMode = lastStreamMode;
                    stats.secureDesktopActive = lastSecureState ? 1 : 0;
                    stats.displayIndex = currentDisplay;
                    inputPipe.PublishStreamStats(stats);
                }

                captureAttempts = 0;
                changedFrames = 0;
                cursorOnlyFrames = 0;
                skippedFrames = 0;
                writtenFrames = 0;
                inputEvents = 0;
                resetCount = 0;
                nextStatsAt = currentTime + statsEvery;
            }

            const auto sleepUntil = std::min(nextCaptureAt, currentTime + std::chrono::milliseconds(25));
            if (sleepUntil > currentTime) {
                std::this_thread::sleep_until(sleepUntil);
            }
        }

        if (inputPipeOk) {
            inputPipe.Close();
        }
        shmem.Close();
        if (stopEvent) {
            CloseHandle(stopEvent);
        }
        LogInfo("[streamer] stopped session=" + args.sessionId);
        return 0;
    }

} // namespace hi5