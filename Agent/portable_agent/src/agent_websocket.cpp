#include "agent_websocket.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <mutex>

namespace hi5 {
namespace {
std::once_flag g_ixInit;
}

AgentWebSocket::AgentWebSocket() {
    std::call_once(g_ixInit, []() {
        ix::initNetSystem();
    });
}

AgentWebSocket::~AgentWebSocket() {
    stop();
}

void AgentWebSocket::start(
    const std::string& url,
    MessageHandler onMessage,
    StateHandler onState) {

    stop();

    socket_ = std::make_unique<ix::WebSocket>();
    socket_->setUrl(url);
    socket_->setPingInterval(30);
    socket_->setHandshakeTimeout(10);
    socket_->setMinWaitBetweenReconnectionRetries(1000);
    socket_->setMaxWaitBetweenReconnectionRetries(10000);
    socket_->enableAutomaticReconnection();
    socket_->disablePerMessageDeflate();

    socket_->setOnMessageCallback(
        [this, onMessage = std::move(onMessage), onState = std::move(onState)]
        (const ix::WebSocketMessagePtr& message) {
            if (!message) return;

            switch (message->type) {
            case ix::WebSocketMessageType::Open:
                connected_.store(true);
                if (onState) onState(true, "connected");
                break;
            case ix::WebSocketMessageType::Message:
                if (onMessage) onMessage(message->str);
                break;
            case ix::WebSocketMessageType::Close:
                connected_.store(false);
                if (onState) {
                    onState(
                        false,
                        "closed code=" + std::to_string(message->closeInfo.code) +
                        " reason=" + message->closeInfo.reason);
                }
                break;
            case ix::WebSocketMessageType::Error:
                connected_.store(false);
                if (onState) onState(false, "error: " + message->errorInfo.reason);
                break;
            default:
                break;
            }
        });

    socket_->start();
}

void AgentWebSocket::stop() {
    connected_.store(false);
    if (socket_) {
        socket_->stop();
        socket_.reset();
    }
}

bool AgentWebSocket::sendText(const std::string& text) {
    if (!socket_ || !connected_.load()) return false;
    const auto result = socket_->sendText(text);
    return result.success;
}

} // namespace hi5
