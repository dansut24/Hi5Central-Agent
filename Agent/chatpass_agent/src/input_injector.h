#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <mutex>

#include <nlohmann/json.hpp>

class InputInjector {
public:
    void handleMessage(const nlohmann::json& msg);

    void setTargetDisplayRect(int x, int y, int w, int h);
    void setFollowInputDesktop(bool enabled) {
        m_followInputDesktop.store(enabled, std::memory_order_release);
    }

private:
    bool sendAbsoluteMoveNorm(double xNorm, double yNorm);
    bool sendMouseFlag(DWORD flags);
    bool sendMouseClick(int button, int clickCount = 1);
    bool sendWheel(int deltaX, int deltaY);
    bool sendKey(const std::string& code, bool isDown);
    bool sendUnicodeText(const std::string& text);
    bool sendShortcut(const std::string& action);

    bool mapDomCodeToVk(const std::string& code, WORD& vk, bool& extended);

private:
    std::mutex m_targetMu;
    int m_targetX = 0;
    int m_targetY = 0;
    int m_targetW = 0;
    int m_targetH = 0;
    std::atomic<bool> m_followInputDesktop{ false };
};
