#pragma once

#include "../session/session_bridge.h"

#include <functional>
#include <mutex>
#include <vector>

namespace hi5 {

class ChatController {
public:
    using ChatUpdatedCallback = std::function<void(const std::vector<ChatMessage>&)>;

    explicit ChatController(SessionBridge& bridge);

    void SetUpdatedCallback(ChatUpdatedCallback cb);

    void SendTechnicianMessage(const std::string& body, const std::string& displayName = "Technician");
    std::vector<ChatMessage> GetMessages() const;

    // Hook this to SessionBridge event callbacks when you wire things up.
    void NotifyMessagesUpdated();

private:
    SessionBridge& bridge_;
    mutable std::mutex mu_;
    ChatUpdatedCallback callback_;
};

} // namespace hi5
