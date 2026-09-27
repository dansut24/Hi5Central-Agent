#include "signaling_client.h"

#include <rtc/rtc.hpp>

SignalingClient::SignalingClient(std::string url)
    : m_url(std::move(url)) {
}

void SignalingClient::connect() {
    auto ws = std::make_shared<rtc::WebSocket>();
    std::weak_ptr<rtc::WebSocket> weak = ws;

    auto isCurrent = [this, weak]() {
        auto candidate = weak.lock();
        if (!candidate) return false;
        std::lock_guard<std::mutex> lock(m_wsMu);
        return m_ws == candidate;
    };

    ws->onOpen([this, isCurrent]() {
        if (!isCurrent()) return;
        if (m_openHandler) m_openHandler();
        });

    ws->onMessage([this, isCurrent](rtc::message_variant data) {
        if (!isCurrent()) return;
        if (const auto* s = std::get_if<std::string>(&data)) {
            if (m_messageHandler) m_messageHandler(*s);
        }
        });

    ws->onClosed([this, weak, isCurrent]() {
        if (!isCurrent()) return;
        {
            std::lock_guard<std::mutex> lock(m_wsMu);
            auto candidate = weak.lock();
            if (candidate && m_ws == candidate) m_ws.reset();
        }
        if (m_closedHandler) m_closedHandler();
        });

    {
        std::lock_guard<std::mutex> lock(m_wsMu);
        m_ws = ws;
    }
    ws->open(m_url);
}

void SignalingClient::send(const std::string& text) {
    std::shared_ptr<rtc::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(m_wsMu);
        ws = m_ws;
    }
    std::lock_guard<std::mutex> lock(m_sendMu);
    if (ws) {
        ws->send(text);
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