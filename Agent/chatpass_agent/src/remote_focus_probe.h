// Windows interactive-session UI Automation focus probe.
// Publishes only editability and keyboard type into the IPC ring; no field
// values, selection, URL, passwords, or window titles leave the endpoint.
// Keep COM/UI Automation calls off the video capture loop.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <string>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

class RemoteFocusProbe {
public:
    RemoteFocusProbe() = default;
    ~RemoteFocusProbe() { Stop(); }
    RemoteFocusProbe(const RemoteFocusProbe&) = delete;
    RemoteFocusProbe& operator=(const RemoteFocusProbe&) = delete;

    void Start(hi5::InputPipeReader& pipe) {
        if (running_.exchange(true)) return;
        worker_ = std::thread([this, &pipe]() {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IUIAutomation* automation = nullptr;
            if (SUCCEEDED(init)) {
                CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IUIAutomation, reinterpret_cast<void**>(&automation));
            }
            while (running_.load()) {
                const int kind = IsDefaultDesktop() ? GetFocusKind(automation) : 0;
                pipe.PublishRemoteFocus(kind);
                for (int t = 0; t < 7 && running_.load(); ++t)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            pipe.PublishRemoteFocus(0);
            if (automation) automation->Release();
            if (SUCCEEDED(init)) CoUninitialize();
        });
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

private:
    std::atomic<bool> running_{ false };
    std::thread worker_;

    static bool IsDefaultDesktop() {
        HDESK desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
        if (!desktop) return false;
        wchar_t name[64]{};
        DWORD needed = 0;
        bool ordinary = false;
        if (GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed))
            ordinary = (_wcsicmp(name, L"Default") == 0);
        CloseDesktop(desktop);
        return ordinary;
    }

    static std::wstring Lower(std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
        return value;
    }

    static int GetFocusKind(IUIAutomation* automation) {
        if (automation) {
            IUIAutomationElement* focused = nullptr;
            if (SUCCEEDED(automation->GetFocusedElement(&focused)) && focused) {
                CONTROLTYPEID type = 0;
                BOOL isPassword = FALSE;
                BOOL enabled = TRUE;
                const bool hasType = SUCCEEDED(focused->get_CurrentControlType(&type));
                focused->get_CurrentIsPassword(&isPassword);
                focused->get_CurrentIsEnabled(&enabled);
                const bool editable = enabled && hasType &&
                    (type == UIA_EditControlTypeId || type == UIA_ComboBoxControlTypeId);
                int kind = editable ? (isPassword ? 3 : 1) : 0;
                if (kind == 1) {
                    BSTR name = nullptr;
                    BSTR automationId = nullptr;
                    focused->get_CurrentName(&name);
                    focused->get_CurrentAutomationId(&automationId);
                    std::wstring label;
                    if (name) label += name;
                    if (automationId) label += automationId;
                    label = Lower(label);
                    if (label.find(L"address") != std::wstring::npos ||
                        label.find(L"omnibox") != std::wstring::npos ||
                        label.find(L"url") != std::wstring::npos ||
                        label.find(L"location bar") != std::wstring::npos)
                        kind = 2;
                    if (name) SysFreeString(name);
                    if (automationId) SysFreeString(automationId);
                }
                focused->Release();
                if (kind != 0) return kind;
            }
        }

        // This probe runs on a worker thread, not the foreground GUI thread.
        // Query the currently active app so Win32 RichEdit and classic Notepad
        // can be recognised when they do not expose a UIA edit control.
        const HWND foreground = GetForegroundWindow();
        const DWORD threadId = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        if (threadId == 0) return 0;
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(threadId, &info) || !info.hwndFocus) return 0;
        wchar_t buffer[128]{};
        if (!GetClassNameW(info.hwndFocus, buffer, 128)) return 0;
        const std::wstring cls = Lower(buffer);
        if (cls.find(L"edit") != std::wstring::npos ||
            cls.find(L"richedit") != std::wstring::npos ||
            cls.find(L"textbox") != std::wstring::npos)
            return 1;
        return 0;
    }
};
