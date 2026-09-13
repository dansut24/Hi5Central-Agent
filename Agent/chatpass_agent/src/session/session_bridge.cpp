#include "session_bridge.h"

#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace hi5 {

namespace {
    static std::int64_t NowUnixMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
}

SessionBridge::SessionBridge() = default;

void SessionBridge::SetEventCallback(EventCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    eventCallback_ = std::move(cb);
}

void SessionBridge::SetActiveSession(const std::string& sessionId) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        activeSessionId_ = sessionId;
        bannerState_.sessionId = sessionId;
    }
    Emit(SessionEventType::ViewerConnected);
}

void SessionBridge::ClearActiveSession() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        activeSessionId_.clear();
        bannerState_ = {};
        currentRemotePath_ = "/";
        currentRemoteEntries_.clear();
    }
    Emit(SessionEventType::ViewerDisconnected);
}

std::string SessionBridge::GetActiveSession() const {
    std::lock_guard<std::mutex> lock(mu_);
    return activeSessionId_;
}

void SessionBridge::RequestChatOpen() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        json payload = {
            {"type", "chat_open"},
            {"session_id", activeSessionId_}
        };
        outgoingJson_.push_back(payload.dump());
    }
    Emit(SessionEventType::ChatOpenRequested);
}

void SessionBridge::RequestChatClose() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        json payload = {
            {"type", "chat_close"},
            {"session_id", activeSessionId_}
        };
        outgoingJson_.push_back(payload.dump());
    }
    Emit(SessionEventType::ChatCloseRequested);
}

void SessionBridge::QueueOutgoingChatMessage(const std::string& body,
                                             const std::string& sender,
                                             const std::string& displayName) {
    ChatMessage msg;
    {
        std::lock_guard<std::mutex> lock(mu_);
        msg.sessionId = activeSessionId_;
        msg.sender = sender;
        msg.displayName = displayName;
        msg.body = body;
        msg.unixMs = NowUnixMs();
        chatHistory_.push_back(msg);

        json payload = {
            {"type", "chat_message"},
            {"session_id", activeSessionId_},
            {"sender", sender},
            {"display_name", displayName},
            {"body", body},
            {"unix_ms", msg.unixMs}
        };
        outgoingJson_.push_back(payload.dump());
    }
    Emit(SessionEventType::ChatMessageSent);
}

std::vector<ChatMessage> SessionBridge::GetChatHistory() const {
    std::lock_guard<std::mutex> lock(mu_);
    return chatHistory_;
}

void SessionBridge::AddIncomingChatMessage(const ChatMessage& msg) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        chatHistory_.push_back(msg);
    }
    Emit(SessionEventType::ChatMessageReceived);
}

void SessionBridge::SetAgentBannerState(const AgentBannerState& state) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        bannerState_ = state;
        json payload = {
            {"type", "agent_presence"},
            {"session_id", bannerState_.sessionId},
            {"technician_name", bannerState_.technicianName},
            {"connected", bannerState_.connected},
            {"chat_available", bannerState_.chatAvailable}
        };
        outgoingJson_.push_back(payload.dump());
    }
    Emit(state.connected ? SessionEventType::AgentBannerRequested
                         : SessionEventType::AgentBannerDismissed);
}

AgentBannerState SessionBridge::GetAgentBannerState() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bannerState_;
}

void SessionBridge::RequestRemoteFileList(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        json payload = {
            {"type", "remote_file_list_request"},
            {"session_id", activeSessionId_},
            {"path", path.empty() ? "/" : path}
        };
        outgoingJson_.push_back(payload.dump());
    }
    Emit(SessionEventType::FileListRequested);
}

void SessionBridge::SetRemoteFileList(const std::string& path, const std::vector<FileBrowserEntry>& entries) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        currentRemotePath_ = path.empty() ? "/" : path;
        currentRemoteEntries_ = entries;
    }
    Emit(SessionEventType::FileListReceived);
}

std::string SessionBridge::GetCurrentRemotePath() const {
    std::lock_guard<std::mutex> lock(mu_);
    return currentRemotePath_;
}

std::vector<FileBrowserEntry> SessionBridge::GetCurrentRemoteEntries() const {
    std::lock_guard<std::mutex> lock(mu_);
    return currentRemoteEntries_;
}

void SessionBridge::HandleIncomingJson(const std::string& raw) {
    json msg = json::parse(raw, nullptr, false);
    if (msg.is_discarded()) {
        return;
    }

    const std::string type = msg.value("type", "");
    if (type == "chat_open") {
        Emit(SessionEventType::ChatOpenRequested);
        return;
    }

    if (type == "chat_close") {
        Emit(SessionEventType::ChatCloseRequested);
        return;
    }

    if (type == "chat_message") {
        ChatMessage cm;
        cm.sessionId = msg.value("session_id", "");
        cm.sender = msg.value("sender", "user");
        cm.displayName = msg.value("display_name", "Remote user");
        cm.body = msg.value("body", "");
        cm.unixMs = msg.value("unix_ms", NowUnixMs());
        AddIncomingChatMessage(cm);
        return;
    }

    if (type == "agent_presence") {
        AgentBannerState s;
        s.sessionId = msg.value("session_id", "");
        s.technicianName = msg.value("technician_name", "");
        s.connected = msg.value("connected", false);
        s.chatAvailable = msg.value("chat_available", false);
        SetAgentBannerState(s);
        return;
    }

    if (type == "remote_file_list") {
        std::vector<FileBrowserEntry> entries;
        if (msg.contains("entries") && msg["entries"].is_array()) {
            for (const auto& item : msg["entries"]) {
                FileBrowserEntry e;
                e.name = item.value("name", "");
                e.path = item.value("path", "");
                e.isDir = item.value("is_dir", false);
                e.size = item.value("size", static_cast<std::uint64_t>(0));
                e.fileId = item.value("file_id", "");
                e.fileToken = item.value("file_token", "");
                entries.push_back(std::move(e));
            }
        }
        SetRemoteFileList(msg.value("path", "/"), entries);
        return;
    }

    if (type == "secure_desktop_entering") {
        Emit(SessionEventType::SecureDesktopEntering);
        return;
    }
    if (type == "secure_desktop_ready") {
        Emit(SessionEventType::SecureDesktopReady);
        return;
    }
    if (type == "secure_desktop_exited") {
        Emit(SessionEventType::SecureDesktopExited);
        return;
    }
    if (type == "desktop_handoff_entering") {
        Emit(SessionEventType::DesktopHandoffEntering);
        return;
    }
    if (type == "desktop_handoff_ready") {
        Emit(SessionEventType::DesktopHandoffReady);
        return;
    }
}

std::optional<std::string> SessionBridge::PopOutgoingJson() {
    std::lock_guard<std::mutex> lock(mu_);
    if (outgoingJson_.empty()) {
        return std::nullopt;
    }
    std::string out = std::move(outgoingJson_.front());
    outgoingJson_.pop_front();
    return out;
}

void SessionBridge::Emit(SessionEventType ev) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = eventCallback_;
    }
    if (cb) {
        cb(ev);
    }
}

} // namespace hi5
