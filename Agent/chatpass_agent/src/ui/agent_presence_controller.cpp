#include "agent_presence_controller.h"

namespace hi5 {

AgentPresenceController::AgentPresenceController(SessionBridge& bridge)
    : bridge_(bridge) {}

void AgentPresenceController::SetVisibilityCallback(VisibilityCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    callback_ = std::move(cb);
}

void AgentPresenceController::ShowConnected(const std::string& technicianName, bool chatAvailable) {
    AgentBannerState state = bridge_.GetAgentBannerState();
    state.technicianName = technicianName;
    state.connected = true;
    state.chatAvailable = chatAvailable;

    bridge_.SetAgentBannerState(state);

    VisibilityCallback cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = callback_;
    }
    if (cb) {
        cb(state);
    }
}

void AgentPresenceController::Hide() {
    AgentBannerState state = bridge_.GetAgentBannerState();
    state.connected = false;
    state.chatAvailable = false;
    bridge_.SetAgentBannerState(state);

    VisibilityCallback cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = callback_;
    }
    if (cb) {
        cb(state);
    }
}

} // namespace hi5
