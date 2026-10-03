#include "remote_webrtc.h"

namespace hi5 {

class RemoteWebRtcSession {};

RemoteDesktopManager::RemoteDesktopManager(SendFn send) : send_(std::move(send)) {}
RemoteDesktopManager::~RemoteDesktopManager() = default;

bool RemoteDesktopManager::handleMessage(const nlohmann::json& message) {
    const std::string type = message.value("type", "");
    if (type != "start_webrtc") return false;
    const std::string sessionId = message.value("session_id", message.value("sessionId", ""));
    send_({
        {"type", "remote_error"},
        {"session_id", sessionId},
        {"code", "platform_remote_pending"},
        {"message", "This platform remote desktop provider is not available in this build."}
    });
    return true;
}

void RemoteDesktopManager::stopAll() {}

} // namespace hi5