#pragma once

#include <windows.h>
#include <string>
#include <mutex>

#include <nlohmann/json.hpp>

class InputInjector {
public:
    void handleMessage(const nlohmann::json& msg);

    void setTargetDisplayRect(int x, int y, int w, int h);

private:
    bool sendAbsoluteMoveNorm(double xNorm, double yNorm);
    bool sendMouseFlag(DWORD flags);
    bool sendWheel(int deltaX, int deltaY);
    bool sendKey(const std::string& code, bool isDown);

    bool mapDomCodeToVk(const std::string& code, WORD& vk, bool& extended);

private:
    std::mutex m_targetMu;
    int m_targetX = 0;
    int m_targetY = 0;
    int m_targetW = 0;
    int m_targetH = 0;
};  