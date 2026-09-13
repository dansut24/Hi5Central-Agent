#pragma once

#include "../session/session_bridge.h"

#include <functional>
#include <mutex>

namespace hi5 {

class AgentPresenceController {
public:
    using VisibilityCallback = std::function<void(const AgentBannerState&)>;

    explicit AgentPresenceController(SessionBridge& bridge);

    void SetVisibilityCallback(VisibilityCallback cb);

    void ShowConnected(const std::string& technicianName, bool chatAvailable);
    void Hide();

private:
    SessionBridge& bridge_;
    std::mutex mu_;
    VisibilityCallback callback_;
};

} // namespace hi5
