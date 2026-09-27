#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hi5 {

struct ConnectChatMessage {
    std::string sender;
    std::string displayName;
    std::string body;
};

class NativeConnectWindow {
public:
    using SendCallback = std::function<void(const std::string&)>;
    using EndCallback = std::function<void()>;

    NativeConnectWindow();
    ~NativeConnectWindow();
    bool Start(const std::string& sessionId,
        const std::string& technicianName,
        const std::string& organisationName,
        SendCallback onSend,
        EndCallback onEnd);

    void Stop();
    void SetIdentity(const std::string& technicianName,
        const std::string& organisationName);
    void SetConnectionState(const std::string& statusText,
        bool remoteControlActive);
    void AppendMessage(const ConnectChatMessage& message);
    void Restore();

private:
    enum class UiActionType {
        Identity,
        ConnectionState,
        ChatMessage,
        Restore,
        Stop,
    };
    struct UiAction {
        UiActionType type{ UiActionType::Restore };
        std::string first;
        std::string second;
        bool flag{ false };
        ConnectChatMessage chat;
    };

    static LRESULT CALLBACK StaticWndProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void UiThreadMain();
    bool CreateUi();
    void DestroyUi();
    void LayoutChildren();
    void CenterWindow();
    void ApplyIdentity(const std::string& technicianName,
        const std::string& organisationName);
    void ApplyConnectionState(const std::string& statusText,
        bool remoteControlActive);
    void AppendMessageOnUiThread(const ConnectChatMessage& message);
    void HandleSend();
    void HandleEndSession();
    void UpdateDurationText();
    void PostAction(UiAction action);
    bool PopAction(UiAction& action);
    void DrainActions();

private:
    std::string sessionId_;
    std::string technicianName_;
    std::string organisationName_;
    SendCallback onSend_;
    EndCallback onEnd_;

    std::thread uiThread_;
    std::atomic<bool> running_{ false };
    DWORD uiThreadId_{ 0 };
    std::atomic<HWND> hwnd_{ nullptr };
    HANDLE readyEvent_{ nullptr };

    HWND headerLabel_{ nullptr };
    HWND statusLabel_{ nullptr };
    HWND technicianLabel_{ nullptr };
    HWND organisationLabel_{ nullptr };
    HWND durationLabel_{ nullptr };
    HWND trustLabel_{ nullptr };
    HWND chatLog_{ nullptr };
    HWND chatInput_{ nullptr };
    HWND sendButton_{ nullptr };
    HWND endButton_{ nullptr };

    HFONT titleFont_{ nullptr };
    HFONT bodyFont_{ nullptr };
    HFONT smallFont_{ nullptr };

    std::mutex queueMu_;
    std::vector<UiAction> queue_;
    bool remoteControlActive_{ false };
    bool remoteControlStarted_{ false };
    bool ending_{ false };
    std::chrono::steady_clock::time_point remoteControlStartedAt_{};

    static constexpr UINT WM_HI5_CONNECT_QUEUE = WM_APP + 211;
    static constexpr UINT_PTR TIMER_DURATION = 211;
};

} // namespace hi5
