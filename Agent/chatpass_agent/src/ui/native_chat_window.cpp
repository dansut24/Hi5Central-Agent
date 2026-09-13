#include "native_chat_window.h"

#include <windowsx.h>

#include <sstream>

namespace hi5 {

    namespace {
        constexpr wchar_t kChatWindowClass[] = L"Hi5CentralNativeChatWindow";
        constexpr int kListId = 1001;
        constexpr int kEditId = 1002;
        constexpr int kSendId = 1003;

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
        uiThreadId_ = 0;
        onSend_ = nullptr;
        sessionId_.clear();

        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }

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
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            420,
            360,
            nullptr,
            nullptr,
            hinst,
            this
        );

        if (!hwnd_) {
            return false;
        }

        font_ = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI");

        listBox_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
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
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
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
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            312, 245, 80, 28,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSendId)),
            hinst,
            nullptr
        );

        if (font_) {
            SendMessageW(listBox_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(editBox_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(sendButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }

        ShowWindow(hwnd_, SW_HIDE);
        UpdateWindow(hwnd_);
        return true;
    }

    void NativeChatWindow::DestroyUi() {
        if (sendButton_) DestroyWindow(sendButton_);
        if (editBox_) DestroyWindow(editBox_);
        if (listBox_) DestroyWindow(listBox_);
        if (hwnd_) DestroyWindow(hwnd_);

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
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
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

    void NativeChatWindow::HandleSendClicked() {
        if (!editBox_) return;

        const int len = GetWindowTextLengthW(editBox_);
        if (len <= 0) {
            return;
        }

        std::wstring text(static_cast<size_t>(len), L'\0');
        GetWindowTextW(editBox_, text.data(), len + 1);

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
        case WM_COMMAND:
            if (LOWORD(wParam) == kSendId && HIWORD(wParam) == BN_CLICKED) {
                HandleSendClicked();
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
                    ShowOnUiThread();
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

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace hi5