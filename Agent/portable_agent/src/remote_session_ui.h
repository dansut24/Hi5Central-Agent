#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace hi5 {

class RemoteSessionUi {
public:
    using SendFn = std::function<bool(const nlohmann::json&)>;

    explicit RemoteSessionUi(SendFn send);
    ~RemoteSessionUi();

    RemoteSessionUi(const RemoteSessionUi&) = delete;
    RemoteSessionUi& operator=(const RemoteSessionUi&) = delete;

    void handleStart(const nlohmann::json& message);
    void handleChatMessage(const nlohmann::json& message);
    void handleChatClose();
    void handleSessionEnd();

    void run();
    void quit();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hi5
