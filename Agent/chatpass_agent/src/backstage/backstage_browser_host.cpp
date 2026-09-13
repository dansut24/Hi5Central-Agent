#include "backstage_browser_host.h"
#include "util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

HWND g_hwnd = nullptr;
std::wstring g_initialUrl = L"https://www.google.com";
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;

std::optional<std::string> GetArgValue(int argc, char** argv, const std::string& key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == key) return std::string(argv[i + 1]);
    }
    return std::nullopt;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, n - 1)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::wstring NormaliseUrl(std::wstring url) {
    while (!url.empty() && (url.back() == L'\r' || url.back() == L'\n' || url.back() == L' ' || url.back() == L'\t')) url.pop_back();
    while (!url.empty() && (url.front() == L' ' || url.front() == L'\t')) url.erase(url.begin());
    if (url.empty()) return L"https://www.google.com";
    const std::wstring lower = [&]() {
        std::wstring t = url;
        std::transform(t.begin(), t.end(), t.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return t;
    }();
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 || lower.rfind(L"file://", 0) == 0) {
        return url;
    }
    if (url.find(L'.') != std::wstring::npos && url.find(L' ') == std::wstring::npos) {
        return L"https://" + url;
    }
    return L"https://www.bing.com/search?q=" + url;
}

void ResizeWebView() {
    if (!g_controller || !g_hwnd) return;
    RECT bounds{};
    GetClientRect(g_hwnd, &bounds);
    g_controller->put_Bounds(bounds);
}

std::wstring BrowserUserDataFolder() {
    PWSTR programData = nullptr;
    std::wstring base = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &programData)) && programData) {
        base = programData;
        CoTaskMemFree(programData);
    }
    std::wstring folder = base + L"\\Hi5Central\\Agent\\WebView2\\Backstage";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    return folder;
}

void InitWebView2() {
    const std::wstring userData = BrowserUserDataFolder();
    LogInfo("[backstage-browser] creating WebView2 environment");

    // Private-desktop capture uses PrintWindow/GDI from the Backstage host.
    // WebView2 normally renders through GPU/DirectComposition surfaces, which often
    // capture as a blank white rectangle from an inactive/private desktop. Force the
    // WebView2 child process toward software/GDI-compatible rendering for this
    // Backstage experiment.
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (options) {
        options->put_AdditionalBrowserArguments(
            L"--disable-gpu "
            L"--disable-gpu-compositing "
            L"--disable-direct-composition "
            L"--disable-accelerated-2d-canvas "
            L"--disable-features=CalculateNativeWinOcclusion,DirectCompositionSwapChainPresenter");
        LogInfo("[backstage-browser] WebView2 software-rendering flags enabled for private-desktop capture");
    }

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userData.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    LogWarn("[backstage-browser] CreateCoreWebView2Environment failed hr=" + std::to_string(static_cast<long>(result)));
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                LogWarn("[backstage-browser] CreateCoreWebView2Controller failed hr=" + std::to_string(static_cast<long>(result)));
                                return S_OK;
                            }

                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            ResizeWebView();

                            ComPtr<ICoreWebView2Settings> settings;
                            if (g_webview && SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                settings->put_IsStatusBarEnabled(TRUE);
                                settings->put_AreDefaultContextMenusEnabled(TRUE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(TRUE);
                            }

                            if (g_webview) {
                                LogInfo("[backstage-browser] navigating initial URL");
                                g_webview->Navigate(g_initialUrl.c_str());
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        LogWarn("[backstage-browser] CreateCoreWebView2EnvironmentWithOptions call failed hr=" + std::to_string(static_cast<long>(hr)));
    }
}

LRESULT CALLBACK BrowserWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        InitWebView2();
        return 0;
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_SETFOCUS:
        if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_webview.Reset();
        g_controller.Reset();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

namespace hi5 {

int RunBackstageBrowserMain(int argc, char** argv) {
    LogInfo("[backstage-browser] process start");
    if (auto url = GetArgValue(argc, argv, "--url")) {
        g_initialUrl = NormaliseUrl(Utf8ToWide(*url));
    }

    HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) {
        LogWarn("[backstage-browser] OleInitialize failed hr=" + std::to_string(static_cast<long>(ole)));
    }

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"Hi5CentralBackstageBrowserHostWindow";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BrowserWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        cls,
        L"Hi5Central Backstage Browser",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        120,
        90,
        1120,
        720,
        nullptr,
        nullptr,
        hinst,
        nullptr);

    if (!hwnd) {
        LogWarn("[backstage-browser] CreateWindowExW failed err=" + std::to_string(GetLastError()));
        if (SUCCEEDED(ole)) OleUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(ole)) OleUninitialize();
    LogInfo("[backstage-browser] stopped");
    return 0;
}

} // namespace hi5
