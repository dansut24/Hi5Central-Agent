#include "signaling_client.h"

#include <rtc/rtc.hpp>

SignalingClient::SignalingClient(std::string url)
    : m_url(std::move(url)) {
}

void SignalingClient::connect() {
    auto next = std::make_shared<rtc::WebSocket>();
    const auto generation = m_generation.fetch_add(1) + 1;
    next->onOpen([this, generation]() {
        if (m_generation.load() == generation && m_openHandler) m_openHandler();
        });
    next->onMessage([this, generation](rtc::message_variant data) {
        if (m_generation.load() != generation) return;
        if (const auto* s = std::get_if<std::string>(&data)) {
            if (m_messageHandler) m_messageHandler(*s);
        }
        });
    next->onClosed([this, generation]() {
        if (m_generation.load() == generation && m_closedHandler)
            m_closedHandler();
        });

    std::shared_ptr<rtc::WebSocket> previous;
    {
        std::lock_guard<std::mutex> lock(m_sendMu);
        previous = std::move(m_ws);
        m_ws = next;
        if (previous) m_retired.push_back(previous);
        // A socket generation is retired only after it cannot be current.
        if (m_retired.size() > 16) m_retired.erase(m_retired.begin());
    }
    if (previous) {
        try { previous->close(); } catch (...) {}
    }
    next->open(m_url);
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