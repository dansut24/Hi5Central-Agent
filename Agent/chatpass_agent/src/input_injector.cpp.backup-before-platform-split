#include "input_injector.h"

#include <algorithm>
#include <cmath>

using json = nlohmann::json;

void InputInjector::setTargetDisplayRect(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(m_targetMu);
    m_targetX = x;
    m_targetY = y;
    m_targetW = w;
    m_targetH = h;
}

void InputInjector::handleMessage(const json& msg) {
    const std::string kind = msg.value("kind", "");

    if (kind == "mouse_move") {
        const double xNorm = msg.value("x_norm", 0.0);
        const double yNorm = msg.value("y_norm", 0.0);
        sendAbsoluteMoveNorm(xNorm, yNorm);
        return;
    }

    if (kind == "mouse_down") {
        const int button = msg.value("button", 0);
        switch (button) {
        case 0: sendMouseFlag(MOUSEEVENTF_LEFTDOWN); break;
        case 1: sendMouseFlag(MOUSEEVENTF_MIDDLEDOWN); break;
        case 2: sendMouseFlag(MOUSEEVENTF_RIGHTDOWN); break;
        default: break;
        }
        return;
    }

    if (kind == "mouse_up") {
        const int button = msg.value("button", 0);
        switch (button) {
        case 0: sendMouseFlag(MOUSEEVENTF_LEFTUP); break;
        case 1: sendMouseFlag(MOUSEEVENTF_MIDDLEUP); break;
        case 2: sendMouseFlag(MOUSEEVENTF_RIGHTUP); break;
        default: break;
        }
        return;
    }

    if (kind == "wheel") {
        const int dx = msg.value("delta_x", 0);
        const int dy = msg.value("delta_y", 0);
        sendWheel(dx, dy);
        return;
    }

    if (kind == "key_down") {
        sendKey(msg.value("code", ""), true);
        return;
    }

    if (kind == "key_up") {
        sendKey(msg.value("code", ""), false);
        return;
    }
}

bool InputInjector::sendAbsoluteMoveNorm(double xNorm, double yNorm) {
    xNorm = std::max(0.0, std::min(1.0, xNorm));
    yNorm = std::max(0.0, std::min(1.0, yNorm));

    int targetX, targetY, targetW, targetH;
    {
        std::lock_guard<std::mutex> lock(m_targetMu);
        targetX = m_targetX;
        targetY = m_targetY;
        targetW = m_targetW;
        targetH = m_targetH;
    }

    if (targetW <= 0 || targetH <= 0) {
        targetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        targetY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        targetW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        targetH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vw <= 1 || vh <= 1) {
        return false;
    }

    const double px = targetX + (xNorm * (targetW - 1));
    const double py = targetY + (yNorm * (targetH - 1));

    const LONG absX = static_cast<LONG>(std::llround(((px - vx) * 65535.0) / (vw - 1)));
    const LONG absY = static_cast<LONG>(std::llround(((py - vy) * 65535.0) / (vh - 1)));

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = absX;
    in.mi.dy = absY;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool InputInjector::sendMouseFlag(DWORD flags) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool InputInjector::sendWheel(int deltaX, int deltaY) {
    bool ok = true;

    if (deltaY != 0) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(static_cast<SHORT>(-deltaY));
        ok = ok && (SendInput(1, &in, sizeof(INPUT)) == 1);
    }

    if (deltaX != 0) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = static_cast<DWORD>(static_cast<SHORT>(deltaX));
        ok = ok && (SendInput(1, &in, sizeof(INPUT)) == 1);
    }

    return ok;
}

bool InputInjector::sendKey(const std::string& code, bool isDown) {
    WORD vk = 0;
    bool extended = false;
    if (!mapDomCodeToVk(code, vk, extended)) {
        return false;
    }

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;

    if (extended) {
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool InputInjector::mapDomCodeToVk(const std::string& code, WORD& vk, bool& extended) {
    extended = false;

    if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        char c = code[3];
        if (c >= 'A' && c <= 'Z') {
            vk = static_cast<WORD>(c);
            return true;
        }
    }

    if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
        char c = code[5];
        if (c >= '0' && c <= '9') {
            vk = static_cast<WORD>(c);
            return true;
        }
    }

    if (code == "Space") { vk = VK_SPACE; return true; }
    if (code == "Enter") { vk = VK_RETURN; return true; }
    if (code == "NumpadEnter") { vk = VK_RETURN; extended = true; return true; }
    if (code == "Escape") { vk = VK_ESCAPE; return true; }
    if (code == "Tab") { vk = VK_TAB; return true; }
    if (code == "Backspace") { vk = VK_BACK; return true; }
    if (code == "Delete") { vk = VK_DELETE; extended = true; return true; }
    if (code == "Insert") { vk = VK_INSERT; extended = true; return true; }
    if (code == "Home") { vk = VK_HOME; extended = true; return true; }
    if (code == "End") { vk = VK_END; extended = true; return true; }
    if (code == "PageUp") { vk = VK_PRIOR; extended = true; return true; }
    if (code == "PageDown") { vk = VK_NEXT; extended = true; return true; }

    if (code == "ArrowLeft") { vk = VK_LEFT; extended = true; return true; }
    if (code == "ArrowRight") { vk = VK_RIGHT; extended = true; return true; }
    if (code == "ArrowUp") { vk = VK_UP; extended = true; return true; }
    if (code == "ArrowDown") { vk = VK_DOWN; extended = true; return true; }

    if (code == "ShiftLeft") { vk = VK_LSHIFT; return true; }
    if (code == "ShiftRight") { vk = VK_RSHIFT; return true; }
    if (code == "ControlLeft") { vk = VK_LCONTROL; return true; }
    if (code == "ControlRight") { vk = VK_RCONTROL; extended = true; return true; }
    if (code == "AltLeft") { vk = VK_LMENU; return true; }
    if (code == "AltRight") { vk = VK_RMENU; extended = true; return true; }
    if (code == "MetaLeft") { vk = VK_LWIN; extended = true; return true; }
    if (code == "MetaRight") { vk = VK_RWIN; extended = true; return true; }
    if (code == "ContextMenu") { vk = VK_APPS; extended = true; return true; }

    if (code == "CapsLock") { vk = VK_CAPITAL; return true; }
    if (code == "NumLock") { vk = VK_NUMLOCK; extended = true; return true; }
    if (code == "ScrollLock") { vk = VK_SCROLL; return true; }
    if (code == "Pause") { vk = VK_PAUSE; return true; }
    if (code == "PrintScreen") { vk = VK_SNAPSHOT; extended = true; return true; }

    if (code == "Minus") { vk = VK_OEM_MINUS; return true; }
    if (code == "Equal") { vk = VK_OEM_PLUS; return true; }
    if (code == "BracketLeft") { vk = VK_OEM_4; return true; }
    if (code == "BracketRight") { vk = VK_OEM_6; return true; }
    if (code == "Backslash") { vk = VK_OEM_5; return true; }
    if (code == "Semicolon") { vk = VK_OEM_1; return true; }
    if (code == "Quote") { vk = VK_OEM_7; return true; }
    if (code == "Backquote") { vk = VK_OEM_3; return true; }
    if (code == "Comma") { vk = VK_OEM_COMMA; return true; }
    if (code == "Period") { vk = VK_OEM_PERIOD; return true; }
    if (code == "Slash") { vk = VK_OEM_2; return true; }

    if (code == "Numpad0") { vk = VK_NUMPAD0; return true; }
    if (code == "Numpad1") { vk = VK_NUMPAD1; return true; }
    if (code == "Numpad2") { vk = VK_NUMPAD2; return true; }
    if (code == "Numpad3") { vk = VK_NUMPAD3; return true; }
    if (code == "Numpad4") { vk = VK_NUMPAD4; return true; }
    if (code == "Numpad5") { vk = VK_NUMPAD5; return true; }
    if (code == "Numpad6") { vk = VK_NUMPAD6; return true; }
    if (code == "Numpad7") { vk = VK_NUMPAD7; return true; }
    if (code == "Numpad8") { vk = VK_NUMPAD8; return true; }
    if (code == "Numpad9") { vk = VK_NUMPAD9; return true; }
    if (code == "NumpadAdd") { vk = VK_ADD; return true; }
    if (code == "NumpadSubtract") { vk = VK_SUBTRACT; return true; }
    if (code == "NumpadMultiply") { vk = VK_MULTIPLY; return true; }
    if (code == "NumpadDivide") { vk = VK_DIVIDE; extended = true; return true; }
    if (code == "NumpadDecimal") { vk = VK_DECIMAL; return true; }

    if (code.size() >= 2 && code[0] == 'F') {
        try {
            int fn = std::stoi(code.substr(1));
            if (fn >= 1 && fn <= 24) {
                vk = static_cast<WORD>(VK_F1 + (fn - 1));
                return true;
            }
        }
        catch (...) {
        }
    }

    return false;
}