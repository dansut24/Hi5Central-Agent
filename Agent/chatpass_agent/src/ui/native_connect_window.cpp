#include "native_connect_window.h"
#include "../util/log.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace hi5 {
namespace {

constexpr wchar_t kConnectWindowClass[] = L"Hi5CentralConnectCustomerWindow";
constexpr int kChatInputId = 2108;

constexpr COLORREF kPage = RGB(247, 250, 253);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(224, 231, 240);
constexpr COLORREF kText = RGB(15, 35, 69);
constexpr COLORREF kMuted = RGB(100, 116, 139);
constexpr COLORREF kBlue = RGB(16, 112, 255);
constexpr COLORREF kBlueDark = RGB(9, 82, 196);
constexpr COLORREF kBlueSoft = RGB(238, 246, 255);
constexpr COLORREF kGreen = RGB(22, 163, 74);
constexpr COLORREF kGreenSoft = RGB(220, 252, 231);
constexpr COLORREF kAmber = RGB(180, 83, 9);
constexpr COLORREF kAmberSoft = RGB(255, 247, 214);
constexpr COLORREF kDanger = RGB(220, 38, 38);
constexpr COLORREF kDangerSoft = RGB(255, 241, 242);
constexpr COLORREF kDangerBorder = RGB(251, 113, 133);
constexpr COLORREF kTechBubble = RGB(240, 245, 253);
constexpr COLORREF kUserBubble = RGB(222, 237, 255);

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
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

std::wstring CurrentTimeText() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02u:%02u", time.wHour, time.wMinute);
    return buffer;
}

std::wstring Initials(const std::wstring& name) {
    wchar_t first = 0;
    wchar_t last = 0;
    bool atWordStart = true;
    for (const wchar_t ch : name) {
        if (iswspace(ch)) {
            atWordStart = true;
            continue;
        }
        if (atWordStart) {
            if (!first) first = static_cast<wchar_t>(towupper(ch));
            last = static_cast<wchar_t>(towupper(ch));
            atWordStart = false;
        }
    }
    if (!first) return L"?";
    std::wstring result(1, first);
    if (last && last != first) result.push_back(last);
    return result;
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void RoundBox(HDC dc, const RECT& rect, int radius, COLORREF fill,
    COLORREF border = CLR_INVALID, int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = border == CLR_INVALID
        ? static_cast<HPEN>(GetStockObject(NULL_PEN))
        : CreatePen(PS_SOLID, borderWidth, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
        radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (border != CLR_INVALID) DeleteObject(pen);
    DeleteObject(brush);
}

void Circle(HDC dc, int left, int top, int size, COLORREF fill,
    COLORREF border = CLR_INVALID) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = border == CLR_INVALID
        ? static_cast<HPEN>(GetStockObject(NULL_PEN))
        : CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, left, top, left + size, top + size);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (border != CLR_INVALID) DeleteObject(pen);
    DeleteObject(brush);
}

void DrawTextStyled(HDC dc, const std::wstring& text, RECT rect,
    HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
        flags | DT_NOPREFIX);
    if (oldFont) SelectObject(dc, oldFont);
}

int MeasureWrappedText(HDC dc, const std::wstring& text, HFONT font,
    int width) {
    RECT measure{ 0, 0, std::max(1, width), 4096 };
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measure,
        DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (oldFont) SelectObject(dc, oldFont);
    return std::max(1, static_cast<int>(measure.bottom - measure.top));
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2,
    COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(pen);
}

RECT CenteredSquareRect(int centerX, int centerY, int requestedSize) {
    const int size = std::max(2, requestedSize);
    const int left = centerX - size / 2;
    const int top = centerY - size / 2;
    return RECT{ left, top, left + size, top + size };
}

void DrawFilledPolygon(HDC dc, const POINT* points, int count,
    COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, points, count);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

HICON CreateBrandIcon(int size) {
    HDC screen = GetDC(nullptr);
    HDC colorDc = CreateCompatibleDC(screen);
    HDC maskDc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HGDIOBJ oldColor = SelectObject(colorDc, color);
    HGDIOBJ oldMask = SelectObject(maskDc, mask);

    RECT rect{ 0, 0, size, size };
    FillSolid(colorDc, rect, kBlue);
    PatBlt(maskDc, 0, 0, size, size, BLACKNESS);

    HFONT font = CreateFontW(
        -std::max(8, size * 5 / 9), 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    DrawTextStyled(colorDc, L"H5", rect, font, RGB(255, 255, 255),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(font);

    SelectObject(colorDc, oldColor);
    SelectObject(maskDc, oldMask);
    DeleteDC(colorDc);
    DeleteDC(maskDc);
    ReleaseDC(nullptr, screen);

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void DrawPersonIcon(HDC dc, int x, int y, int size, COLORREF color) {
    const int head = std::max(4, size / 3);
    Circle(dc, x + (size - head) / 2, y, head, color);
    RECT shoulders{
        x + size / 5,
        y + size / 2,
        x + size - size / 5,
        y + size
    };
    RoundBox(dc, shoulders, std::max(2, size / 6), color);
}

void DrawBuildingIcon(HDC dc, int x, int y, int size, COLORREF color) {
    // Prefer filled geometry at this size. Thin outlined rectangles become
    // visibly soft once the remote desktop is scaled in a mobile viewer.
    const int bodyLeft = x + size / 5;
    const int bodyTop = y + size / 8;
    const int bodyRight = x + size - size / 5;
    const int bodyBottom = y + size;
    RECT body{ bodyLeft, bodyTop, bodyRight, bodyBottom };
    RoundBox(dc, body, std::max(1, size / 12), color);

    const int window = std::max(2, size / 8);
    const int gapX = std::max(3, size / 5);
    const int startX = bodyLeft + std::max(2, size / 8);
    const int startY = bodyTop + std::max(3, size / 5);
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            RECT pane{
                startX + col * gapX,
                startY + row * gapX,
                startX + col * gapX + window,
                startY + row * gapX + window
            };
            FillSolid(dc, pane, kCard);
        }
    }
}

void DrawClockIcon(HDC dc, int x, int y, int size, COLORREF color) {
    // Build the ring from two filled circles so the edge remains crisp at
    // 100%, 125% and 150% DPI instead of relying on a tiny stroked ellipse.
    Circle(dc, x, y, size, color);
    const int ring = std::max(2, size / 9);
    const int innerSize = std::max(4, size - ring * 2);
    Circle(dc, x + ring, y + ring, innerSize, kCard);

    const int cx = x + size / 2;
    const int cy = y + size / 2;
    const int stroke = std::max(2, size / 11);
    DrawLine(dc, cx, cy, cx, y + size / 4, color, stroke);
    DrawLine(dc, cx, cy, x + size * 3 / 4, y + size * 2 / 3,
        color, stroke);
    Circle(dc, cx - stroke, cy - stroke, stroke * 2, color);
}

void DrawShieldIcon(HDC dc, int x, int y, int size, COLORREF color) {
    POINT points[5] = {
        { x + size / 2, y },
        { x + size, y + size / 5 },
        { x + size * 4 / 5, y + size * 3 / 4 },
        { x + size / 2, y + size },
        { x + size / 5, y + size * 3 / 4 },
    };
    DrawFilledPolygon(dc, points, 5, color);

    const int stroke = std::max(2, size / 10);
    DrawLine(dc,
        x + size / 3, y + size / 2,
        x + size * 9 / 20, y + size * 13 / 20,
        RGB(255, 255, 255), stroke);
    DrawLine(dc,
        x + size * 9 / 20, y + size * 13 / 20,
        x + size * 7 / 10, y + size * 7 / 20,
        RGB(255, 255, 255), stroke);
}

void DrawInfoIcon(HDC dc, int x, int y, int size, COLORREF color) {
    Circle(dc, x, y, size, color);
    const int cx = x + size / 2;
    const int dot = std::max(2, size / 7);
    Circle(dc, cx - dot / 2, y + size / 4, dot, RGB(255, 255, 255));
    RECT stem{
        cx - std::max(1, dot / 2),
        y + size * 9 / 20,
        cx + std::max(1, dot / 2),
        y + size * 3 / 4
    };
    FillSolid(dc, stem, RGB(255, 255, 255));
}

void DrawAttachmentIcon(HDC dc, int x, int y, int width, int height,
    COLORREF color, COLORREF background) {
    // Two nested filled capsules produce a much cleaner small paperclip than
    // thin GDI outline arcs after the remote desktop is scaled.
    RECT outer{ x, y, x + width, y + height };
    RoundBox(dc, outer, std::max(2, width / 2), color);
    const int inset = std::max(2, width / 5);
    RECT outerHole{
        outer.left + inset,
        outer.top + inset,
        outer.right - inset,
        outer.bottom - inset
    };
    RoundBox(dc, outerHole, std::max(1, width / 3), background);

    const int innerWidth = std::max(5, width / 2);
    const int innerHeight = std::max(8, height * 2 / 3);
    RECT inner{
        x + width / 3,
        y + height / 7,
        x + width / 3 + innerWidth,
        y + height / 7 + innerHeight
    };
    RoundBox(dc, inner, std::max(2, innerWidth / 2), color);
    const int innerInset = std::max(2, innerWidth / 3);
    RECT innerHole{
        inner.left + innerInset,
        inner.top + innerInset,
        inner.right - innerInset,
        inner.bottom - innerInset
    };
    if (innerHole.right > innerHole.left &&
        innerHole.bottom > innerHole.top) {
        RoundBox(dc, innerHole, std::max(1, innerWidth / 4), background);
    }
}

void DrawPaperPlaneIcon(HDC dc, int x, int y, int width, int height,
    COLORREF color) {
    POINT plane[4] = {
        { x, y + height / 2 },
        { x + width, y },
        { x + width * 3 / 5, y + height },
        { x + width * 2 / 5, y + height * 3 / 5 },
    };
    DrawFilledPolygon(dc, plane, 4, color);
}

} // namespace

NativeConnectWindow::NativeConnectWindow() = default;
NativeConnectWindow::~NativeConnectWindow() { Stop(); }

bool NativeConnectWindow::Start(const std::string& sessionId,
    const std::string& technicianName,
    const std::string& organisationName,
    SendCallback onSend,
    EndCallback onEnd) {
    Stop();

    sessionId_ = sessionId;
    technicianName_ = technicianName.empty()
        ? "Hi5Central technician" : technicianName;
    organisationName_ = organisationName.empty()
        ? "Hi5Central" : organisationName;
    statusText_ = "Waiting for technician";
    onSend_ = std::move(onSend);
    onEnd_ = std::move(onEnd);
    remoteControlActive_ = false;
    remoteControlStarted_ = false;
    ending_ = false;
    hoverSend_ = false;
    hoverEnd_ = false;
    trackingMouse_ = false;
    autoScrollChat_ = true;
    chatScrollOffset_ = 0;
    chatContentHeight_ = 0;
    messages_.clear();

    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent_) return false;

    running_.store(true);
    uiThread_ = std::thread([this]() { UiThreadMain(); });

    const DWORD wait = WaitForSingleObject(readyEvent_, 5000);
    return wait == WAIT_OBJECT_0 && hwnd_.load() != nullptr;
}

void NativeConnectWindow::Stop() {
    const bool wasRunning = running_.exchange(false);
    if (wasRunning) {
        PostAction(UiAction{ UiActionType::Stop });
    }
    if (uiThread_.joinable()) uiThread_.join();

    if (readyEvent_) {
        CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
    }

    onSend_ = nullptr;
    onEnd_ = nullptr;
    sessionId_.clear();

    std::lock_guard<std::mutex> lock(queueMu_);
    queue_.clear();
}

void NativeConnectWindow::SetIdentity(const std::string& technicianName,
    const std::string& organisationName) {
    UiAction action;
    action.type = UiActionType::Identity;
    action.first = technicianName;
    action.second = organisationName;
    PostAction(std::move(action));
}

void NativeConnectWindow::SetConnectionState(
    const std::string& statusText, bool remoteControlActive) {
    UiAction action;
    action.type = UiActionType::ConnectionState;
    action.first = statusText;
    action.flag = remoteControlActive;
    PostAction(std::move(action));
}

void NativeConnectWindow::AppendMessage(const ConnectChatMessage& message) {
    UiAction action;
    action.type = UiActionType::ChatMessage;
    action.chat = message;
    PostAction(std::move(action));
}

void NativeConnectWindow::Restore() {
    PostAction(UiAction{ UiActionType::Restore });
}

void NativeConnectWindow::PostAction(UiAction action) {
    {
        std::lock_guard<std::mutex> lock(queueMu_);
        queue_.push_back(std::move(action));
    }
    if (HWND hwnd = hwnd_.load()) {
        PostMessageW(hwnd, WM_HI5_CONNECT_QUEUE, 0, 0);
    }
}

bool NativeConnectWindow::PopAction(UiAction& action) {
    std::lock_guard<std::mutex> lock(queueMu_);
    if (queue_.empty()) return false;
    action = std::move(queue_.front());
    queue_.erase(queue_.begin());
    return true;
}

int NativeConnectWindow::S(int logical) const {
    return static_cast<int>(std::lround(
        static_cast<double>(logical) * static_cast<double>(dpiScale_)));
}

void NativeConnectWindow::UiThreadMain() {
    uiThreadId_ = GetCurrentThreadId();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const bool created = CreateUi();
    if (readyEvent_) SetEvent(readyEvent_);
    if (!created) {
        running_.store(false);
        return;
    }

    MSG msg{};
    while (running_.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyUi();
    uiThreadId_ = 0;
    running_.store(false);
}

void NativeConnectWindow::CreateFonts() {
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (subtitleFont_) { DeleteObject(subtitleFont_); subtitleFont_ = nullptr; }
    if (headingFont_) { DeleteObject(headingFont_); headingFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
    if (tinyFont_) { DeleteObject(tinyFont_); tinyFont_ = nullptr; }

    const UINT dpi = static_cast<UINT>(std::max(96.0f, 96.0f * dpiScale_));
    auto makeFont = [dpi](int points, int weight) {
        return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72),
            0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI");
    };

    titleFont_ = makeFont(18, FW_BOLD);
    subtitleFont_ = makeFont(9, FW_NORMAL);
    headingFont_ = makeFont(11, FW_SEMIBOLD);
    bodyFont_ = makeFont(10, FW_NORMAL);
    smallFont_ = makeFont(9, FW_NORMAL);
    tinyFont_ = makeFont(8, FW_NORMAL);

    if (chatInput_ && bodyFont_) {
        SendMessageW(chatInput_, WM_SETFONT,
            reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    }
}

bool NativeConnectWindow::CreateUi() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &NativeConnectWindow::StaticWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kConnectWindowClass;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogWarn("[connect-ui] failed to register attended support window class");
        return false;
    }

    const UINT initialDpi = GetDpiForSystem();
    const float initialScale =
        std::max(1.0f, static_cast<float>(initialDpi) / 96.0f);

    RECT windowRect{
        0, 0,
        static_cast<LONG>(std::lround(620.0f * initialScale)),
        static_cast<LONG>(std::lround(680.0f * initialScale))
    };
    const DWORD style =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;
    AdjustWindowRectExForDpi(
        &windowRect, style, FALSE, exStyle, initialDpi);

    HWND hwnd = CreateWindowExW(
        exStyle,
        kConnectWindowClass,
        L"Hi5Central Connect",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr, nullptr, instance, this);
    if (!hwnd) {
        LogWarn("[connect-ui] failed to create attended support window");
        return false;
    }

    hwnd_.store(hwnd);
    dpiScale_ = std::max(
        1.0f, static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);

    appIcon_ = CreateBrandIcon(32);
    // Let Windows downscale a clean 32px source for the titlebar rather than
    // rasterising tiny H5 glyphs directly at 16px.
    appIconSmall_ = CreateBrandIcon(32);
    if (appIcon_) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,
            reinterpret_cast<LPARAM>(appIcon_));
    }
    if (appIconSmall_) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
            reinterpret_cast<LPARAM>(appIconSmall_));
    }

    inputBrush_ = CreateSolidBrush(kCard);
    chatInput_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 100, 32,
        hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatInputId)),
        instance,
        nullptr);
    if (!chatInput_) {
        LogWarn("[connect-ui] failed to create chat composer edit control");
        DestroyWindow(hwnd);
        hwnd_.store(nullptr);
        return false;
    }

    inputOldProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(chatInput_, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&NativeConnectWindow::StaticInputProc)));

    CreateFonts();
    SendMessageW(chatInput_, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(2), S(2)));
    SendMessageW(chatInput_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"Chat becomes available when the technician connects"));
    EnableWindow(chatInput_, FALSE);

    const int rounded = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &rounded, sizeof(rounded));

    LayoutChildren();
    CenterWindow();

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetTimer(hwnd, TIMER_DURATION, 1000, nullptr);
    SetForegroundWindow(hwnd);

    LogInfo("[connect-ui] polished attended support window shown");
    return true;
}

void NativeConnectWindow::DestroyUi() {
    if (HWND hwnd = hwnd_.exchange(nullptr)) {
        if (IsWindow(hwnd)) {
            KillTimer(hwnd, TIMER_DURATION);
            DestroyWindow(hwnd);
        }
    }

    chatInput_ = nullptr;
    inputOldProc_ = nullptr;

    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (subtitleFont_) { DeleteObject(subtitleFont_); subtitleFont_ = nullptr; }
    if (headingFont_) { DeleteObject(headingFont_); headingFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
    if (tinyFont_) { DeleteObject(tinyFont_); tinyFont_ = nullptr; }
    if (inputBrush_) { DeleteObject(inputBrush_); inputBrush_ = nullptr; }
    if (appIcon_) { DestroyIcon(appIcon_); appIcon_ = nullptr; }
    if (appIconSmall_) { DestroyIcon(appIconSmall_); appIconSmall_ = nullptr; }
}

void NativeConnectWindow::LayoutChildren() {
    HWND hwnd = hwnd_.load();
    if (!hwnd) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = S(22);

    chatRect_ = {
        margin,
        S(278),
        width - margin,
        std::max(S(470), height - S(146))
    };

    const int composerTop = chatRect_.bottom + S(12);
    composerRect_ = {
        margin,
        composerTop,
        width - margin - S(120),
        composerTop + S(50)
    };
    sendRect_ = {
        composerRect_.right + S(10),
        composerTop,
        width - margin,
        composerTop + S(50)
    };
    endRect_ = {
        width - margin - S(154),
        height - S(60),
        width - margin,
        height - S(18)
    };

    if (chatInput_) {
        const int editLeft = composerRect_.left + S(42);
        const int editTop = composerRect_.top + S(9);
        const int editRight = composerRect_.right - S(12);
        const int editBottom = composerRect_.bottom - S(9);
        MoveWindow(chatInput_,
            editLeft,
            editTop,
            std::max(S(80), editRight - editLeft),
            std::max(S(24), editBottom - editTop),
            TRUE);

        HRGN region = CreateRoundRectRgn(
            0, 0,
            std::max(S(80), editRight - editLeft),
            std::max(S(24), editBottom - editTop),
            S(9), S(9));
        if (!SetWindowRgn(chatInput_, region, TRUE)) {
            DeleteObject(region);
        }
    }

    chatViewportHeight_ = std::max(1,
        static_cast<int>(chatRect_.bottom - chatRect_.top) - S(28));
}

void NativeConnectWindow::CenterWindow() {
    HWND hwnd = hwnd_.load();
    if (!hwnd) return;

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor =
        MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    const int x = info.rcWork.left +
        std::max(0, (workWidth - width) / 2);
    const int y = info.rcWork.top +
        std::max(0, (workHeight - height) / 2);

    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring NativeConnectWindow::SessionTimeText() const {
    if (!remoteControlStarted_) return L"--:--";

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() -
            remoteControlStartedAt_).count();
    const long long hours = elapsed / 3600;
    const long long minutes = (elapsed % 3600) / 60;
    const long long seconds = elapsed % 60;

    std::wostringstream text;
    if (hours > 0) {
        text << hours << L":"
             << std::setw(2) << std::setfill(L'0') << minutes << L":";
    } else {
        text << minutes << L":";
    }
    text << std::setw(2) << std::setfill(L'0') << seconds;
    return text.str();
}

std::wstring NativeConnectWindow::SessionStateText() const {
    if (remoteControlActive_) return L"Remote control active";
    if (remoteControlStarted_) return L"Remote control paused";
    return L"Not started";
}

void NativeConnectWindow::Paint(HDC hdc, const RECT& client) {
    FillSolid(hdc, client, kPage);
    PaintHeader(hdc, client);
    PaintSessionCard(hdc, client);
    PaintTrustCard(hdc, client);
    PaintChat(hdc, client);
    PaintComposer(hdc, client);
    PaintFooter(hdc, client);
}

void NativeConnectWindow::PaintHeader(HDC hdc, const RECT& client) {
    const int margin = S(22);
    const int logoSize = S(54);
    RECT logo{
        margin, S(20),
        margin + logoSize, S(20) + logoSize
    };
    RoundBox(hdc, logo, S(12), kBlue);

    RECT logoText = logo;
    DrawTextStyled(hdc, L"H5", logoText, headingFont_,
        RGB(255, 255, 255),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT title{
        logo.right + S(16), S(20),
        client.right - S(145), S(50)
    };
    DrawTextStyled(hdc, L"Hi5Central Remote Support",
        title, titleFont_, kText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT subtitle{
        title.left, S(51),
        client.right - S(145), S(73)
    };
    DrawTextStyled(hdc, L"Secure  •  Simple  •  Expert Help",
        subtitle, subtitleFont_, RGB(91, 114, 153),
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const bool connected = remoteControlActive_;
    const COLORREF pillFill =
        connected ? kGreenSoft : kAmberSoft;
    const COLORREF pillText =
        connected ? RGB(21, 128, 61) : kAmber;
    const std::wstring status = Utf8ToWide(
        statusText_.empty()
            ? (connected ? "Connected" : "Waiting")
            : statusText_);
    const int pillWidth = connected ? S(112) : S(142);
    RECT pill{
        client.right - margin - pillWidth,
        S(27),
        client.right - margin,
        S(61)
    };
    RoundBox(hdc, pill, S(17), pillFill);

    Circle(hdc,
        pill.left + S(13),
        pill.top + S(13),
        S(8),
        pillText);
    RECT pillLabel{
        pill.left + S(29), pill.top,
        pill.right - S(10), pill.bottom
    };
    DrawTextStyled(hdc, status, pillLabel,
        smallFont_, pillText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void NativeConnectWindow::PaintSessionCard(
    HDC hdc, const RECT& client) {
    const int margin = S(22);
    RECT card{
        margin, S(94),
        client.right - margin, S(190)
    };
    RoundBox(hdc, card, S(14), kCard, kBorder);

    const int cardWidth = card.right - card.left;
    const int columnWidth = cardWidth / 3;
    DrawLine(hdc, card.left + columnWidth, card.top + S(18),
        card.left + columnWidth, card.bottom - S(18),
        RGB(231, 237, 245));
    DrawLine(hdc, card.left + columnWidth * 2, card.top + S(18),
        card.left + columnWidth * 2, card.bottom - S(18),
        RGB(231, 237, 245));

    const int iconSize = S(25);
    const int iconY = card.top + S(26);
    const COLORREF iconColor = RGB(111, 137, 176);

    DrawPersonIcon(hdc, card.left + S(20), iconY,
        iconSize, iconColor);
    DrawBuildingIcon(hdc,
        card.left + columnWidth + S(20), iconY,
        iconSize, iconColor);
    DrawClockIcon(hdc,
        card.left + columnWidth * 2 + S(20), iconY,
        iconSize, iconColor);

    auto drawColumn = [&](int index,
        const std::wstring& label,
        const std::wstring& value,
        const std::wstring& subvalue) {
        const int left =
            card.left + index * columnWidth + S(56);
        const int right =
            card.left + (index + 1) * columnWidth - S(10);
        RECT labelRect{
            left, card.top + S(19),
            right, card.top + S(40)
        };
        RECT valueRect{
            left, card.top + S(40),
            right, card.top + S(65)
        };
        RECT subRect{
            left, card.top + S(65),
            right, card.top + S(86)
        };
        DrawTextStyled(hdc, label, labelRect,
            tinyFont_, kMuted,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextStyled(hdc, value, valueRect,
            headingFont_, kText,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!subvalue.empty()) {
            DrawTextStyled(hdc, subvalue, subRect,
                tinyFont_, kMuted,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    };

    drawColumn(0, L"Technician",
        Utf8ToWide(technicianName_), L"");
    drawColumn(1, L"Organisation",
        Utf8ToWide(organisationName_), L"");
    drawColumn(2, L"Session time",
        SessionTimeText(), SessionStateText());
}

void NativeConnectWindow::PaintTrustCard(
    HDC hdc, const RECT& client) {
    const int margin = S(22);
    RECT card{
        margin, S(202),
        client.right - margin, S(268)
    };
    RoundBox(hdc, card, S(14), kBlueSoft);

    const int iconCircle = S(42);
    const int iconLeft = card.left + S(16);
    const int iconTop = card.top + S(12);
    Circle(hdc, iconLeft, iconTop, iconCircle,
        RGB(218, 235, 255));
    DrawShieldIcon(hdc,
        iconLeft + S(11), iconTop + S(9), S(20), kBlue);

    RECT headline{
        iconLeft + iconCircle + S(14),
        card.top + S(10),
        card.right - S(42),
        card.top + S(34)
    };
    RECT subline{
        headline.left,
        card.top + S(34),
        card.right - S(42),
        card.bottom - S(8)
    };
    DrawTextStyled(hdc, L"This is a temporary support session",
        headline, headingFont_, kText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextStyled(hdc,
        L"No managed Hi5Central Agent is installed on this device.",
        subline, smallFont_, RGB(87, 110, 149),
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawInfoIcon(hdc,
        card.right - S(31),
        card.top + S(21),
        S(18),
        kBlue);
}

void NativeConnectWindow::PaintChat(
    HDC hdc, const RECT& client) {
    (void)client;

    RoundBox(hdc, chatRect_, S(14), kCard, kBorder);

    const int innerLeft = chatRect_.left + S(16);
    const int innerRight = chatRect_.right - S(16);
    const int innerTop = chatRect_.top + S(14);
    const int innerBottom = chatRect_.bottom - S(14);
    chatViewportHeight_ = std::max(1, innerBottom - innerTop);

    struct MeasuredMessage {
        int bodyHeight{ 0 };
        int bubbleHeight{ 0 };
        int blockHeight{ 0 };
        int bubbleWidth{ 0 };
    };

    const int avatarSize = S(34);
    const int gap = S(10);
    const int messageWidth =
        std::max(S(170), innerRight - innerLeft - avatarSize - gap);
    const int bubbleMaxWidth =
        std::max(S(150), messageWidth * 74 / 100);
    const int textWidth = std::max(S(100), bubbleMaxWidth - S(24));

    std::vector<MeasuredMessage> measured;
    measured.reserve(messages_.size());

    int totalHeight = 0;
    for (const auto& message : messages_) {
        const std::wstring body = Utf8ToWide(message.body);
        const int bodyHeight =
            MeasureWrappedText(hdc, body, bodyFont_, textWidth);
        const int bubbleHeight =
            std::max(S(42), bodyHeight + S(20));
        const int blockHeight =
            S(18) + S(4) + bubbleHeight + S(12);
        measured.push_back({
            bodyHeight,
            bubbleHeight,
            blockHeight,
            bubbleMaxWidth
        });
        totalHeight += blockHeight;
    }

    chatContentHeight_ = totalHeight;
    const int maxScroll =
        std::max(0, chatContentHeight_ - chatViewportHeight_);
    if (autoScrollChat_) {
        chatScrollOffset_ = maxScroll;
        autoScrollChat_ = false;
    } else {
        chatScrollOffset_ =
            std::clamp(chatScrollOffset_, 0, maxScroll);
    }

    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc,
        innerLeft, innerTop, innerRight, innerBottom);

    if (messages_.empty()) {
        RECT empty{
            innerLeft + S(18),
            innerTop + S(40),
            innerRight - S(18),
            innerBottom - S(20)
        };
        DrawTextStyled(hdc,
            remoteControlActive_
                ? L"Chat is ready. Messages from your technician will appear here."
                : L"Your secure chat will become available when the technician connects.",
            empty, smallFont_, RGB(126, 145, 172),
            DT_CENTER | DT_WORDBREAK);
        RestoreDC(hdc, saved);
        return;
    }

    int y = innerTop - chatScrollOffset_;
    for (size_t i = 0; i < messages_.size(); ++i) {
        const auto& message = messages_[i];
        const auto& size = measured[i];
        const bool outgoing = message.outgoing;

        const int avatarLeft = outgoing
            ? innerRight - avatarSize
            : innerLeft;
        const int bubbleRightLimit = outgoing
            ? avatarLeft - gap
            : innerRight;
        const int bubbleLeftLimit = outgoing
            ? innerLeft
            : avatarLeft + avatarSize + gap;

        const int bubbleWidth =
            std::min(size.bubbleWidth,
                bubbleRightLimit - bubbleLeftLimit);
        const int bubbleLeft = outgoing
            ? bubbleRightLimit - bubbleWidth
            : bubbleLeftLimit;
        const int bubbleRight = bubbleLeft + bubbleWidth;

        std::wstring name = outgoing
            ? L"You"
            : Utf8ToWide(
                message.displayName.empty()
                    ? technicianName_
                    : message.displayName);
        if (!outgoing &&
            name.find(L"(Technician)") == std::wstring::npos) {
            name += L" (Technician)";
        }
        const std::wstring meta =
            name + L"   " + message.timeText;

        RECT metaRect{
            bubbleLeft,
            y,
            bubbleRight,
            y + S(18)
        };
        DrawTextStyled(hdc, meta, metaRect,
            tinyFont_, RGB(104, 126, 162),
            outgoing
                ? DT_RIGHT | DT_VCENTER | DT_SINGLELINE
                : DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        const int bubbleTop = y + S(22);
        RECT bubble{
            bubbleLeft,
            bubbleTop,
            bubbleRight,
            bubbleTop + size.bubbleHeight
        };
        RoundBox(hdc, bubble, S(13),
            outgoing ? kUserBubble : kTechBubble);

        RECT body{
            bubble.left + S(12),
            bubble.top + S(9),
            bubble.right - S(12),
            bubble.bottom - S(8)
        };
        DrawTextStyled(hdc, Utf8ToWide(message.body),
            body, bodyFont_,
            outgoing ? kBlueDark : kText,
            DT_LEFT | DT_WORDBREAK);

        const int avatarTop =
            bubble.top + std::max(0,
                (size.bubbleHeight - avatarSize) / 2);
        Circle(hdc, avatarLeft, avatarTop,
            avatarSize, kBlue);
        RECT avatarText{
            avatarLeft,
            avatarTop,
            avatarLeft + avatarSize,
            avatarTop + avatarSize
        };
        DrawTextStyled(hdc,
            outgoing
                ? L"Y"
                : Initials(
                    Utf8ToWide(message.displayName.empty()
                        ? technicianName_
                        : message.displayName)),
            avatarText, smallFont_, RGB(255, 255, 255),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        y += size.blockHeight;
    }

    if (chatContentHeight_ > chatViewportHeight_) {
        const int trackHeight = chatViewportHeight_;
        const int thumbHeight = std::max(
            S(36),
            trackHeight * chatViewportHeight_
                / std::max(1, chatContentHeight_));
        const int travel = std::max(1, trackHeight - thumbHeight);
        const int thumbTop =
            innerTop + travel * chatScrollOffset_
                / std::max(1, maxScroll);

        RECT thumb{
            chatRect_.right - S(7),
            thumbTop,
            chatRect_.right - S(4),
            thumbTop + thumbHeight
        };
        RoundBox(hdc, thumb, S(2), RGB(202, 213, 227));
    }

    RestoreDC(hdc, saved);
}

void NativeConnectWindow::PaintComposer(
    HDC hdc, const RECT& client) {
    (void)client;

    RoundBox(hdc, composerRect_, S(13),
        kCard, kBorder);

    const int iconX = composerRect_.left + S(14);
    const int iconY = composerRect_.top + S(13);
    DrawAttachmentIcon(hdc,
        iconX, iconY, S(17), S(24),
        RGB(91, 114, 153), kCard);

    const bool sendEnabled =
        remoteControlActive_ && !ending_;
    const COLORREF sendFill = !sendEnabled
        ? RGB(190, 204, 222)
        : (hoverSend_ ? RGB(8, 94, 224) : kBlue);
    RoundBox(hdc, sendRect_, S(13), sendFill);

    const int planeX = sendRect_.left + S(18);
    const int planeY = sendRect_.top + S(14);
    DrawPaperPlaneIcon(hdc,
        planeX, planeY, S(24), S(22),
        RGB(255, 255, 255));

    RECT sendLabel{
        sendRect_.left + S(48),
        sendRect_.top,
        sendRect_.right - S(10),
        sendRect_.bottom
    };
    DrawTextStyled(hdc, L"Send", sendLabel,
        headingFont_, RGB(255, 255, 255),
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void NativeConnectWindow::PaintFooter(
    HDC hdc, const RECT& client) {
    RECT footer{
        0,
        composerRect_.bottom + S(10),
        client.right,
        client.bottom
    };
    FillSolid(hdc, footer, RGB(242, 246, 250));

    if (ending_) {
        RECT disabled = endRect_;
        RoundBox(hdc, disabled, S(12),
            RGB(248, 226, 229), RGB(238, 186, 193));
    } else {
        if (hoverEnd_) {
            RECT shadow = endRect_;
            OffsetRect(&shadow, 0, S(2));
            RoundBox(hdc, shadow, S(12),
                RGB(252, 214, 219));
        }
        RoundBox(hdc, endRect_, S(12),
            hoverEnd_ ? RGB(255, 235, 238) : kDangerSoft,
            kDangerBorder, S(1));
    }

    const COLORREF dangerText =
        ending_ ? RGB(182, 99, 108) : kDanger;

    const int iconSize = S(22);
    const int iconLeft = endRect_.left + S(16);
    const int iconTop =
        endRect_.top +
        (endRect_.bottom - endRect_.top - iconSize) / 2;
    Circle(hdc, iconLeft, iconTop, iconSize,
        dangerText);

    const int iconCenterX = iconLeft + iconSize / 2;
    const int iconCenterY = iconTop + iconSize / 2;
    RECT stop = CenteredSquareRect(
        iconCenterX, iconCenterY, S(8));
    FillSolid(hdc, stop, RGB(255, 255, 255));

    RECT label{
        iconLeft + iconSize + S(10),
        endRect_.top,
        endRect_.right - S(12),
        endRect_.bottom
    };
    DrawTextStyled(hdc,
        ending_ ? L"Ending..." : L"End session",
        label, headingFont_, dangerText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void NativeConnectWindow::ApplyIdentity(
    const std::string& technicianName,
    const std::string& organisationName) {
    technicianName_ = technicianName.empty()
        ? "Hi5Central technician" : technicianName;
    organisationName_ = organisationName.empty()
        ? "Hi5Central" : organisationName;
    if (HWND hwnd = hwnd_.load()) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void NativeConnectWindow::ApplyConnectionState(
    const std::string& statusText,
    bool remoteControlActive) {
    statusText_ = statusText.empty()
        ? (remoteControlActive
            ? "Connected"
            : "Waiting for technician")
        : statusText;
    remoteControlActive_ = remoteControlActive;

    if (remoteControlActive && !remoteControlStarted_) {
        remoteControlStarted_ = true;
        remoteControlStartedAt_ =
            std::chrono::steady_clock::now();
    }

    if (chatInput_) {
        EnableWindow(chatInput_,
            remoteControlActive && !ending_ ? TRUE : FALSE);
        SendMessageW(chatInput_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(
                remoteControlActive
                    ? L"Type a message..."
                    : L"Chat becomes available when the technician connects"));
    }

    if (HWND hwnd = hwnd_.load()) {
        InvalidateRect(hwnd, nullptr, FALSE);
        if (remoteControlActive && IsIconic(hwnd)) {
            FlashTaskbar();
        }
    }
}

void NativeConnectWindow::AppendMessageOnUiThread(
    const ConnectChatMessage& message) {
    if (message.body.empty()) return;

    DisplayMessage display{};
    display.sender = message.sender;
    display.displayName = message.displayName;
    display.body = message.body;
    display.timeText = CurrentTimeText();
    display.outgoing =
        message.sender == "user" ||
        message.sender == "customer";

    messages_.push_back(std::move(display));
    if (messages_.size() > 150) {
        messages_.erase(messages_.begin(),
            messages_.begin() + 25);
    }

    autoScrollChat_ = true;
    if (HWND hwnd = hwnd_.load()) {
        InvalidateRect(hwnd, &chatRect_, FALSE);
        if (IsIconic(hwnd)) FlashTaskbar();
    }
}

void NativeConnectWindow::HandleSend() {
    if (!remoteControlActive_ || ending_ || !chatInput_) return;

    const int length = GetWindowTextLengthW(chatInput_);
    if (length <= 0) return;

    std::wstring wide(
        static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(chatInput_, wide.data(), length + 1);
    wide.resize(static_cast<size_t>(length));

    while (!wide.empty() &&
        iswspace(wide.back())) {
        wide.pop_back();
    }
    size_t first = 0;
    while (first < wide.size() &&
        iswspace(wide[first])) {
        ++first;
    }
    if (first > 0) wide.erase(0, first);
    if (wide.empty()) return;

    const std::string body = WideToUtf8(wide);
    if (body.empty()) return;

    ConnectChatMessage local{};
    local.sender = "user";
    local.displayName = "You";
    local.body = body;
    AppendMessageOnUiThread(local);
    SetWindowTextW(chatInput_, L"");

    if (onSend_) onSend_(body);
}

void NativeConnectWindow::HandleEndSession() {
    HWND hwnd = hwnd_.load();
    if (!hwnd || ending_) return;

    const int answer = MessageBoxW(hwnd,
        L"End this Hi5Central support session?\n\n"
        L"The technician will immediately lose access to this computer.",
        L"End remote support session",
        MB_YESNO | MB_ICONWARNING |
        MB_SETFOREGROUND | MB_TOPMOST);
    if (answer != IDYES) return;

    ending_ = true;
    remoteControlActive_ = false;
    statusText_ = "Ending session...";

    if (chatInput_) EnableWindow(chatInput_, FALSE);
    InvalidateRect(hwnd, nullptr, FALSE);

    if (onEnd_) onEnd_();
}

void NativeConnectWindow::FlashTaskbar() {
    HWND hwnd = hwnd_.load();
    if (!hwnd) return;

    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = hwnd;
    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    flash.uCount = 3;
    flash.dwTimeout = 0;
    FlashWindowEx(&flash);
}

void NativeConnectWindow::UpdateHoverState(POINT point) {
    const bool send =
        PtInRect(&sendRect_, point) &&
        remoteControlActive_ && !ending_;
    const bool end =
        PtInRect(&endRect_, point) &&
        !ending_;

    if (send != hoverSend_ || end != hoverEnd_) {
        hoverSend_ = send;
        hoverEnd_ = end;
        if (HWND hwnd = hwnd_.load()) {
            InvalidateRect(hwnd, &sendRect_, FALSE);
            InvalidateRect(hwnd, &endRect_, FALSE);
        }
    }

    if ((send || end)) {
        SetCursor(LoadCursorW(
            nullptr, MAKEINTRESOURCEW(32649))); // IDC_HAND
    }
}

void NativeConnectWindow::UpdateChatScroll(int delta) {
    if (chatContentHeight_ <= chatViewportHeight_) {
        chatScrollOffset_ = 0;
        return;
    }

    const int step = S(48);
    const int direction =
        delta > 0 ? -step : step;
    const int maxScroll =
        std::max(0,
            chatContentHeight_ - chatViewportHeight_);
    chatScrollOffset_ =
        std::clamp(chatScrollOffset_ + direction,
            0, maxScroll);
    autoScrollChat_ = false;

    if (HWND hwnd = hwnd_.load()) {
        InvalidateRect(hwnd, &chatRect_, FALSE);
    }
}

void NativeConnectWindow::DrainActions() {
    UiAction action;
    while (PopAction(action)) {
        switch (action.type) {
        case UiActionType::Identity:
            ApplyIdentity(action.first, action.second);
            break;
        case UiActionType::ConnectionState:
            ApplyConnectionState(action.first, action.flag);
            break;
        case UiActionType::ChatMessage:
            AppendMessageOnUiThread(action.chat);
            break;
        case UiActionType::Restore:
            if (HWND hwnd = hwnd_.load()) {
                if (IsIconic(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            }
            break;
        case UiActionType::Stop:
            running_.store(false);
            if (HWND hwnd = hwnd_.load()) {
                DestroyWindow(hwnd);
            }
            return;
        }
    }
}

LRESULT CALLBACK NativeConnectWindow::StaticWndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    NativeConnectWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create =
            reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<NativeConnectWindow*>(
            create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<NativeConnectWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self->WndProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK NativeConnectWindow::StaticInputProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND parent = GetParent(hwnd);
    auto* self = parent
        ? reinterpret_cast<NativeConnectWindow*>(
            GetWindowLongPtrW(parent, GWLP_USERDATA))
        : nullptr;

    if (self && msg == WM_KEYDOWN &&
        wParam == VK_RETURN) {
        self->HandleSend();
        return 0;
    }

    if (self && self->inputOldProc_) {
        return CallWindowProcW(
            self->inputOldProc_,
            hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT NativeConnectWindow::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HI5_CONNECT_QUEUE:
        DrainActions();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        HDC memory = CreateCompatibleDC(dc);
        const int paintWidth = std::max(1, static_cast<int>(client.right));
        const int paintHeight = std::max(1, static_cast<int>(client.bottom));
        HBITMAP bitmap = CreateCompatibleBitmap(
            dc, paintWidth, paintHeight);
        HGDIOBJ oldBitmap =
            SelectObject(memory, bitmap);

        Paint(memory, client);
        BitBlt(dc, 0, 0,
            client.right, client.bottom,
            memory, 0, 0, SRCCOPY);

        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wParam == TIMER_DURATION) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            LayoutChildren();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DPICHANGED: {
        const UINT dpi = LOWORD(wParam);
        dpiScale_ = std::max(
            1.0f, static_cast<float>(dpi) / 96.0f);
        RECT* suggested =
            reinterpret_cast<RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd, nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        CreateFonts();
        LayoutChildren();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lParam) == chatInput_) {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            SetTextColor(editDc, kText);
            SetBkColor(editDc, kCard);
            return reinterpret_cast<LRESULT>(inputBrush_);
        }
        break;

    case WM_MOUSEMOVE: {
        POINT point{
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam)
        };
        UpdateHoverState(point);
        if (!trackingMouse_) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            trackingMouse_ = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (hoverSend_ || hoverEnd_) {
            hoverSend_ = false;
            hoverEnd_ = false;
            InvalidateRect(hwnd, &sendRect_, FALSE);
            InvalidateRect(hwnd, &endRect_, FALSE);
        }
        return 0;

    case WM_LBUTTONUP: {
        POINT point{
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam)
        };
        if (PtInRect(&sendRect_, point) &&
            remoteControlActive_ && !ending_) {
            HandleSend();
            return 0;
        }
        if (PtInRect(&endRect_, point) && !ending_) {
            HandleEndSession();
            return 0;
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        POINT point{
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam)
        };
        ScreenToClient(hwnd, &point);
        if (PtInRect(&chatRect_, point)) {
            UpdateChatScroll(
                GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == kChatInputId &&
            HIWORD(wParam) == EN_CHANGE) {
            InvalidateRect(hwnd, &sendRect_, FALSE);
            return 0;
        }
        break;

    case WM_CLOSE:
        HandleEndSession();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_DURATION);
        hwnd_.store(nullptr);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace hi5
