#include "native_connect_window.h"
#include "../util/log.h"

#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace hi5 {
namespace {

constexpr wchar_t kConnectWindowClass[] = L"Hi5CentralConnectCustomerWindow";
constexpr int kHeaderId = 2101;
constexpr int kStatusId = 2102;
constexpr int kTechnicianId = 2103;
constexpr int kOrganisationId = 2104;
constexpr int kDurationId = 2105;
constexpr int kTrustId = 2106;
constexpr int kChatLogId = 2107;
constexpr int kChatInputId = 2108;
constexpr int kSendId = 2109;
constexpr int kEndId = 2110;

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

std::wstring DisplayNameFor(const ConnectChatMessage& message) {
    if (!message.displayName.empty()) return Utf8ToWide(message.displayName);
    return message.sender == "user" ? L"You" : L"Technician";
}

void SetControlFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
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
    technicianName_ = technicianName;
    organisationName_ = organisationName;
    onSend_ = std::move(onSend);
    onEnd_ = std::move(onEnd);
    remoteControlActive_ = false;
    remoteControlStarted_ = false;
    ending_ = false;

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

bool NativeConnectWindow::CreateUi() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &NativeConnectWindow::StaticWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kConnectWindowClass;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32516)); // IDI_INFORMATION
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogWarn("[connect-ui] failed to register customer window class");
        return false;
    }

    RECT windowRect{ 0, 0, 520, 570 };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;
    AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);

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
        LogWarn("[connect-ui] failed to create customer support window");
        return false;
    }
    hwnd_.store(hwnd);

    const UINT dpi = GetDpiForWindow(hwnd);
    titleFont_ = CreateFontW(-MulDiv(18, dpi, 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    bodyFont_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    smallFont_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    headerLabel_ = CreateWindowExW(0, L"STATIC", L"Hi5Central Remote Support",
        WS_CHILD | WS_VISIBLE, 0, 0, 100, 30, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHeaderId)), instance, nullptr);
    statusLabel_ = CreateWindowExW(0, L"STATIC", L"● Waiting for technician",
        WS_CHILD | WS_VISIBLE, 0, 0, 100, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)), instance, nullptr);
    technicianLabel_ = CreateWindowExW(0, L"STATIC", L"Technician:",
        WS_CHILD | WS_VISIBLE, 0, 0, 100, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTechnicianId)), instance, nullptr);
    organisationLabel_ = CreateWindowExW(0, L"STATIC", L"Organisation:",
        WS_CHILD | WS_VISIBLE, 0, 0, 100, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOrganisationId)), instance, nullptr);
    durationLabel_ = CreateWindowExW(0, L"STATIC",
        L"Remote control has not started",
        WS_CHILD | WS_VISIBLE, 0, 0, 100, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDurationId)), instance, nullptr);
    trustLabel_ = CreateWindowExW(0, L"STATIC",
        L"This is a temporary support session. No managed Hi5Central Agent is installed.",
        WS_CHILD | WS_VISIBLE, 0, 0, 100, 40, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTrustId)), instance, nullptr);

    chatLog_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 100, 180, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatLogId)), instance, nullptr);
    chatInput_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 100, 32, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatInputId)), instance, nullptr);
    sendButton_ = CreateWindowExW(0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 80, 32, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSendId)), instance, nullptr);
    endButton_ = CreateWindowExW(0, L"BUTTON", L"End session",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 120, 36, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEndId)), instance, nullptr);

    SetControlFont(headerLabel_, titleFont_);
    SetControlFont(statusLabel_, bodyFont_);
    SetControlFont(technicianLabel_, bodyFont_);
    SetControlFont(organisationLabel_, bodyFont_);
    SetControlFont(durationLabel_, bodyFont_);
    SetControlFont(trustLabel_, smallFont_);
    SetControlFont(chatLog_, bodyFont_);
    SetControlFont(chatInput_, bodyFont_);
    SetControlFont(sendButton_, bodyFont_);
    SetControlFont(endButton_, bodyFont_);

    const int rounded = 2;
    DwmSetWindowAttribute(hwnd, 33, &rounded, sizeof(rounded));

    ApplyIdentity(technicianName_, organisationName_);
    ApplyConnectionState("Waiting for technician", false);
    LayoutChildren();
    CenterWindow();

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetTimer(hwnd, TIMER_DURATION, 1000, nullptr);
    SetForegroundWindow(hwnd);
    return true;
}
void NativeConnectWindow::DestroyUi() {
    if (HWND hwnd = hwnd_.exchange(nullptr)) {
        if (IsWindow(hwnd)) {
            KillTimer(hwnd, TIMER_DURATION);
            DestroyWindow(hwnd);
        }
    }

    headerLabel_ = nullptr;
    statusLabel_ = nullptr;
    technicianLabel_ = nullptr;
    organisationLabel_ = nullptr;
    durationLabel_ = nullptr;
    trustLabel_ = nullptr;
    chatLog_ = nullptr;
    chatInput_ = nullptr;
    sendButton_ = nullptr;
    endButton_ = nullptr;

    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
}

void NativeConnectWindow::LayoutChildren() {
    HWND hwnd = hwnd_.load();
    if (!hwnd) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int margin = 22;
    const int contentWidth = std::max(100, width - margin * 2);

    MoveWindow(headerLabel_, margin, 20, contentWidth, 34, TRUE);
    MoveWindow(statusLabel_, margin, 58, contentWidth, 24, TRUE);
    MoveWindow(technicianLabel_, margin, 96, contentWidth, 24, TRUE);
    MoveWindow(organisationLabel_, margin, 122, contentWidth, 24, TRUE);
    MoveWindow(durationLabel_, margin, 148, contentWidth, 24, TRUE);
    MoveWindow(trustLabel_, margin, 181, contentWidth, 42, TRUE);

    MoveWindow(chatLog_, margin, 236, contentWidth, 220, TRUE);

    const int sendWidth = 82;
    MoveWindow(chatInput_, margin, 470,
        std::max(100, contentWidth - sendWidth - 10), 34, TRUE);
    MoveWindow(sendButton_, margin + contentWidth - sendWidth, 470,
        sendWidth, 34, TRUE);

    const int endWidth = 126;
    MoveWindow(endButton_, width - margin - endWidth, 518,
        endWidth, 36, TRUE);
}

void NativeConnectWindow::CenterWindow() {
    HWND hwnd = hwnd_.load();
    if (!hwnd) return;

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    const int x = info.rcWork.left + std::max(0, (workWidth - width) / 2);
    const int y = info.rcWork.top + std::max(0, (workHeight - height) / 2);

    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void NativeConnectWindow::ApplyIdentity(
    const std::string& technicianName,
    const std::string& organisationName) {
    technicianName_ = technicianName.empty()
        ? "Hi5Central technician" : technicianName;
    organisationName_ = organisationName.empty()
        ? "Hi5Central" : organisationName;
    const std::wstring technician =
        L"Technician: " + Utf8ToWide(technicianName_);
    const std::wstring organisation =
        L"Organisation: " + Utf8ToWide(organisationName_);
    SetWindowTextW(technicianLabel_, technician.c_str());
    SetWindowTextW(organisationLabel_, organisation.c_str());
}

void NativeConnectWindow::ApplyConnectionState(
    const std::string& statusText,
    bool remoteControlActive) {
    remoteControlActive_ = remoteControlActive;
    if (remoteControlActive && !remoteControlStarted_) {
        remoteControlStarted_ = true;
        remoteControlStartedAt_ = std::chrono::steady_clock::now();
    }

    const std::string normalized = statusText.empty()
        ? (remoteControlActive ? "Connected" : "Waiting for technician")
        : statusText;
    const std::wstring status = L"● " + Utf8ToWide(normalized);
    SetWindowTextW(statusLabel_, status.c_str());

    EnableWindow(chatInput_, remoteControlActive ? TRUE : FALSE);
    EnableWindow(sendButton_, remoteControlActive ? TRUE : FALSE);
    UpdateDurationText();

    if (HWND hwnd = hwnd_.load()) {
        InvalidateRect(hwnd, nullptr, TRUE);
        if (remoteControlActive && IsIconic(hwnd)) {
            FLASHWINFO flash{};
            flash.cbSize = sizeof(flash);
            flash.hwnd = hwnd;
            flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
            flash.uCount = 3;
            FlashWindowEx(&flash);
        }
    }
}
void NativeConnectWindow::UpdateDurationText() {
    if (!durationLabel_) return;

    if (!remoteControlActive_ || !remoteControlStarted_) {
        SetWindowTextW(durationLabel_, L"Remote control has not started");
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - remoteControlStartedAt_).count();
    const long long hours = elapsed / 3600;
    const long long minutes = (elapsed % 3600) / 60;
    const long long seconds = elapsed % 60;

    std::wostringstream text;
    text << L"Remote control active for ";
    if (hours > 0) {
        text << hours << L":"
             << std::setw(2) << std::setfill(L'0') << minutes << L":";
    } else {
        text << minutes << L":";
    }
    text << std::setw(2) << std::setfill(L'0') << seconds;
    SetWindowTextW(durationLabel_, text.str().c_str());
}

void NativeConnectWindow::AppendMessageOnUiThread(
    const ConnectChatMessage& message) {
    if (!chatLog_ || message.body.empty()) return;
    std::wstring line = DisplayNameFor(message);
    line += L": ";
    line += Utf8ToWide(message.body);
    line += L"\r\n\r\n";

    SendMessageW(chatLog_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(chatLog_, EM_REPLACESEL, FALSE,
        reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(chatLog_, EM_SCROLLCARET, 0, 0);

    if (HWND hwnd = hwnd_.load(); hwnd && IsIconic(hwnd)) {
        FLASHWINFO flash{};
        flash.cbSize = sizeof(flash);
        flash.hwnd = hwnd;
        flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        flash.uCount = 3;
        flash.dwTimeout = 0;
        FlashWindowEx(&flash);
    }
}

void NativeConnectWindow::HandleSend() {
    if (!remoteControlActive_ || !chatInput_) return;
    const int length = GetWindowTextLengthW(chatInput_);
    if (length <= 0) return;

    std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(chatInput_, wide.data(), length + 1);
    wide.resize(static_cast<size_t>(length));
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
        MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND);
    if (answer != IDYES) return;

    ending_ = true;
    EnableWindow(endButton_, FALSE);
    EnableWindow(sendButton_, FALSE);
    EnableWindow(chatInput_, FALSE);
    SetWindowTextW(statusLabel_, L"● Ending session...");
    if (onEnd_) onEnd_();
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
                if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            }
            break;
        case UiActionType::Stop:
            running_.store(false);
            if (HWND hwnd = hwnd_.load()) DestroyWindow(hwnd);
            return;
        }
    }
}

LRESULT CALLBACK NativeConnectWindow::StaticWndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    NativeConnectWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<NativeConnectWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<NativeConnectWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->WndProc(hwnd, msg, wParam, lParam);
}

LRESULT NativeConnectWindow::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HI5_CONNECT_QUEUE:
        DrainActions();
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DURATION) {
            UpdateDurationText();
            return 0;
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) LayoutChildren();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kSendId:
            if (HIWORD(wParam) == BN_CLICKED) HandleSend();
            return 0;
        case kEndId:
            if (HIWORD(wParam) == BN_CLICKED) HandleEndSession();
            return 0;
        default:
            break;
        }
        break;

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        if (reinterpret_cast<HWND>(lParam) == statusLabel_) {
            SetTextColor(dc, remoteControlActive_
                ? RGB(22, 128, 61) : RGB(180, 83, 9));
        } else {
            SetTextColor(dc, RGB(30, 41, 59));
        }
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

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
