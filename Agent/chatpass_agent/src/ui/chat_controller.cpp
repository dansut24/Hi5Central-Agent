#include "chat_controller.h"

namespace hi5 {

ChatController::ChatController(SessionBridge& bridge)
    : bridge_(bridge) {}

void ChatController::SetUpdatedCallback(ChatUpdatedCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    callback_ = std::move(cb);
}

void ChatController::SendTechnicianMessage(const std::string& body, const std::string& displayName) {
    bridge_.QueueOutgoingChatMessage(body, "tech", displayName);
    NotifyMessagesUpdated();
}

std::vector<ChatMessage> ChatController::GetMessages() const {
    return bridge_.GetChatHistory();
}

void ChatController::NotifyMessagesUpdated() {
    ChatUpdatedCallback cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = callback_;
    }
    if (cb) {
        cb(bridge_.GetChatHistory());
    }
}

} // namespace hi5
