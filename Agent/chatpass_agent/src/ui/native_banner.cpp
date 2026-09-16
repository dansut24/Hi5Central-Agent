#include "native_banner.h"
#include "../util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace hi5 {
namespace {
constexpr const wchar_t* kBannerClass = L"Hi5CentralSupportPanelWndV3";
constexpr UINT kBannerUpdateText = WM_APP + 101;
constexpr UINT kNotifyCallback = WM_APP + 102;
constexpr UINT_PTR kUiTimer = 1;
constexpr UINT_PTR kCollapseTimer = 2;
constexpr UINT_PTR kSlideTimer = 3;
constexpr float kCollapsedWidth = 40.0f;
constexpr float kCollapsedHeight = 82.0f;
constexpr float kExpandedWidth = 344.0f;
constexpr float kExpandedHeight = 372.0f;
constexpr float kExpandedInfoHeight = 420.0f;

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string ArgValue(int argc, char** argv, const std::string& name, const std::string& fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && name == argv[i]) return argv[i + 1] ? std::string(argv[i + 1]) : fallback;
    }
    return fallback;
}

bool ArgBool(int argc, char** argv, const std::string& name, bool fallback) {
    std::string value = ArgValue(argc, argv, name, fallback ? "1" : "0");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}
struct BannerState {
    std::wstring technician;
    std::wstring sessionId;
    HANDLE chatEvent = nullptr;
    HANDLE endEvent = nullptr;
    bool expanded = false;
    bool trackingMouse = false;
    float reveal = 0.0f;
    bool showInfo = false;
    bool notifyOnStart = true;
    float scale = 1.0f;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    ID2D1Factory* d2dFactory = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* renderTarget = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;

    NOTIFYICONDATAW notifyIcon{};
    bool notifyIconAdded = false;
};

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

D2D1_COLOR_F HexColor(unsigned int rgb, float alpha = 1.0f) {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xff) / 255.0f,
        ((rgb >> 8) & 0xff) / 255.0f,
        (rgb & 0xff) / 255.0f,
        alpha);
}
float ExpandedHeight(const BannerState& state) {
    return state.showInfo ? kExpandedInfoHeight : kExpandedHeight;
}

int Px(float dip, float scale) {
    return std::max(1, static_cast<int>(std::lround(dip * scale)));
}

void ApplyRoundedRegion(HWND hwnd, int width, int height, float scale, bool expanded) {
    const int radius = Px(expanded ? 18.0f : 14.0f, scale);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (region) {
        if (!SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
    }
}

void PositionBanner(HWND hwnd, BannerState& state) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const float t = std::max(0.0f, std::min(1.0f, state.reveal));
    const float widthDip = kCollapsedWidth + ((kExpandedWidth - kCollapsedWidth) * t);
    const float heightDip = kCollapsedHeight + ((ExpandedHeight(state) - kCollapsedHeight) * t);
    const int width = Px(widthDip, state.scale);
    const int height = Px(heightDip, state.scale);
    const int workHeight = work.bottom - work.top;
    const int preferredY = work.top + static_cast<int>((workHeight - height) * 0.32);
    const int y = std::max(static_cast<int>(work.top) + Px(24.0f, state.scale), preferredY);
    const int x = work.right - width;

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    ApplyRoundedRegion(hwnd, width, height, state.scale, state.reveal >= 0.5f);
}
void DiscardDeviceResources(BannerState& state) {
    SafeRelease(state.brush);
    SafeRelease(state.renderTarget);
}

bool EnsureDeviceResources(HWND hwnd, BannerState& state) {
    if (state.renderTarget && state.brush) return true;
    if (!state.d2dFactory) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(std::max(1L, rc.right - rc.left)),
        static_cast<UINT32>(std::max(1L, rc.bottom - rc.top)));

    HRESULT hr = state.d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd, size),
        &state.renderTarget);
    if (FAILED(hr) || !state.renderTarget) return false;

    const float dpi = 96.0f * state.scale;
    state.renderTarget->SetDpi(dpi, dpi);
    hr = state.renderTarget->CreateSolidColorBrush(HexColor(0x0f172a), &state.brush);
    if (FAILED(hr)) {
        DiscardDeviceResources(state);
        return false;
    }
    return true;
}

void SetBrush(BannerState& state, unsigned int rgb, float alpha = 1.0f) {
    if (state.brush) state.brush->SetColor(HexColor(rgb, alpha));
}
void DrawTextLine(BannerState& state,
    const std::wstring& text,
    const D2D1_RECT_F& rect,
    float size,
    DWRITE_FONT_WEIGHT weight,
    unsigned int rgb,
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (!state.dwriteFactory || !state.renderTarget || !state.brush) return;

    IDWriteTextFormat* format = nullptr;
    HRESULT hr = state.dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size,
        L"en-GB",
        &format);
    if (FAILED(hr) || !format) return;

    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    SetBrush(state, rgb);
    state.renderTarget->DrawText(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        format,
        rect,
        state.brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
    format->Release();
}

std::wstring Initials(const std::wstring& name) {
    std::wstring out;
    bool take = true;
    for (wchar_t ch : name) {
        if (take && ch != L' ' && ch != L'\t') {
            out.push_back(static_cast<wchar_t>(towupper(ch)));
            if (out.size() == 2) break;
            take = false;
        }
        else if (ch == L' ' || ch == L'\t') {
            take = true;
        }
    }
    return out.empty() ? L"H5" : out;
}
std::wstring DurationText(const BannerState& state) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - state.started).count();
    const long long hours = elapsed / 3600;
    const long long minutes = (elapsed % 3600) / 60;
    const long long seconds = elapsed % 60;
    std::wostringstream ss;
    ss << L"Connected for ";
    if (hours > 0) ss << hours << L":" << std::setw(2) << std::setfill(L'0') << minutes << L":";
    else ss << minutes << L":";
    ss << std::setw(2) << std::setfill(L'0') << seconds;
    return ss.str();
}

void FillRounded(BannerState& state, const D2D1_RECT_F& rect, float radius, unsigned int rgb, float alpha = 1.0f) {
    if (!state.renderTarget || !state.brush) return;
    SetBrush(state, rgb, alpha);
    state.renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), state.brush);
}

void StrokeRounded(BannerState& state, const D2D1_RECT_F& rect, float radius, unsigned int rgb, float width = 1.0f) {
    if (!state.renderTarget || !state.brush) return;
    SetBrush(state, rgb);
    state.renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), state.brush, width);
}

void DrawButton(BannerState& state,
    const D2D1_RECT_F& rect,
    const std::wstring& label,
    bool destructive,
    const std::wstring& glyph) {
    const unsigned int fill = destructive ? 0xfff5f5 : 0xf8fafc;
    const unsigned int border = destructive ? 0xef4444 : 0xdbe3ec;
    const unsigned int text = destructive ? 0xdc2626 : 0x172033;
    FillRounded(state, rect, 10.0f, fill);
    StrokeRounded(state, rect, 10.0f, border, 1.0f);
    DrawTextLine(state, glyph, D2D1::RectF(rect.left + 14, rect.top, rect.left + 42, rect.bottom),
        16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, text, DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawTextLine(state, label, D2D1::RectF(rect.left + 48, rect.top, rect.right - 12, rect.bottom),
        14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, text);
}
void PaintCollapsed(BannerState& state) {
    auto* rt = state.renderTarget;
    if (!rt) return;

    rt->Clear(HexColor(0x0f172a));
    FillRounded(state, D2D1::RectF(0.0f, 0.0f, kCollapsedWidth, kCollapsedHeight), 13.0f, 0x111827);

    SetBrush(state, 0x22c55e);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(20.0f, 18.0f), 5.0f, 5.0f), state.brush);

    DrawTextLine(state, L"\u2039", D2D1::RectF(0.0f, 27.0f, kCollapsedWidth, 58.0f),
        24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0xffffff, DWRITE_TEXT_ALIGNMENT_CENTER);

    SetBrush(state, 0x2563eb);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(10.0f, 61.0f, 30.0f, 77.0f), 5.0f, 5.0f),
        state.brush);
    DrawTextLine(state, L"H5", D2D1::RectF(9.0f, 60.0f, 31.0f, 78.0f),
        8.0f, DWRITE_FONT_WEIGHT_BOLD, 0xffffff, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void PaintHeader(BannerState& state) {
    FillRounded(state, D2D1::RectF(16.0f, 14.0f, 48.0f, 46.0f), 9.0f, 0x2563eb);
    DrawTextLine(state, L"H5", D2D1::RectF(16.0f, 14.0f, 48.0f, 46.0f),
        11.0f, DWRITE_FONT_WEIGHT_BOLD, 0xffffff, DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawTextLine(state, L"Hi5Central Remote Support", D2D1::RectF(58.0f, 12.0f, 304.0f, 48.0f),
        17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0x111827);
    DrawTextLine(state, L"\u00d7", D2D1::RectF(306.0f, 10.0f, 336.0f, 42.0f),
        19.0f, DWRITE_FONT_WEIGHT_NORMAL, 0x64748b, DWRITE_TEXT_ALIGNMENT_CENTER);

    SetBrush(state, 0x22c55e);
    state.renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(24.0f, 61.0f), 4.5f, 4.5f), state.brush);
    DrawTextLine(state, L"Connected", D2D1::RectF(34.0f, 50.0f, 150.0f, 72.0f),
        13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0x15803d);
}
void PaintExpanded(BannerState& state) {
    auto* rt = state.renderTarget;
    if (!rt) return;

    rt->Clear(HexColor(0xf7f9fc));
    FillRounded(state, D2D1::RectF(0.0f, 0.0f, kExpandedWidth, ExpandedHeight(state)), 16.0f, 0xf8fafc);
    PaintHeader(state);

    SetBrush(state, 0xe5eaf0);
    rt->DrawLine(D2D1::Point2F(16.0f, 78.0f), D2D1::Point2F(328.0f, 78.0f), state.brush, 1.0f);

    SetBrush(state, 0xe8eef7);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(42.0f, 112.0f), 26.0f, 26.0f), state.brush);
    DrawTextLine(state, Initials(state.technician), D2D1::RectF(16.0f, 86.0f, 68.0f, 138.0f),
        15.0f, DWRITE_FONT_WEIGHT_BOLD, 0x2457a6, DWRITE_TEXT_ALIGNMENT_CENTER);

    const std::wstring tech = state.technician.empty() ? L"Technician" : state.technician;
    DrawTextLine(state, tech, D2D1::RectF(82.0f, 87.0f, 324.0f, 113.0f),
        16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0x111827);
    DrawTextLine(state, L"Support technician", D2D1::RectF(82.0f, 112.0f, 324.0f, 136.0f),
        12.5f, DWRITE_FONT_WEIGHT_NORMAL, 0x64748b);

    DrawTextLine(state, L"\u25f7", D2D1::RectF(18.0f, 146.0f, 42.0f, 176.0f),
        15.0f, DWRITE_FONT_WEIGHT_NORMAL, 0x64748b, DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawTextLine(state, DurationText(state), D2D1::RectF(48.0f, 146.0f, 324.0f, 176.0f),
        13.0f, DWRITE_FONT_WEIGHT_NORMAL, 0x475569);

    DrawButton(state, D2D1::RectF(16.0f, 186.0f, 328.0f, 230.0f), L"Chat", false, L"\u2709");
    DrawButton(state, D2D1::RectF(16.0f, 238.0f, 328.0f, 282.0f), L"Session info", false, L"i");
    DrawButton(state, D2D1::RectF(16.0f, 290.0f, 328.0f, 334.0f), L"End session", true, L"\u25a0");

    if (state.showInfo) {
        FillRounded(state, D2D1::RectF(16.0f, 342.0f, 328.0f, 388.0f), 9.0f, 0xf1f5f9);
        DrawTextLine(state, L"Encrypted connection  •  Remote control active",
            D2D1::RectF(28.0f, 344.0f, 316.0f, 366.0f), 11.5f,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, 0x334155);
        DrawTextLine(state, L"The local user can end this session at any time.",
            D2D1::RectF(28.0f, 365.0f, 316.0f, 388.0f), 10.5f,
            DWRITE_FONT_WEIGHT_NORMAL, 0x64748b);
    }

    const float footerTop = state.showInfo ? 392.0f : 340.0f;
    DrawTextLine(state, L"Your screen may be viewed and controlled",
        D2D1::RectF(16.0f, footerTop, 328.0f, footerTop + 22.0f), 10.5f,
        DWRITE_FONT_WEIGHT_NORMAL, 0x64748b, DWRITE_TEXT_ALIGNMENT_CENTER);
}
void PaintBanner(HWND hwnd, BannerState& state) {
    if (!EnsureDeviceResources(hwnd, state)) return;

    state.renderTarget->BeginDraw();
    state.renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    if (state.reveal >= 0.22f) PaintExpanded(state);
    else PaintCollapsed(state);

    const HRESULT hr = state.renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources(state);
    }
}

void SetExpanded(HWND hwnd, BannerState& state, bool expanded, bool /*pin*/) {
    KillTimer(hwnd, kCollapseTimer);
    state.expanded = expanded;
    if (!expanded) state.showInfo = false;
    SetTimer(hwnd, kSlideTimer, 15, nullptr);
}

bool PointIn(float x, float y, float left, float top, float right, float bottom) {
    return x >= left && x <= right && y >= top && y <= bottom;
}

void SignalAction(HANDLE eventHandle) {
    if (eventHandle) SetEvent(eventHandle);
}

void RemoveNotificationIcon(BannerState& state) {
    if (!state.notifyIconAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &state.notifyIcon);
    state.notifyIconAdded = false;
}
void ShowConnectionNotification(HWND hwnd, BannerState& state) {
    if (!state.notifyOnStart) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kNotifyCallback;
    nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32516)); // IDI_INFORMATION
    wcscpy_s(nid.szTip, L"Hi5Central Remote Support");

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        LogWarn("[banner] Windows notification icon registration failed");
        return;
    }

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, L"Hi5Central Remote Support");
    std::wstring message = (state.technician.empty() ? L"A support technician" : state.technician) +
        L" has connected to your device. Your screen may now be viewed and controlled.";
    wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    nid.uTimeout = 8000;
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    state.notifyIcon = nid;
    state.notifyIconAdded = true;
    LogInfo("[banner] Windows connection notification shown");
}

void ConfigureDwm(HWND hwnd) {
    // Numeric values keep compatibility with older Windows SDK headers while
    // enabling Windows 11 rounded corners when the attribute exists.
    const DWORD cornerAttribute = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
    const int roundPreference = 2;    // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, cornerAttribute, &roundPreference, sizeof(roundPreference));

    BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    MARGINS margins{ 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
}
LRESULT CALLBACK BannerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<BannerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<BannerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        ConfigureDwm(hwnd);
        SetTimer(hwnd, kUiTimer, 1000, nullptr);
        return 0;
    }
    case kBannerUpdateText: {
        auto* incoming = reinterpret_cast<std::wstring*>(lParam);
        if (state && incoming) {
            state->technician = *incoming;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete incoming;
        return 0;
    }
    case WM_TIMER:
        if (!state) return 0;
        if (wParam == kUiTimer && state->expanded) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kCollapseTimer) {
            KillTimer(hwnd, kCollapseTimer);
            SetExpanded(hwnd, *state, false, false);
        }
        else if (wParam == kSlideTimer) {
            const float target = state->expanded ? 1.0f : 0.0f;
            const float step = 0.14f;
            if (state->reveal < target) state->reveal = std::min(target, state->reveal + step);
            else if (state->reveal > target) state->reveal = std::max(target, state->reveal - step);
            DiscardDeviceResources(*state);
            PositionBanner(hwnd, *state);
            InvalidateRect(hwnd, nullptr, FALSE);
            if (std::fabs(state->reveal - target) < 0.001f) KillTimer(hwnd, kSlideTimer);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!state) return 0;
        KillTimer(hwnd, kCollapseTimer);
        if (!state->trackingMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            state->trackingMouse = true;
        }
        if (!state->expanded) SetExpanded(hwnd, *state, true, false);
        return 0;
    case WM_MOUSELEAVE:
        if (state) {
            state->trackingMouse = false;
            SetTimer(hwnd, kCollapseTimer, 140, nullptr);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!state) return 0;
        if (!state->expanded) {
            SetExpanded(hwnd, *state, true, false);
            return 0;
        }
        else {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / state->scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / state->scale;
            if (PointIn(x, y, 306.0f, 8.0f, 340.0f, 46.0f)) {
                SetExpanded(hwnd, *state, false, false);
            }
            else if (PointIn(x, y, 16.0f, 186.0f, 328.0f, 230.0f)) {
                SignalAction(state->chatEvent);
                LogInfo("[banner] local user requested chat");
            }
            else if (PointIn(x, y, 16.0f, 238.0f, 328.0f, 282.0f)) {
                state->showInfo = !state->showInfo;
                DiscardDeviceResources(*state);
                PositionBanner(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (PointIn(x, y, 16.0f, 290.0f, 328.0f, 334.0f)) {
                const int answer = MessageBoxW(hwnd,
                    L"End this Hi5Central remote support session?\n\nThe technician will immediately lose access.",
                    L"End remote support session",
                    MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
                if (answer == IDYES) {
                    SignalAction(state->endEvent);
                    LogInfo("[banner] local user requested session end");
                }
            }
        }
        return 0;
    case kNotifyCallback:
        if (state && (static_cast<UINT>(lParam) == NIN_BALLOONUSERCLICK || LOWORD(lParam) == NIN_BALLOONUSERCLICK)) {
            SetExpanded(hwnd, *state, true, false);
            SetTimer(hwnd, kCollapseTimer, 1200, nullptr);
        }
        return 0;
    case WM_SIZE:
        if (state) DiscardDeviceResources(*state);
        return 0;
    case WM_DPICHANGED:
        if (state) {
            const UINT dpi = HIWORD(wParam);
            state->scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
            DiscardDeviceResources(*state);
            PositionBanner(hwnd, *state);
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (state) {
            const UINT dpi = GetDpiForWindow(hwnd);
            state->scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
            DiscardDeviceResources(*state);
            PositionBanner(hwnd, *state);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        if (state) PaintBanner(hwnd, *state);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (state) RemoveNotificationIcon(*state);
        KillTimer(hwnd, kUiTimer);
        KillTimer(hwnd, kCollapseTimer);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace
NativeBanner::NativeBanner() = default;
NativeBanner::~NativeBanner() { Stop(); }

void NativeBanner::Start(const std::string& technicianName,
    const std::string& sessionId,
    const std::string& chatEventName,
    const std::string& endEventName,
    bool notifyOnStart) {
    technicianName_ = technicianName;
    sessionId_ = sessionId;
    chatEventName_ = chatEventName;
    endEventName_ = endEventName;
    notifyOnStart_ = notifyOnStart;

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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    BannerState state;
    state.technician = ToWide(technicianName_);
    state.sessionId = ToWide(sessionId_);
    state.notifyOnStart = notifyOnStart_;
    state.started = std::chrono::steady_clock::now();

    if (!chatEventName_.empty()) {
        state.chatEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, chatEventName_.c_str());
        if (!state.chatEvent) LogWarn("[banner] failed to open chat action event err=" + std::to_string(GetLastError()));
    }
    if (!endEventName_.empty()) {
        state.endEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, endEventName_.c_str());
        if (!state.endEvent) LogWarn("[banner] failed to open end-session action event err=" + std::to_string(GetLastError()));
    }

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &state.d2dFactory);
    if (FAILED(hr)) LogWarn("[banner] Direct2D factory creation failed hr=" + std::to_string(static_cast<long>(hr)));
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&state.dwriteFactory));
    if (FAILED(hr)) LogWarn("[banner] DirectWrite factory creation failed hr=" + std::to_string(static_cast<long>(hr)));

    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = BannerWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kBannerClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kBannerClass,
        L"Hi5Central Remote Support",
        WS_POPUP | WS_VISIBLE,
        0, 0, Px(kCollapsedWidth, state.scale), Px(kCollapsedHeight, state.scale),
        nullptr, nullptr, hInst, &state);

    if (!hwnd) {
        LogWarn("[banner] failed to create support panel window err=" + std::to_string(GetLastError()));
        SafeRelease(state.dwriteFactory);
        SafeRelease(state.d2dFactory);
        if (state.chatEvent) CloseHandle(state.chatEvent);
        if (state.endEvent) CloseHandle(state.endEvent);
        running_.store(false);
        CoUninitialize();
        return;
    }

    const UINT dpi = GetDpiForWindow(hwnd);
    state.scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
    state.expanded = notifyOnStart_;
    PositionBanner(hwnd, state);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    if (state.expanded) SetTimer(hwnd, kCollapseTimer, 5000, nullptr);
    ShowConnectionNotification(hwnd, state);

    LogInfo("[banner] native edge support panel shown technician=" + technicianName_ +
        " notify=" + std::string(notifyOnStart_ ? "true" : "false"));

    MSG msg{};
    while (running_.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    RemoveNotificationIcon(state);
    DiscardDeviceResources(state);
    SafeRelease(state.dwriteFactory);
    SafeRelease(state.d2dFactory);
    if (state.chatEvent) CloseHandle(state.chatEvent);
    if (state.endEvent) CloseHandle(state.endEvent);
    running_.store(false);
    threadId_ = 0;
    CoUninitialize();
    LogInfo("[banner] native edge support panel stopped");
}
int RunNativeBannerMain(int argc, char** argv) {
    const std::string technician = ArgValue(argc, argv, "--technician", "Technician");
    const std::string sessionId = ArgValue(argc, argv, "--session", "");
    const std::string stopEventName = ArgValue(argc, argv, "--stop-event", "");
    const std::string chatEventName = ArgValue(argc, argv, "--chat-event", "");
    const std::string endEventName = ArgValue(argc, argv, "--end-event", "");
    const bool notifyOnStart = ArgBool(argc, argv, "--notify", true);

    HANDLE stopEvent = nullptr;
    if (!stopEventName.empty()) {
        stopEvent = OpenEventA(SYNCHRONIZE, FALSE, stopEventName.c_str());
        if (!stopEvent) {
            LogWarn("[banner] failed to open stop event=" + stopEventName + " err=" + std::to_string(GetLastError()));
        }
    }

    LogInfo("[banner] helper start technician=" + technician +
        " session=" + sessionId +
        " notify=" + std::string(notifyOnStart ? "true" : "false"));

    NativeBanner banner;
    banner.Start(technician, sessionId, chatEventName, endEventName, notifyOnStart);

    while (true) {
        if (stopEvent && WaitForSingleObject(stopEvent, 100) == WAIT_OBJECT_0) break;
        if (!stopEvent) Sleep(100);
    }

    banner.Stop();
    if (stopEvent) CloseHandle(stopEvent);
    LogInfo("[banner] helper stop event signaled");
    return 0;
}

} // namespace hi5
