#pragma once

#include "session_messages.h"

#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hi5 {

class SessionBridge {
public:
    using EventCallback = std::function<void(SessionEventType)>;

    SessionBridge();

    void SetEventCallback(EventCallback cb);

    // Session lifecycle
    void SetActiveSession(const std::string& sessionId);
    void ClearActiveSession();
    std::string GetActiveSession() const;

    // Chat
    void RequestChatOpen();
    void RequestChatClose();
    void QueueOutgoingChatMessage(const std::string& body,
                                  const std::string& sender = "tech",
                                  const std::string& displayName = "Technician");
    std::vector<ChatMessage> GetChatHistory() const;
    void AddIncomingChatMessage(const ChatMessage& msg);

    // Agent presence / banner
    void SetAgentBannerState(const AgentBannerState& state);
    AgentBannerState GetAgentBannerState() const;

    // File browser
    void RequestRemoteFileList(const std::string& path);
    void SetRemoteFileList(const std::string& path, const std::vector<FileBrowserEntry>& entries);
    std::string GetCurrentRemotePath() const;
    std::vector<FileBrowserEntry> GetCurrentRemoteEntries() const;

    // JSON bridge helpers
    void HandleIncomingJson(const std::string& json);
    std::optional<std::string> PopOutgoingJson();

private:
    void Emit(SessionEventType ev);

    mutable std::mutex mu_;
    EventCallback eventCallback_;

    std::string activeSessionId_;
    std::deque<std::string> outgoingJson_;

    std::vector<ChatMessage> chatHistory_;

    AgentBannerState bannerState_{};

    std::string currentRemotePath_ = "/";
    std::vector<FileBrowserEntry> currentRemoteEntries_;
};

} // namespace hi5
