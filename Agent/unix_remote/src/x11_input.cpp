#include "x11_input.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

using json = nlohmann::json;

X11InputInjector::X11InputInjector() {
    XInitThreads();
    m_display = XOpenDisplay(nullptr);
    if (!m_display) throw std::runtime_error("Could not open X11 display for input injection.");
}

X11InputInjector::~X11InputInjector() {
    if (m_display) XCloseDisplay(m_display);
}

void X11InputInjector::setTargetDisplayRect(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_x = x; m_y = y; m_w = w; m_h = h;
}

bool X11InputInjector::moveNormalized(double xNorm, double yNorm) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_display) return false;
    xNorm = std::clamp(xNorm, 0.0, 1.0);
    yNorm = std::clamp(yNorm, 0.0, 1.0);
    const int screen = DefaultScreen(m_display);
    const int fallbackW = DisplayWidth(m_display, screen);
    const int fallbackH = DisplayHeight(m_display, screen);
    const int x = m_x + static_cast<int>(std::llround(xNorm * std::max(0, (m_w > 0 ? m_w : fallbackW) - 1)));
    const int y = m_y + static_cast<int>(std::llround(yNorm * std::max(0, (m_h > 0 ? m_h : fallbackH) - 1)));
    const bool ok = XTestFakeMotionEvent(m_display, -1, x, y, CurrentTime) != 0;
    XFlush(m_display);
    return ok;
}

bool X11InputInjector::mouseButton(int browserButton, bool down) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_display) return false;
    unsigned int button = 0;
    if (browserButton == 0) button = 1;
    else if (browserButton == 1) button = 2;
    else if (browserButton == 2) button = 3;
    if (!button) return false;
    const bool ok = XTestFakeButtonEvent(m_display, button, down ? True : False, CurrentTime) != 0;
    XFlush(m_display);
    return ok;
}

bool X11InputInjector::wheel(int deltaX, int deltaY) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_display) return false;
    auto pulse = [&](unsigned int button, int amount) {
        const int steps = std::clamp(static_cast<int>(std::ceil(std::abs(amount) / 100.0)), 1, 10);
        for (int i = 0; i < steps; ++i) {
            XTestFakeButtonEvent(m_display, button, True, CurrentTime);
            XTestFakeButtonEvent(m_display, button, False, CurrentTime);
        }
    };
    if (deltaY > 0) pulse(5, deltaY);
    else if (deltaY < 0) pulse(4, deltaY);
    if (deltaX > 0) pulse(7, deltaX);
    else if (deltaX < 0) pulse(6, deltaX);
    XFlush(m_display);
    return true;
}

unsigned long X11InputInjector::keysymForDomCode(const std::string& code) const {
    if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(code[3])));
        return static_cast<unsigned long>(c);
    }
    if (code.size() == 6 && code.rfind("Digit", 0) == 0) return static_cast<unsigned long>(code[5]);

    static const std::unordered_map<std::string, KeySym> map = {
        {"Space", XK_space}, {"Enter", XK_Return}, {"NumpadEnter", XK_KP_Enter},
        {"Escape", XK_Escape}, {"Tab", XK_Tab}, {"Backspace", XK_BackSpace},
        {"Delete", XK_Delete}, {"Insert", XK_Insert}, {"Home", XK_Home}, {"End", XK_End},
        {"PageUp", XK_Page_Up}, {"PageDown", XK_Page_Down},
        {"ArrowLeft", XK_Left}, {"ArrowRight", XK_Right}, {"ArrowUp", XK_Up}, {"ArrowDown", XK_Down},
        {"ShiftLeft", XK_Shift_L}, {"ShiftRight", XK_Shift_R},
        {"ControlLeft", XK_Control_L}, {"ControlRight", XK_Control_R},
        {"AltLeft", XK_Alt_L}, {"AltRight", XK_Alt_R},
        {"MetaLeft", XK_Super_L}, {"MetaRight", XK_Super_R}, {"ContextMenu", XK_Menu},
        {"CapsLock", XK_Caps_Lock}, {"NumLock", XK_Num_Lock}, {"ScrollLock", XK_Scroll_Lock},
        {"Pause", XK_Pause}, {"PrintScreen", XK_Print},
        {"Minus", XK_minus}, {"Equal", XK_equal}, {"BracketLeft", XK_bracketleft},
        {"BracketRight", XK_bracketright}, {"Backslash", XK_backslash},
        {"Semicolon", XK_semicolon}, {"Quote", XK_apostrophe}, {"Backquote", XK_grave},
        {"Comma", XK_comma}, {"Period", XK_period}, {"Slash", XK_slash},
        {"Numpad0", XK_KP_0}, {"Numpad1", XK_KP_1}, {"Numpad2", XK_KP_2},
        {"Numpad3", XK_KP_3}, {"Numpad4", XK_KP_4}, {"Numpad5", XK_KP_5},
        {"Numpad6", XK_KP_6}, {"Numpad7", XK_KP_7}, {"Numpad8", XK_KP_8},
        {"Numpad9", XK_KP_9}, {"NumpadAdd", XK_KP_Add}, {"NumpadSubtract", XK_KP_Subtract},
        {"NumpadMultiply", XK_KP_Multiply}, {"NumpadDivide", XK_KP_Divide},
        {"NumpadDecimal", XK_KP_Decimal}
    };
    if (auto it = map.find(code); it != map.end()) return it->second;
    if (code.size() >= 2 && code[0] == 'F') {
        try {
            const int n = std::stoi(code.substr(1));
            if (n >= 1 && n <= 35) return XK_F1 + (n - 1);
        } catch (...) {}
    }
    return NoSymbol;
}

bool X11InputInjector::key(const std::string& code, bool down) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_display) return false;
    const KeySym sym = static_cast<KeySym>(keysymForDomCode(code));
    if (sym == NoSymbol) return false;
    const KeyCode kc = XKeysymToKeycode(m_display, sym);
    if (!kc) return false;
    const bool ok = XTestFakeKeyEvent(m_display, kc, down ? True : False, CurrentTime) != 0;
    XFlush(m_display);
    return ok;
}

bool X11InputInjector::handleMessage(const json& msg) {
    const std::string kind = msg.value("kind", msg.value("type", ""));
    if (kind == "mouse_move") return moveNormalized(msg.value("x_norm", 0.0), msg.value("y_norm", 0.0));
    if (kind == "mouse_down") return mouseButton(msg.value("button", 0), true);
    if (kind == "mouse_up") return mouseButton(msg.value("button", 0), false);
    if (kind == "wheel") return wheel(msg.value("delta_x", 0), msg.value("delta_y", 0));
    if (kind == "key_down") return key(msg.value("code", ""), true);
    if (kind == "key_up") return key(msg.value("code", ""), false);
    return false;
}
