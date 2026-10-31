#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rtc {
    class WebSocket;
}

class SignalingClient {
public:
    using MessageHandler = std::function<void(const std::string&)>;
    using OpenHandler = std::function<void()>;
    using ClosedHandler = std::function<void()>;

    explicit SignalingClient(std::string url);

    void connect();
    void send(const std::string& text);

    void onOpen(OpenHandler cb);
    void onMessage(MessageHandler cb);
    void onClosed(ClosedHandler cb);

private:
    std::string m_url;
    std::shared_ptr<rtc::WebSocket> m_ws;
    // Closed or timed-out generations must not deliver stale SDP/events. Keep
    // a bounded set of retired sockets alive during async callback shutdown.
    std::vector<std::shared_ptr<rtc::WebSocket>> m_retired;
    std::atomic<std::uint64_t> m_generation{0};

    OpenHandler m_openHandler;
    MessageHandler m_messageHandler;
    ClosedHandler m_closedHandler;

    std::mutex m_sendMu;
};  