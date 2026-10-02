#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace ix {
class WebSocket;
}

namespace hi5 {

class AgentWebSocket {
public:
    using MessageHandler = std::function<void(const std::string&)>;
    using StateHandler = std::function<void(bool connected, const std::string& detail)>;

    AgentWebSocket();
    ~AgentWebSocket();

    AgentWebSocket(const AgentWebSocket&) = delete;
    AgentWebSocket& operator=(const AgentWebSocket&) = delete;

    void start(const std::string& url, MessageHandler onMessage, StateHandler onState);
    void stop();
    bool sendText(const std::string& text);
    bool connected() const { return connected_.load(); }

private:
    std::unique_ptr<ix::WebSocket> socket_;
    std::atomic<bool> connected_{false};
};

} // namespace hi5
