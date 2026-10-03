#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace hi5 {

class RemoteWebRtcSession;

class RemoteDesktopManager {
public:
    using SendFn = std::function<bool(const nlohmann::json&)>;

    explicit RemoteDesktopManager(SendFn send);
    ~RemoteDesktopManager();

    bool handleMessage(const nlohmann::json& message);
    void stopAll();

private:
    SendFn send_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<RemoteWebRtcSession>> sessions_;
};

} // namespace hi5