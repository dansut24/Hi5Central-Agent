from pathlib import Path

cpp = Path('Agent/chatpass_agent/src/ui/native_chat_window.cpp')
hdr = Path('Agent/chatpass_agent/src/ui/native_chat_window.h')
s = cpp.read_text(encoding='utf-8')
h = hdr.read_text(encoding='utf-8')

def rep(old, new):
    global s
    if old not in s:
        raise RuntimeError('native chat theme block missing: ' + old[:80])
    s = s.replace(old, new, 1)

rep('''        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
''', '''        if (font_) { DeleteObject(font_); font_ = nullptr; }
        if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
        if (backgroundBrush_) { DeleteObject(backgroundBrush_); backgroundBrush_ = nullptr; }
        if (controlBrush_) { DeleteObject(controlBrush_); controlBrush_ = nullptr; }
''')

rep('''        const UINT dpi = GetDpiForWindow(hwnd_);
        font_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI Variable");
        const int cornerPreference = 2;
        DwmSetWindowAttribute(hwnd_, 33, &cornerPreference, sizeof(cornerPreference));
''', '''        const UINT dpi = GetDpiForWindow(hwnd_);
        const int bodyFontPx = -MulDiv(10, static_cast<int>(dpi), 72);
        const int titleFontPx = -MulDiv(12, static_cast<int>(dpi), 72);
        font_ = CreateFontW(bodyFontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI Variable");
        titleFont_ = CreateFontW(titleFontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH, L"Segoe UI Variable");
        backgroundBrush_ = CreateSolidBrush(RGB(247, 249, 252));
        controlBrush_ = CreateSolidBrush(RGB(255, 255, 255));
        const int cornerPreference = 2;
        DwmSetWindowAttribute(hwnd_, 33, &cornerPreference, sizeof(cornerPreference));
''')

rep('''            WS_EX_CLIENTEDGE,
            L"LISTBOX",
''', '''            0,
            L"LISTBOX",
''')
rep('''            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
''', '''            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
''')
rep('            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,', '            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,')

rep('''        const int composerHeight = px(40);
        const int buttonWidth = px(82);
        const int listHeight = std::max(80, height - (margin * 3) - composerHeight);
        if (listBox_) MoveWindow(listBox_, margin, margin, std::max(80, width - margin * 2), listHeight, TRUE);
        if (editBox_) MoveWindow(editBox_, margin, margin * 2 + listHeight,
            std::max(80, width - margin * 3 - buttonWidth), composerHeight, TRUE);
        if (sendButton_) MoveWindow(sendButton_, std::max(margin, width - margin - buttonWidth),
            margin * 2 + listHeight, buttonWidth, composerHeight, TRUE);
''', '''        const int headerHeight = px(72);
        const int composerHeight = px(40);
        const int buttonWidth = px(82);
        const int listTop = headerHeight;
        const int listHeight = std::max(px(92), height - headerHeight - composerHeight - margin * 3);
        if (listBox_) MoveWindow(listBox_, margin, listTop, std::max(px(120), width - margin * 2), listHeight, TRUE);
        const int composerY = listTop + listHeight + margin;
        if (editBox_) MoveWindow(editBox_, margin, composerY,
            std::max(px(100), width - margin * 3 - buttonWidth), composerHeight, TRUE);
        if (sendButton_) MoveWindow(sendButton_, std::max(margin, width - margin - buttonWidth),
            composerY, buttonWidth, composerHeight, TRUE);
''')

paint = '''        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{}; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, backgroundBrush_ ? backgroundBrush_ : GetSysColorBrush(COLOR_WINDOW));
            const float scale = std::max(1.0f, static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
            const auto px = [scale](int dip) { return std::max(1, static_cast<int>(dip * scale)); };
            RECT badge{ px(12), px(14), px(48), px(50) };
            HBRUSH accent = CreateSolidBrush(RGB(37, 99, 235));
            FillRect(dc, &badge, accent); DeleteObject(accent);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255,255,255));
            HFONT old = reinterpret_cast<HFONT>(SelectObject(dc, titleFont_ ? titleFont_ : font_));
            DrawTextW(dc, L"H5", -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT title{ px(60), px(12), rc.right - px(12), px(39) };
            SetTextColor(dc, RGB(17,24,39));
            DrawTextW(dc, L"Hi5Central Remote Support", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (font_) SelectObject(dc, font_);
            HBRUSH green = CreateSolidBrush(RGB(34,197,94));
            HGDIOBJ oldBrush = SelectObject(dc, green);
            Ellipse(dc, px(61), px(45), px(69), px(53));
            SelectObject(dc, oldBrush); DeleteObject(green);
            RECT status{ px(74), px(39), rc.right - px(12), px(61) };
            SetTextColor(dc, RGB(100,116,139));
            DrawTextW(dc, L"Connected - Support chat", -1, &status, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(219,227,236));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, px(12), px(68), nullptr); LineTo(dc, rc.right - px(12), px(68));
            SelectObject(dc, oldPen); DeleteObject(pen);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(17,24,39));
            SetBkColor(dc, RGB(255,255,255));
            return reinterpret_cast<LRESULT>(controlBrush_ ? controlBrush_ : GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlID == kSendId) {
                HBRUSH brush = CreateSolidBrush(RGB(37,99,235));
                FillRect(dis->hDC, &dis->rcItem, brush); DeleteObject(brush);
                SetBkMode(dis->hDC, TRANSPARENT); SetTextColor(dis->hDC, RGB(255,255,255));
                HFONT old = reinterpret_cast<HFONT>(SelectObject(dis->hDC, font_));
                RECT r = dis->rcItem; DrawTextW(dis->hDC, L"Send", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, old);
                return TRUE;
            }
            break;
        }

'''
rep('''        switch (msg) {
        case WM_COMMAND:
''', '        switch (msg) {\n' + paint + '        case WM_COMMAND:\n')

rep('''                mmi->ptMinTrackSize.x = 320;
                mmi->ptMinTrackSize.y = 260;
''', '''                RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                mmi->ptMinTrackSize.x = std::min<LONG>(300, std::max<LONG>(220, work.right - work.left));
                mmi->ptMinTrackSize.y = std::min<LONG>(300, std::max<LONG>(220, work.bottom - work.top));
''')

if 'HFONT titleFont_' not in h:
    h = h.replace('''        HFONT font_{ nullptr };

        HANDLE readyEvent_{ nullptr };
''', '''        HFONT font_{ nullptr };
        HFONT titleFont_{ nullptr };
        HBRUSH backgroundBrush_{ nullptr };
        HBRUSH controlBrush_{ nullptr };

        HANDLE readyEvent_{ nullptr };
''', 1)

cpp.write_text(s, encoding='utf-8')
hdr.write_text(h, encoding='utf-8')
print('Native endpoint Chat theme applied')
