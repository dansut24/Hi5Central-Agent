#include "native_tray.h"

#if defined(_WIN32)
#include "util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace hi5 {
namespace {
using json = nlohmann::json;
constexpr UINT kTrayMessage = WM_APP + 241;
constexpr UINT_PTR kStopTimer = 17;
constexpr UINT kActionBase = 1000;
constexpr UINT kSupportCommand = 9000;

std::string ArgValue(int argc, char** argv, const std::string& name, const std::string& fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && name == argv[i]) return argv[i + 1] ? std::string(argv[i + 1]) : fallback;
    }
    return fallback;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), needed);
    return out;
}

std::string DecodeBase64(const std::string& encoded) {
    if (encoded.empty()) return {};
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr) || bytes == 0) return {};
    std::vector<BYTE> buffer(bytes);
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64, buffer.data(), &bytes, nullptr, nullptr)) return {};
    return std::string(reinterpret_cast<const char*>(buffer.data()), bytes);
}

struct TrayAction {
    std::string id;
    std::wstring label;
    std::wstring description;
    std::wstring eventName;
    bool confirmationRequired = false;
};

struct TrayApp {
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid{};
    HANDLE stopEvent = nullptr;
    HANDLE singleton = nullptr;
    UINT taskbarCreated = 0;
    std::wstring title = L"Hi5Central";
    std::wstring supportName;
    std::wstring supportUrl;
    std::vector<TrayAction> actions;

    void AddIcon() {
        nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayMessage;
        nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        wcsncpy_s(nid.szTip, title.c_str(), _TRUNCATE);
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
    }

    void Notify(const std::wstring& text, bool error = false) {
        NOTIFYICONDATAW info = nid;
        info.uFlags = NIF_INFO;
        wcsncpy_s(info.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(info.szInfo, text.c_str(), _TRUNCATE);
        info.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &info);
    }

    void Invoke(size_t index) {
        if (index >= actions.size()) return;
        const auto& action = actions[index];
        if (action.confirmationRequired) {
            std::wstring prompt = L"Run \"" + action.label + L"\" on this device?";
            if (MessageBoxW(hwnd, prompt.c_str(), title.c_str(), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
        }
        HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, action.eventName.c_str());
        if (!eventHandle) {
            Notify(L"Hi5Central could not contact the management service.", true);
            return;
        }
        const BOOL sent = SetEvent(eventHandle);
        CloseHandle(eventHandle);
        Notify(sent ? (action.label + L" request sent to Hi5Central.") : L"The action could not be requested.", sent == FALSE);
    }

    void OpenSupport() {
        if (supportUrl.empty()) return;
        ShellExecuteW(hwnd, L"open", supportUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void ShowMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, title.c_str());
        if (!supportName.empty()) AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, supportName.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (actions.empty()) {
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"No self-service actions available");
        } else {
            for (size_t i = 0; i < actions.size(); ++i) {
                AppendMenuW(menu, MF_STRING, kActionBase + static_cast<UINT>(i), actions[i].label.c_str());
            }
        }
        if (!supportUrl.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kSupportCommand, L"Open support portal");
        }
        POINT anchor{};
        UINT alignment = TPM_RIGHTALIGN | TPM_BOTTOMALIGN;
        bool anchoredToIcon = false;
        NOTIFYICONIDENTIFIER identifier{};
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = hwnd;
        identifier.uID = nid.uID;
        RECT iconRect{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &iconRect))) {
            anchor.x = iconRect.right;
            anchor.y = iconRect.top;
            anchoredToIcon = true;
            HMONITOR monitor = MonitorFromRect(&iconRect, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (monitor && GetMonitorInfoW(monitor, &info)) {
                if (info.rcWork.top > info.rcMonitor.top) {
                    anchor.y = iconRect.bottom;
                    alignment = TPM_RIGHTALIGN | TPM_TOPALIGN;
                } else if (info.rcWork.left > info.rcMonitor.left) {
                    anchor.x = iconRect.right;
                    anchor.y = iconRect.bottom;
                    alignment = TPM_LEFTALIGN | TPM_BOTTOMALIGN;
                } else if (info.rcWork.right < info.rcMonitor.right) {
                    anchor.x = iconRect.left;
                    anchor.y = iconRect.bottom;
                    alignment = TPM_RIGHTALIGN | TPM_BOTTOMALIGN;
                }
            }
        }
        if (!anchoredToIcon) GetCursorPos(&anchor);
        SetForegroundWindow(hwnd);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | alignment, anchor.x, anchor.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        if (command >= kActionBase && command < kActionBase + actions.size()) Invoke(command - kActionBase);
        else if (command == kSupportCommand) OpenSupport();
    }
};

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<TrayApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app) app->hwnd = hwnd;
    }
    if (!app) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == app->taskbarCreated) { app->AddIcon(); return 0; }
    switch (message) {
    case kTrayMessage:
        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) app->ShowMenu();
        return 0;
    case WM_TIMER:
        if (wParam == kStopTimer && app->stopEvent && WaitForSingleObject(app->stopEvent, 0) == WAIT_OBJECT_0) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &app->nid);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
}

int RunNativeTrayMain(int argc, char** argv) {
    const std::string policyText = DecodeBase64(ArgValue(argc, argv, "--policy-b64"));
    const std::string stopEventName = ArgValue(argc, argv, "--stop-event");
    const std::string sessionId = ArgValue(argc, argv, "--session", "0");
    const auto policy = json::parse(policyText, nullptr, false);
    if (policy.is_discarded() || !policy.is_object()) {
        LogError("[tray] invalid policy payload");
        return 2;
    }

    TrayApp app;
    app.title = Wide(policy.value("title", std::string("Hi5Central")));
    if (app.title.empty()) app.title = L"Hi5Central";
    if (policy.contains("support") && policy["support"].is_object()) {
        app.supportName = Wide(policy["support"].value("displayName", std::string()));
        app.supportUrl = Wide(policy["support"].value("portalUrl", std::string()));
    }
    if (policy.contains("actions") && policy["actions"].is_array()) {
        for (const auto& item : policy["actions"]) {
            if (!item.is_object()) continue;
            TrayAction action;
            action.id = item.value("id", std::string());
            action.label = Wide(item.value("label", std::string("Hi5Central action")));
            action.description = Wide(item.value("description", std::string()));
            action.eventName = Wide(item.value("eventName", std::string()));
            action.confirmationRequired = item.value("confirmationRequired", false);
            if (!action.id.empty() && !action.eventName.empty()) app.actions.push_back(std::move(action));
        }
    }

    const std::wstring mutexName = L"Local\\Hi5CentralTray_" + Wide(sessionId);
    app.singleton = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!app.singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (app.singleton) CloseHandle(app.singleton);
        return 0;
    }
    if (!stopEventName.empty()) app.stopEvent = OpenEventA(SYNCHRONIZE, FALSE, stopEventName.c_str());
    app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    const wchar_t* className = L"Hi5CentralTrayWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, className, L"Hi5Central Tray", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, &app);
    if (!hwnd) {
        if (app.stopEvent) CloseHandle(app.stopEvent);
        ReleaseMutex(app.singleton); CloseHandle(app.singleton);
        return 3;
    }
    app.AddIcon();
    SetTimer(hwnd, kStopTimer, 500, nullptr);
    LogInfo("[tray] started session=" + sessionId + " actions=" + std::to_string(app.actions.size()));

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (app.stopEvent) CloseHandle(app.stopEvent);
    if (app.singleton) { ReleaseMutex(app.singleton); CloseHandle(app.singleton); }
    LogInfo("[tray] stopped session=" + sessionId);
    return 0;
}
} // namespace hi5

#else
namespace hi5 { int RunNativeTrayMain(int, char**) { return 1; } }
#endif
