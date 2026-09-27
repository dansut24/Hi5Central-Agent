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
    struct DisplayMessage {
        std::string sender;
        std::string displayName;
        std::string body;
        std::wstring timeText;
        bool outgoing{ false };
    };

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
    static LRESULT CALLBACK StaticInputProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void UiThreadMain();
    bool CreateUi();
    void DestroyUi();
    void CreateFonts();
    void LayoutChildren();
    void CenterWindow();

    void Paint(HDC hdc, const RECT& client);
    void PaintHeader(HDC hdc, const RECT& client);
    void PaintSessionCard(HDC hdc, const RECT& client);
    void PaintTrustCard(HDC hdc, const RECT& client);
    void PaintChat(HDC hdc, const RECT& client);
    void PaintComposer(HDC hdc, const RECT& client);
    void PaintFooter(HDC hdc, const RECT& client);

    void ApplyIdentity(const std::string& technicianName,
        const std::string& organisationName);
    void ApplyConnectionState(const std::string& statusText,
        bool remoteControlActive);
    void AppendMessageOnUiThread(const ConnectChatMessage& message);
    void HandleSend();
    void HandleEndSession();
    void PostAction(UiAction action);
    bool PopAction(UiAction& action);
    void DrainActions();

    int S(int logical) const;
    std::wstring SessionTimeText() const;
    std::wstring SessionStateText() const;
    void UpdateHoverState(POINT point);
    void UpdateChatScroll(int delta);
    void FlashTaskbar();

private:
    std::string sessionId_;
    std::string technicianName_;
    std::string organisationName_;
    std::string statusText_{ "Waiting for technician" };
    SendCallback onSend_;
    EndCallback onEnd_;

    std::thread uiThread_;
    std::atomic<bool> running_{ false };
    DWORD uiThreadId_{ 0 };
    std::atomic<HWND> hwnd_{ nullptr };
    HANDLE readyEvent_{ nullptr };

    HWND chatInput_{ nullptr };
    WNDPROC inputOldProc_{ nullptr };

    HFONT titleFont_{ nullptr };
    HFONT subtitleFont_{ nullptr };
    HFONT headingFont_{ nullptr };
    HFONT bodyFont_{ nullptr };
    HFONT smallFont_{ nullptr };
    HFONT tinyFont_{ nullptr };
    HBRUSH inputBrush_{ nullptr };
    HICON appIcon_{ nullptr };
    HICON appIconSmall_{ nullptr };

    std::mutex queueMu_;
    std::vector<UiAction> queue_;
    std::vector<DisplayMessage> messages_;

    float dpiScale_{ 1.0f };
    bool remoteControlActive_{ false };
    bool remoteControlStarted_{ false };
    bool ending_{ false };
    bool hoverSend_{ false };
    bool hoverEnd_{ false };
    bool trackingMouse_{ false };
    bool autoScrollChat_{ true };
    std::chrono::steady_clock::time_point remoteControlStartedAt_{};

    RECT chatRect_{};
    RECT composerRect_{};
    RECT sendRect_{};
    RECT endRect_{};
    int chatScrollOffset_{ 0 };
    int chatContentHeight_{ 0 };
    int chatViewportHeight_{ 0 };

    static constexpr UINT WM_HI5_CONNECT_QUEUE = WM_APP + 211;
    static constexpr UINT_PTR TIMER_DURATION = 211;
};

} // namespace hi5
