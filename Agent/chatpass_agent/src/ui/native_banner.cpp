#include "native_banner.h"
#include "../util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <chrono>
#include <string>
#include <algorithm>

namespace hi5 {
namespace {
    constexpr const wchar_t* kBannerClass = L"Hi5CentralNativeBannerWndV2";
    constexpr UINT kBannerUpdateText = WM_APP + 101;

    constexpr int kCompactWidth = 330;
    constexpr int kCompactHeight = 42;
    constexpr int kExpandedWidth = 660;
    constexpr int kExpandedHeight = 90;
    constexpr int kTopMargin = 14;

    std::wstring ToWide(const std::string& s) {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
        return out;
    }

    static std::string ArgValue(int argc, char** argv, const std::string& name, const std::string& fallback = {}) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (argv[i] && name == argv[i]) return argv[i + 1] ? std::string(argv[i + 1]) : fallback;
        }
        return fallback;
    }

    void PositionBanner(HWND hwnd, bool expanded) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = expanded ? kExpandedWidth : kCompactWidth;
        const int height = expanded ? kExpandedHeight : kCompactHeight;
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + kTopMargin;
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    struct BannerState {
        std::wstring technician;
        bool expanded = false;
        bool trackingMouse = false;
    };

    void PaintBanner(HWND hwnd, HDC hdc) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        auto* state = reinterpret_cast<BannerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        const bool expanded = state && state->expanded;
        const std::wstring tech = (state && !state->technician.empty()) ? state->technician : L"Technician";

        HBRUSH bg = CreateSolidBrush(RGB(20, 24, 31));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 150, 245));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 18, 18);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        SetBkMode(hdc, TRANSPARENT);

        HFONT titleFont = CreateFontW(expanded ? 21 : 17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT subFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        SetTextColor(hdc, RGB(255, 255, 255));
        SelectObject(hdc, titleFont);

        if (!expanded) {
            RECT iconRc{ 14, 8, 40, 34 };
            HBRUSH accent = CreateSolidBrush(RGB(37, 99, 235));
            FillRect(hdc, &iconRc, accent);
            DeleteObject(accent);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, L"H5", -1, &iconRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT titleRc{ 50, 6, rc.right - 14, rc.bottom - 6 };
            DrawTextW(hdc, L"Hi5Central support connected", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        else {
            RECT titleRc{ 18, 12, rc.right - 18, 38 };
            RECT subRc{ 18, 42, rc.right - 18, rc.bottom - 10 };
            DrawTextW(hdc, L"Hi5Central support connected", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(hdc, RGB(198, 207, 219));
            SelectObject(hdc, subFont);
            std::wstring sub = tech + L" is connected to this device. Chat is available if the technician sends a message.";
            DrawTextW(hdc, sub.c_str(), -1, &subRc, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        }

        DeleteObject(titleFont);
        DeleteObject(subFont);
    }

    LRESULT CALLBACK BannerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* state = reinterpret_cast<BannerState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return 0;
        }
        case kBannerUpdateText: {
            auto* state = reinterpret_cast<BannerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            auto* incoming = reinterpret_cast<std::wstring*>(lParam);
            if (state && incoming) {
                state->technician = *incoming;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            delete incoming;
            return 0;
        }
        case WM_MOUSEMOVE: {
            auto* state = reinterpret_cast<BannerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (state) {
                if (!state->trackingMouse) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                    state->trackingMouse = true;
                }
                if (!state->expanded) {
                    state->expanded = true;
                    PositionBanner(hwnd, true);
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            auto* state = reinterpret_cast<BannerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (state) {
                state->trackingMouse = false;
                state->expanded = false;
                PositionBanner(hwnd, false);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintBanner(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            auto* state = reinterpret_cast<BannerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            PositionBanner(hwnd, state && state->expanded);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}

NativeBanner::NativeBanner() = default;
NativeBanner::~NativeBanner() { Stop(); }

void NativeBanner::Start(const std::string& technicianName) {
    technicianName_ = technicianName;
    if (running_.exchange(true)) {
        SetTechnicianName(technicianName);
        return;
    }
    thread_ = std::thread([this]() { ThreadMain(); });
}

void NativeBanner::Stop() {
    if (!running_.exchange(false)) return;
    if (threadId_ != 0) PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void NativeBanner::SetTechnicianName(const std::string& technicianName) {
    technicianName_ = technicianName;
}

void NativeBanner::ThreadMain() {
    threadId_ = GetCurrentThreadId();
    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = BannerWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kBannerClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    BannerState state;
    state.technician = ToWide(technicianName_);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kBannerClass,
        L"Hi5Central Support Connected",
        WS_POPUP | WS_VISIBLE,
        0, 0, kCompactWidth, kCompactHeight,
        nullptr, nullptr, hInst, &state);

    if (!hwnd) {
        LogWarn("[banner] failed to create window");
        running_.store(false);
        return;
    }

    PositionBanner(hwnd, false);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    LogInfo("[banner] native banner shown");

    MSG msg{};
    while (running_.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running_.store(false);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    DestroyWindow(hwnd);
    LogInfo("[banner] native banner stopped");
}

int RunNativeBannerMain(int argc, char** argv) {
    const std::string technician = ArgValue(argc, argv, "--technician", "Technician");
    const std::string stopEventName = ArgValue(argc, argv, "--stop-event", "");
    int durationMs = 0;
    const std::string rawDuration = ArgValue(argc, argv, "--duration-ms", "0");
    try { durationMs = std::max(0, std::stoi(rawDuration)); } catch (...) { durationMs = 0; }

    HANDLE stopEvent = nullptr;
    if (!stopEventName.empty()) {
        stopEvent = OpenEventA(SYNCHRONIZE, FALSE, stopEventName.c_str());
        if (!stopEvent) {
            LogWarn("[banner] failed to open stop event=" + stopEventName + " err=" + std::to_string(GetLastError()));
        }
    }

    LogInfo("[banner] helper start technician=" + technician + " duration_ms=" + std::to_string(durationMs) + " stop_event=" + stopEventName);

    NativeBanner banner;
    banner.Start(technician);

    const auto started = std::chrono::steady_clock::now();
    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                banner.Stop();
                if (stopEvent) CloseHandle(stopEvent);
                LogInfo("[banner] helper quit");
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            banner.Stop();
            CloseHandle(stopEvent);
            LogInfo("[banner] helper stop event signaled");
            return 0;
        }

        if (durationMs > 0 && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(durationMs)) {
            banner.Stop();
            if (stopEvent) CloseHandle(stopEvent);
            LogInfo("[banner] helper duration elapsed");
            return 0;
        }
        Sleep(150);
    }
}

} // namespace hi5
