#include "signaling_client.h"

#include <rtc/rtc.hpp>

SignalingClient::SignalingClient(std::string url)
    : m_url(std::move(url)) {
}

void SignalingClient::connect() {
    m_ws = std::make_shared<rtc::WebSocket>();

    m_ws->onOpen([this]() {
        if (m_openHandler) m_openHandler();
        });

    m_ws->onMessage([this](rtc::message_variant data) {
        if (const auto* s = std::get_if<std::string>(&data)) {
            if (m_messageHandler) m_messageHandler(*s);
        }
        });

    m_ws->onClosed([this]() {
        if (m_closedHandler) m_closedHandler();
        });

    m_ws->open(m_url);
}

void SignalingClient::send(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_sendMu);
    if (m_ws) {
        m_ws->send(text);
    }
}

void SignalingClient::onOpen(OpenHandler cb) {
    m_openHandler = std::move(cb);
}

void SignalingClient::onMessage(MessageHandler cb) {
    m_messageHandler = std::move(cb);
}

void SignalingClient::onClosed(ClosedHandler cb) {
    m_closedHandler = std::move(cb);
}