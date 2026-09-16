#include "native_chat_window.h"
#include "util/log.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace hi5 {

    namespace {
        constexpr wchar_t kChatWindowClass[] = L"Hi5CentralNativeChatWindow";
        constexpr int kListId = 1001;
        constexpr int kEditId = 1002;
        constexpr int kSendId = 1003;
        constexpr int kCloseId = 1004;

        std::wstring Utf8ToWide(const std::string& s) {
            if (s.empty()) return std::wstring();
            const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            if (len <= 0) return std::wstring();
            std::wstring out(static_cast<size_t>(len), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
            return out;
        }

        std::string WideToUtf8(const std::wstring& s) {
            if (s.empty()) return std::string();
            const int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            if (len <= 0) return std::string();
            std::string out(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
            return out;
        }

        std::wstring FormatLine(const ChatMessage& msg) {
            std::wstring who = Utf8ToWide(msg.displayName.empty() ? msg.sender : msg.displayName);
            std::wstring body = Utf8ToWide(msg.body);
            if (who.empty()) who = L"Message";
            return who + L": " + body;
        }
    }

    NativeChatWindow::NativeChatWindow() {
    }

    NativeChatWindow::~NativeChatWindow() {
        Stop();
    }

    bool NativeChatWindow::Start(const std::string& sessionId, SendCallback onSend) {
        Stop();

        dismissedByUser_ = false;
        sessionId_ = sessionId;
        onSend_ = std::move(onSend);
        readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!readyEvent_) {
            return false;
        }

        running_ = true;
        uiThread_ = std::thread([this]() { UiThreadMain(); });

        const DWORD wait = WaitForSingleObject(readyEvent_, 5000);
        return wait == WAIT_OBJECT_0 && hwnd_ != nullptr;
    }

    void NativeChatWindow::Stop() {
        running_ = false;

        if (hwnd_) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        else if (uiThreadId_ != 0) {
            PostThreadMessageW(uiThreadId_, WM_QUIT, 0, 0);
        }

        if (uiThread_.joinable()) {
            uiThread_.join();
        }

        if (readyEvent_) {
            CloseHandle(readyEvent_);
            readyEvent_ = nullptr;
        }

        hwnd_ = nullptr;
        listBox_ = nullptr;
        editBox_ = nullptr;
        sendButton_ = nullptr;
        closeButton_ = nullptr;
        uiThreadId_ = 0;
        onSend_ = nullptr;
        sessionId_.clear();

        if (font_) { DeleteObject(font_); font_ = nullptr; }
        if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
        if (backgroundBrush_) { DeleteObject(backgroundBrush_); backgroundBrush_ = nullptr; }
        if (controlBrush_) { DeleteObject(controlBrush_); controlBrush_ = nullptr; }

        std::lock_guard<std::mutex> lock(queueMu_);
        queue_.clear();
    }

    void NativeChatWindow::Show() {
        PostUiMessage(PendingUiMessage{ PendingUiMessage::Type::Show, {} });
    }

    void NativeChatWindow::Hide() {
        PostUiMessage(PendingUiMessage{ PendingUiMessage::Type::Hide, {} });
    }

    void NativeChatWindow::AppendMessage(const ChatMessage& msg) {
        PendingUiMessage item;
        item.type = PendingUiMessage::Type::Append;
        item.chat = msg;
        PostUiMessage(item);
    }

    void NativeChatWindow::Clear() {
        PostUiMessage(PendingUiMessage{ PendingUiMessage::Type::Clear, {} });
    }

    void NativeChatWindow::PostUiMessage(const PendingUiMessage& item) {
        {
            std::lock_guard<std::mutex> lock(queueMu_);
            queue_.push_back(item);
        }
        if (hwnd_) {
            PostMessageW(hwnd_, WM_HI5_CHAT_QUEUE, 0, 0);
        }
    }

    bool NativeChatWindow::PopUiMessage(PendingUiMessage& item) {
        std::lock_guard<std::mutex> lock(queueMu_);
        if (queue_.empty()) return false;
        item = queue_.front();
        queue_.erase(queue_.begin());
        return true;
    }

    void NativeChatWindow::UiThreadMain() {
        uiThreadId_ = GetCurrentThreadId();
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        CreateUi();
        if (readyEvent_) {
            SetEvent(readyEvent_);
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        DestroyUi();
    }

    bool NativeChatWindow::CreateUi() {
        HINSTANCE hinst = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &NativeChatWindow::StaticWndProc;
        wc.hInstance = hinst;
        wc.lpszClassName = kChatWindowClass;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kChatWindowClass,
            L"Hi5Central Support Chat",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            420,
            560,
            nullptr,
            nullptr,
            hinst,
            this
        );

        if (!hwnd_) {
            return false;
        }

        const UINT dpi = GetDpiForWindow(hwnd_);
        const int bodyFontPx = -MulDiv(10, static_cast<int>(dpi), 72);
        const int titleFontPx = -MulDiv(12, static_cast<int>(dpi), 72);
        font_ = CreateFontW(bodyFontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI Variable");
        titleFont_ = CreateFontW(titleFontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI Variable");
        backgroundBrush_ = CreateSolidBrush(RGB(247, 249, 252));
        controlBrush_ = CreateSolidBrush(RGB(255, 255, 255));

        const int cornerPreference = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPreference, sizeof(cornerPreference));

        listBox_ = CreateWindowExW(
            0,
            L"LISTBOX",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            12, 12, 380, 220,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
            hinst,
            nullptr
        );

        editBox_ = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
            12, 245, 290, 28,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
            hinst,
            nullptr
        );

        sendButton_ = CreateWindowExW(
            0,
            L"BUTTON",
            L"Send",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            312, 245, 80, 28,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSendId)),
            hinst,
            nullptr
        );

        closeButton_ = CreateWindowExW(
            0, L"BUTTON", L"×", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 32, 28, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)), hinst, nullptr);

        if (font_) {
            SendMessageW(listBox_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(editBox_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(sendButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            if (closeButton_) SendMessageW(closeButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }

        LayoutChildren();
        ClampToCurrentWorkArea(true);
        ShowWindow(hwnd_, SW_HIDE);
        UpdateWindow(hwnd_);
        return true;
    }

    void NativeChatWindow::DestroyUi() {
        if (closeButton_) DestroyWindow(closeButton_);
        if (sendButton_) DestroyWindow(sendButton_);
        if (editBox_) DestroyWindow(editBox_);
        if (listBox_) DestroyWindow(listBox_);
        if (hwnd_) DestroyWindow(hwnd_);

        closeButton_ = nullptr;
        sendButton_ = nullptr;
        editBox_ = nullptr;
        listBox_ = nullptr;
        hwnd_ = nullptr;
    }

    void NativeChatWindow::AppendMessageOnUiThread(const ChatMessage& msg) {
        if (!listBox_) return;

        const std::wstring line = FormatLine(msg);
        SendMessageW(listBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));

        const LRESULT count = SendMessageW(listBox_, LB_GETCOUNT, 0, 0);
        if (count > 0) {
            SendMessageW(listBox_, LB_SETTOPINDEX, static_cast<WPARAM>(count - 1), 0);
        }
    }

    void NativeChatWindow::ShowOnUiThread() {
        if (!hwnd_) return;
        dismissedByUser_ = false;
        ClampToCurrentWorkArea(false);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
    }

    void NativeChatWindow::HideOnUiThread() {
        if (!hwnd_) return;
        ShowWindow(hwnd_, SW_HIDE);
    }

    void NativeChatWindow::ClearOnUiThread() {
        if (!listBox_) return;
        SendMessageW(listBox_, LB_RESETCONTENT, 0, 0);
    }

    void NativeChatWindow::LayoutChildren() {
        if (!hwnd_) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);
        const float scale = std::max(1.0f, static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f);
        const auto px = [scale](int dip) { return std::max(1, static_cast<int>(dip * scale)); };
        const int margin = px(12);
        const int headerHeight = px(72);
        const int composerHeight = px(40);
        const int buttonWidth = px(82);
        if (closeButton_) MoveWindow(closeButton_, std::max(margin, width - margin - px(34)), px(12), px(34), px(30), TRUE);
        const int listTop = headerHeight;
        const int listHeight = std::max(px(92), height - headerHeight - composerHeight - margin * 3);
        if (listBox_) MoveWindow(listBox_, margin, listTop, std::max(px(120), width - margin * 2), listHeight, TRUE);
        const int composerY = listTop + listHeight + margin;
        if (editBox_) MoveWindow(editBox_, margin, composerY,
            std::max(px(100), width - margin * 3 - buttonWidth), composerHeight, TRUE);
        if (sendButton_) MoveWindow(sendButton_, std::max(margin, width - margin - buttonWidth),
            composerY, buttonWidth, composerHeight, TRUE);
    }

    void NativeChatWindow::ClampToCurrentWorkArea(bool preferBottomRight) {
        if (!hwnd_) return;
        HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info)) return;

        RECT current{};
        GetWindowRect(hwnd_, &current);
        const int workLeft = static_cast<int>(info.rcWork.left);
        const int workTop = static_cast<int>(info.rcWork.top);
        const int workRight = static_cast<int>(info.rcWork.right);
        const int workBottom = static_cast<int>(info.rcWork.bottom);
        const int workW = std::max(1, workRight - workLeft);
        const int workH = std::max(1, workBottom - workTop);
        const int currentW = static_cast<int>(current.right - current.left);
        const int currentH = static_cast<int>(current.bottom - current.top);
        const int minW = std::min(300, std::max(220, workW - 16));
        const int minH = std::min(300, std::max(220, workH - 16));
        int width = std::min(currentW, std::max(minW, workW - 32));
        int height = std::min(currentH, std::max(minH, workH - 32));
        width = std::min(workW, std::max(minW, width));
        height = std::min(workH, std::max(minH, height));

        int x = static_cast<int>(current.left);
        int y = static_cast<int>(current.top);
        if (preferBottomRight) {
            x = workRight - width - 18;
            y = workBottom - height - 18;
        }
        x = std::max(workLeft, std::min(x, workRight - width));
        y = std::max(workTop, std::min(y, workBottom - height));
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        LayoutChildren();
    }

    void NativeChatWindow::HandleSendClicked() {
        if (!editBox_) return;

        const int len = GetWindowTextLengthW(editBox_);
        if (len <= 0) {
            return;
        }

        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(editBox_, text.data(), len + 1);
        text.resize(static_cast<size_t>(len));

        std::string body = WideToUtf8(text);
        if (body.empty()) {
            return;
        }

        ChatMessage msg{};
        msg.sessionId = sessionId_;
        msg.sender = "user";
        msg.displayName = "User";
        msg.body = body;

        AppendMessageOnUiThread(msg);
        SetWindowTextW(editBox_, L"");

        if (onSend_) {
            onSend_(body);
        }
    }

    LRESULT CALLBACK NativeChatWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        NativeChatWindow* self = nullptr;

        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<NativeChatWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else {
            self = reinterpret_cast<NativeChatWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        return self->WndProc(hwnd, msg, wParam, lParam);
    }

    LRESULT NativeChatWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{}; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, backgroundBrush_ ? backgroundBrush_ : GetSysColorBrush(COLOR_WINDOW));
            const float scale = std::max(1.0f, static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
            const auto px = [scale](int dip) { return std::max(1, static_cast<int>(dip * scale)); };
            RECT badge{ px(12), px(14), px(48), px(50) };
            HBRUSH accent = CreateSolidBrush(RGB(37, 99, 235));
            FillRect(dc, &badge, accent); DeleteObject(accent);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255,255,255));
            HFONT old = reinterpret_cast<HFONT>(SelectObject(dc, titleFont_ ? titleFont_ : font_));
            DrawTextW(dc, L"H5", -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT title{ px(60), px(12), rc.right - px(58), px(39) };
            SetTextColor(dc, RGB(17,24,39));
            DrawTextW(dc, L"Hi5Central Remote Support", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (font_) SelectObject(dc, font_);
            HBRUSH green = CreateSolidBrush(RGB(34,197,94));
            HGDIOBJ oldBrush = SelectObject(dc, green);
            Ellipse(dc, px(61), px(45), px(69), px(53));
            SelectObject(dc, oldBrush); DeleteObject(green);
            RECT status{ px(74), px(39), rc.right - px(12), px(61) };
            SetTextColor(dc, RGB(100,116,139));
            DrawTextW(dc, L"Connected · Support chat", -1, &status, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(219,227,236));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, px(12), px(68), nullptr); LineTo(dc, rc.right - px(12), px(68));
            SelectObject(dc, oldPen); DeleteObject(pen);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(17,24,39));
            SetBkColor(dc, RGB(255,255,255));
            return reinterpret_cast<LRESULT>(controlBrush_ ? controlBrush_ : GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlID == kSendId) {
                HBRUSH brush = CreateSolidBrush(RGB(37,99,235));
                FillRect(dis->hDC, &dis->rcItem, brush); DeleteObject(brush);
                SetBkMode(dis->hDC, TRANSPARENT); SetTextColor(dis->hDC, RGB(255,255,255));
                HFONT old = reinterpret_cast<HFONT>(SelectObject(dis->hDC, font_));
                RECT r = dis->rcItem; DrawTextW(dis->hDC, L"Send", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, old);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == kSendId && HIWORD(wParam) == BN_CLICKED) {
                HandleSendClicked();
                return 0;
            }
            if (LOWORD(wParam) == kCloseId && HIWORD(wParam) == BN_CLICKED) {
                dismissedByUser_ = true;
                HideOnUiThread();
                return 0;
            }
            if (LOWORD(wParam) == kEditId && HIWORD(wParam) == EN_MAXTEXT) {
                return 0;
            }
            break;

        case WM_KEYDOWN:
            if (wParam == VK_RETURN && GetFocus() == editBox_) {
                HandleSendClicked();
                return 0;
            }
            break;

        case WM_HI5_CHAT_QUEUE: {
            PendingUiMessage item;
            while (PopUiMessage(item)) {
                switch (item.type) {
                case PendingUiMessage::Type::Append:
                    AppendMessageOnUiThread(item.chat);
                    if (!dismissedByUser_) ShowOnUiThread();
                    break;
                case PendingUiMessage::Type::Show:
                    ShowOnUiThread();
                    break;
                case PendingUiMessage::Type::Hide:
                    HideOnUiThread();
                    break;
                case PendingUiMessage::Type::Clear:
                    ClearOnUiThread();
                    break;
                }
            }
            return 0;
        }

        case WM_SIZE:
            LayoutChildren();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            ClampToCurrentWorkArea(false);
            return 0;

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            ClampToCurrentWorkArea(false);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi) {
                RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                mmi->ptMinTrackSize.x = std::min<LONG>(300, std::max<LONG>(220, work.right - work.left));
                mmi->ptMinTrackSize.y = std::min<LONG>(300, std::max<LONG>(220, work.bottom - work.top));
            }
            return 0;
        }

        case WM_CLOSE:
            if (!running_.load()) {
                DestroyWindow(hwnd);
            } else {
                dismissedByUser_ = true;
                HideOnUiThread();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    namespace {
        std::string ChatHelperArg(int argc, char** argv, const std::string& key) {
            for (int i = 1; i + 1 < argc; ++i) {
                if (std::string(argv[i]) == key) return std::string(argv[i + 1]);
            }
            return {};
        }

        HANDLE OpenChatPipeClient(const std::string& name, DWORD access) {
            if (name.empty()) return INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 60; ++attempt) {
                HANDLE pipe = CreateFileA(name.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE) {
                    DWORD mode = PIPE_READMODE_MESSAGE;
                    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
                    return pipe;
                }
                if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) break;
                WaitNamedPipeA(name.c_str(), 100);
            }
            return INVALID_HANDLE_VALUE;
        }

        bool WriteChatHelperLine(HANDLE pipe, const nlohmann::json& payload) {
            if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;
            const std::string line = payload.dump() + "\n";
            DWORD written = 0;
            return WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size();
        }
    }

    int RunNativeChatMain(int argc, char** argv) {
        const std::string sessionId = ChatHelperArg(argc, argv, "--session");
        const std::string inPipeName = ChatHelperArg(argc, argv, "--pipe-in");
        const std::string outPipeName = ChatHelperArg(argc, argv, "--pipe-out");
        if (sessionId.empty() || inPipeName.empty() || outPipeName.empty()) {
            LogError("[native-chat] missing session/pipe arguments");
            return 2;
        }

        HANDLE inPipe = OpenChatPipeClient(inPipeName, GENERIC_READ);
        HANDLE outPipe = OpenChatPipeClient(outPipeName, GENERIC_WRITE);
        if (inPipe == INVALID_HANDLE_VALUE || outPipe == INVALID_HANDLE_VALUE) {
            if (inPipe != INVALID_HANDLE_VALUE) CloseHandle(inPipe);
            if (outPipe != INVALID_HANDLE_VALUE) CloseHandle(outPipe);
            LogError("[native-chat] failed to connect service pipes session=" + sessionId);
            return 3;
        }

        std::mutex outMu;
        NativeChatWindow window;
        if (!window.Start(sessionId, [&](const std::string& body) {
            std::lock_guard<std::mutex> lock(outMu);
            WriteChatHelperLine(outPipe, {
                {"type", "chat_message"}, {"session_id", sessionId},
                {"sender", "user"}, {"display_name", "Remote user"}, {"body", body}
            });
        })) {
            CloseHandle(inPipe);
            CloseHandle(outPipe);
            LogError("[native-chat] failed to create native window session=" + sessionId);
            return 4;
        }

        {
            std::lock_guard<std::mutex> lock(outMu);
            WriteChatHelperLine(outPipe, {{"type", "overlay_ready"}, {"session_id", sessionId}});
        }
        window.Show();
        LogInfo("[native-chat] ready and shown session=" + sessionId);

        std::string pending;
        char buffer[8192];
        bool running = true;
        while (running) {
            DWORD read = 0;
            BOOL ok = ReadFile(inPipe, buffer, sizeof(buffer), &read, nullptr);
            const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            if (read > 0) pending.append(buffer, buffer + read);

            for (;;) {
                const auto pos = pending.find('\n');
                if (pos == std::string::npos) break;
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                auto msg = nlohmann::json::parse(line, nullptr, false);
                if (msg.is_discarded()) continue;
                const std::string type = msg.value("type", std::string());
                if (type == "close") {
                    running = false;
                    break;
                }
                if (type == "show") {
                    window.Show();
                    continue;
                }
                if (type == "chat_message") {
                    ChatMessage chat{};
                    chat.sessionId = sessionId;
                    chat.sender = msg.value("sender", std::string("tech"));
                    chat.displayName = msg.value("display_name", std::string("Technician"));
                    chat.body = msg.value("body", std::string());
                    if (!chat.body.empty()) window.AppendMessage(chat);
                }
            }

            if (!ok && err != ERROR_MORE_DATA) break;
        }

        window.Stop();
        {
            std::lock_guard<std::mutex> lock(outMu);
            WriteChatHelperLine(outPipe, {{"type", "closed"}, {"session_id", sessionId}});
        }
        CloseHandle(inPipe);
        CloseHandle(outPipe);
        LogInfo("[native-chat] stopped session=" + sessionId);
        return 0;
    }

} // namespace hi5