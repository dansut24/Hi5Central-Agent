#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hi5 {

    struct ChatMessage {
        std::string sessionId;
        std::string sender;
        std::string displayName;
        std::string body;
    };

    class NativeChatWindow {
    public:
        using SendCallback = std::function<void(const std::string&)>;

        NativeChatWindow();
        ~NativeChatWindow();

        bool Start(const std::string& sessionId, SendCallback onSend);
        void Stop();

        void Show();
        void Hide();
        void AppendMessage(const ChatMessage& msg);
        void Clear();

    private:
        static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        void UiThreadMain();
        bool CreateUi();
        void DestroyUi();
        void AppendMessageOnUiThread(const ChatMessage& msg);
        void ShowOnUiThread();
        void HideOnUiThread();
        void ClearOnUiThread();
        void HandleSendClicked();
        void LayoutChildren();
        void ClampToCurrentWorkArea(bool preferBottomRight);

        struct PendingUiMessage {
            enum class Type {
                Append,
                Show,
                Hide,
                Clear,
            } type{ Type::Append };
            ChatMessage chat;
        };

        void PostUiMessage(const PendingUiMessage& item);
        bool PopUiMessage(PendingUiMessage& item);

    private:
        std::string sessionId_;
        SendCallback onSend_;

        std::thread uiThread_;
        std::atomic<bool> running_{ false };
        bool dismissedByUser_ = false;

        DWORD uiThreadId_{ 0 };
        HWND hwnd_{ nullptr };
        HWND listBox_{ nullptr };
        HWND editBox_{ nullptr };
        HWND sendButton_{ nullptr };
        HWND closeButton_{ nullptr };
        HFONT font_{ nullptr };
        HFONT titleFont_{ nullptr };
        HBRUSH backgroundBrush_{ nullptr };
        HBRUSH controlBrush_{ nullptr };

        HANDLE readyEvent_{ nullptr };

        std::mutex queueMu_;
        std::vector<PendingUiMessage> queue_;

        static constexpr UINT WM_HI5_CHAT_QUEUE = WM_APP + 101;
    };

} // namespace hi5