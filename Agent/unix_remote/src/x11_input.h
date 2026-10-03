#pragma once

#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

struct _XDisplay;
using Display = _XDisplay;

class X11InputInjector {
public:
    X11InputInjector();
    ~X11InputInjector();

    void setTargetDisplayRect(int x, int y, int w, int h);
    bool handleMessage(const nlohmann::json& msg);
    bool moveNormalized(double xNorm, double yNorm);

private:
    bool mouseButton(int browserButton, bool down);
    bool wheel(int deltaX, int deltaY);
    bool key(const std::string& code, bool down);
    unsigned long keysymForDomCode(const std::string& code) const;

    Display* m_display = nullptr;
    std::mutex m_mu;
    int m_x = 0;
    int m_y = 0;
    int m_w = 0;
    int m_h = 0;
};
