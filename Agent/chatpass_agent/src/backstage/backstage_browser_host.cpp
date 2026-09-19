#include "backstage_browser_host.h"
#include "util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

HWND g_hwnd = nullptr;
HWND g_backButton = nullptr;
HWND g_forwardButton = nullptr;
HWND g_homeButton = nullptr;
HWND g_refreshButton = nullptr;
HWND g_addressEdit = nullptr;
HWND g_goButton = nullptr;
HWND g_contextLabel = nullptr;
HWND g_statusLabel = nullptr;
WNDPROC g_addressOriginalProc = nullptr;

bool g_showingHome = false;
std::wstring g_initialUrl = L"about:blank";
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;

constexpr int kToolbarHeight = 52;
constexpr UINT_PTR kWebViewInitTimerId = 5201;

enum BrowserControlId {
    IDC_BROWSER_BACK = 5101,
    IDC_BROWSER_FORWARD = 5102,
    IDC_BROWSER_HOME = 5103,
    IDC_BROWSER_REFRESH = 5104,
    IDC_BROWSER_ADDRESS = 5105,
    IDC_BROWSER_GO = 5106,
    IDC_BROWSER_CONTEXT = 5107,
};

std::optional<std::string> GetArgValue(int argc, char** argv, const std::string& key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == key) return std::string(argv[i + 1]);
    }
    return std::nullopt;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    return value;
}

bool LooksLikeIpv4OrIpv4Port(const std::wstring& value) {
    int dots = 0;
    bool sawDigit = false;
    for (wchar_t ch : value) {
        if (ch >= L'0' && ch <= L'9') {
            sawDigit = true;
            continue;
        }
        if (ch == L'.') {
            ++dots;
            continue;
        }
        if (ch == L':') break;
        return false;
    }
    return sawDigit && dots == 3;
}

bool EndsWith(const std::wstring& value, const wchar_t* suffix) {
    const std::wstring s = suffix;
    return value.size() >= s.size() &&
        value.compare(value.size() - s.size(), s.size(), s) == 0;
}

std::wstring NormaliseUrl(std::wstring url) {
    url = Trim(std::move(url));
    if (url.empty()) return L"about:blank";

    std::wstring lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    if (lower.rfind(L"http://", 0) == 0 ||
        lower.rfind(L"https://", 0) == 0 ||
        lower.rfind(L"file://", 0) == 0 ||
        lower.rfind(L"about:", 0) == 0) {
        return url;
    }

    const bool hasSpace = url.find_first_of(L" \t") != std::wstring::npos;
    if (!hasSpace) {
        const bool localHost =
            lower == L"localhost" ||
            lower.rfind(L"localhost:", 0) == 0 ||
            LooksLikeIpv4OrIpv4Port(lower) ||
            EndsWith(lower, L".local") ||
            EndsWith(lower, L".lan") ||
            EndsWith(lower, L".home") ||
            EndsWith(lower, L".internal") ||
            lower.find(L'.') == std::wstring::npos;

        if (localHost) return L"http://" + url;
        return L"https://" + url;
    }

    return L"https://www.bing.com/search?q=" + url;
}

const wchar_t* Hi5HomeHtml() {
    return LR"HI5(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hi5 Web</title>
<style>
:root{font-family:"Segoe UI",Arial,sans-serif;color:#152033;background:#f7f9fc}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(180deg,#f7f9fc 0,#fff 60%)}
main{max-width:920px;margin:0 auto;padding:82px 42px 48px;text-align:center}
.brand{display:flex;align-items:center;justify-content:center;gap:14px;margin-bottom:8px}
.logo{width:54px;height:54px;border-radius:18px;display:grid;place-items:center;background:#0f86f7;color:#fff;font-size:24px;font-weight:800;box-shadow:0 12px 30px #0f86f733}
h1{font-size:42px;letter-spacing:-1.4px;margin:0}.sub{color:#65758b;margin:8px 0 32px}
.search{display:flex;gap:10px;max-width:760px;margin:0 auto}
.search input{flex:1;height:52px;border:1px solid #d6dde8;border-radius:12px;padding:0 16px;font-size:16px;outline:none;box-shadow:0 5px 18px #23364d0d}
.search input:focus{border-color:#4ca7ff;box-shadow:0 0 0 3px #0f86f71a}
.search button{width:54px;border:0;border-radius:12px;background:#138cf8;color:#fff;font-size:22px;cursor:pointer}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin-top:34px}
.card{display:block;text-decoration:none;color:#213147;border:1px solid #dce3ed;border-radius:13px;background:#fff;padding:18px 12px;min-height:88px;box-shadow:0 5px 18px #23364d0a}
.card:hover{border-color:#99c9f6;background:#f7fbff}.card b{display:block;font-size:14px;margin-bottom:5px}.card span{font-size:11px;color:#708198}
.badge{display:inline-block;margin-top:32px;padding:7px 11px;border-radius:999px;background:#eef4fb;color:#607086;font-size:11px}
@media(max-width:700px){main{padding:48px 18px}.grid{grid-template-columns:repeat(2,1fr)}h1{font-size:34px}}
</style>
</head>
<body>
<main>
  <div class="brand"><div class="logo">H5</div><h1>Hi5 Web</h1></div>
  <div class="sub">A secure, isolated browser for IT support</div>
  <div class="search">
    <input id="q" autofocus placeholder="Search or enter address (e.g. 192.168.1.1)" onkeydown="if(event.key==='Enter')go()">
    <button onclick="go()">&#8594;</button>
  </div>
  <div class="grid">
    <a class="card" href="http://192.168.1.1"><b>Router</b><span>192.168.1.1</span></a>
    <a class="card" href="https://idrac/"><b>iDRAC</b><span>https://idrac/</span></a>
    <a class="card" href="https://ilo/"><b>iLO</b><span>https://ilo/</span></a>
    <a class="card" href="http://192.168.1.254"><b>Printer</b><span>192.168.1.254</span></a>
    <a class="card" href="http://nas.local"><b>NAS</b><span>http://nas.local</span></a>
    <a class="card" href="http://192.168.0.1"><b>Switch</b><span>192.168.0.1</span></a>
    <a class="card" href="https://firewall/"><b>Firewall</b><span>https://firewall/</span></a>
    <a class="card" href="https://example.com"><b>Web test</b><span>Known public test page</span></a>
  </div>
  <div class="badge">User session &nbsp;|&nbsp; isolated browser &nbsp;|&nbsp; background desktop</div>
</main>
<script>
function go(){
  let q=document.getElementById('q').value.trim();
  if(!q)return;
  let lower=q.toLowerCase();
  if(lower.indexOf('://')>0){location.href=q;return;}
  let host=lower.split(':')[0];
  let parts=host.split('.');
  let ipv4=parts.length===4&&parts.every(function(p){let n=Number(p);return p!==''&&Number.isInteger(n)&&n>=0&&n<=255;});
  let local=lower==='localhost'||lower.indexOf('localhost:')===0||
    ['.local','.lan','.home','.internal'].some(function(s){return host.endsWith(s);});
  if(local||ipv4){location.href='http://'+q;return;}
  if(q.indexOf(' ')<0&&q.indexOf('.')>0){location.href='https://'+q;return;}
  location.href='https://www.bing.com/search?q='+encodeURIComponent(q);
}
</script>
</body>
</html>
)HI5";
}

void NavigateHomePage() {
    if (!g_webview) return;
    g_showingHome = true;
    if (g_addressEdit) SetWindowTextW(g_addressEdit, L"Hi5 Web Home");
    LogInfo("[backstage-browser] navigate home");
    g_webview->NavigateToString(Hi5HomeHtml());
}

void UpdateAddressFromSource() {
    if (!g_webview || !g_addressEdit) return;
    LPWSTR source = nullptr;
    if (SUCCEEDED(g_webview->get_Source(&source)) && source) {
        const std::wstring current = source;
        if (g_showingHome && current == L"about:blank") {
            SetWindowTextW(g_addressEdit, L"Hi5 Web Home");
        } else {
            g_showingHome = false;
            SetWindowTextW(g_addressEdit, source);
        }
        CoTaskMemFree(source);
    }
}

void NavigateTo(const std::wstring& raw) {
    if (!g_webview) return;
    const std::wstring trimmed = Trim(raw);
    std::wstring lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (trimmed.empty() || lower == L"about:blank" || lower == L"hi5://home") {
        NavigateHomePage();
        return;
    }

    g_showingHome = false;
    const std::wstring url = NormaliseUrl(trimmed);
    if (g_addressEdit) SetWindowTextW(g_addressEdit, url.c_str());
    LogInfo("[backstage-browser] navigate");
    g_webview->Navigate(url.c_str());
}

void NavigateFromAddressBar() {
    if (!g_addressEdit) return;
    const int len = GetWindowTextLengthW(g_addressEdit);
    std::wstring text(static_cast<size_t>(std::max(0, len)) + 1u, L'\0');
    if (len > 0) GetWindowTextW(g_addressEdit, text.data(), len + 1);
    text.resize(static_cast<size_t>(std::max(0, len)));
    NavigateTo(text);
}

void UpdateNavigationButtons() {
    BOOL canGoBack = FALSE;
    BOOL canGoForward = FALSE;
    if (g_webview) {
        g_webview->get_CanGoBack(&canGoBack);
        g_webview->get_CanGoForward(&canGoForward);
    }
    if (g_backButton) EnableWindow(g_backButton, canGoBack);
    if (g_forwardButton) EnableWindow(g_forwardButton, canGoForward);
}

LRESULT CALLBACK AddressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        NavigateFromAddressBar();
        if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        UpdateAddressFromSource();
        return 0;
    }
    return g_addressOriginalProc
        ? CallWindowProcW(g_addressOriginalProc, hwnd, msg, wp, lp)
        : DefWindowProcW(hwnd, msg, wp, lp);
}

HFONT BrowserUiFont() {
    static HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    return font;
}

void ApplyUiFont(HWND hwnd) {
    if (!hwnd) return;
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(BrowserUiFont()), TRUE);
}

HBRUSH BrowserToolbarBrush() {
    static HBRUSH brush = CreateSolidBrush(RGB(248, 250, 253));
    return brush;
}

void ApplyRoundedRegion(HWND hwnd, int width, int height, int radius) {
    if (!hwnd || width <= 0 || height <= 0) return;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (!region) return;
    if (!SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
}

void DrawBrowserButton(const DRAWITEMSTRUCT* dis) {
    if (!dis || !dis->hDC) return;

    RECT rc = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;
    const bool hot = (dis->itemState & ODS_HOTLIGHT) != 0;
    const bool primary = dis->CtlID == IDC_BROWSER_GO;

    COLORREF fill = primary ? RGB(27, 126, 214) : RGB(255, 255, 255);
    COLORREF border = primary ? RGB(27, 126, 214) : RGB(215, 224, 235);
    COLORREF textColor = primary ? RGB(255, 255, 255) : RGB(45, 59, 78);

    if (hot && !pressed) {
        fill = primary ? RGB(38, 139, 232) : RGB(241, 246, 252);
        border = primary ? RGB(38, 139, 232) : RGB(182, 201, 224);
    }
    if (pressed) {
        fill = primary ? RGB(18, 104, 184) : RGB(226, 235, 246);
        border = primary ? RGB(18, 104, 184) : RGB(163, 187, 216);
    }
    if (disabled) {
        fill = RGB(246, 248, 251);
        border = RGB(230, 234, 240);
        textColor = RGB(162, 171, 182);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, brush);
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, 9, 9);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    HPEN glyphPen = CreatePen(PS_SOLID, 2, textColor);
    HGDIOBJ oldGlyphPen = SelectObject(dis->hDC, glyphPen);
    HGDIOBJ oldGlyphBrush = SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));

    if (dis->CtlID == IDC_BROWSER_BACK) {
        MoveToEx(dis->hDC, cx + 6, cy, nullptr); LineTo(dis->hDC, cx - 6, cy);
        MoveToEx(dis->hDC, cx - 6, cy, nullptr); LineTo(dis->hDC, cx - 1, cy - 5);
        MoveToEx(dis->hDC, cx - 6, cy, nullptr); LineTo(dis->hDC, cx - 1, cy + 5);
    }
    else if (dis->CtlID == IDC_BROWSER_FORWARD || dis->CtlID == IDC_BROWSER_GO) {
        MoveToEx(dis->hDC, cx - 6, cy, nullptr); LineTo(dis->hDC, cx + 6, cy);
        MoveToEx(dis->hDC, cx + 6, cy, nullptr); LineTo(dis->hDC, cx + 1, cy - 5);
        MoveToEx(dis->hDC, cx + 6, cy, nullptr); LineTo(dis->hDC, cx + 1, cy + 5);
    }
    else if (dis->CtlID == IDC_BROWSER_HOME) {
        MoveToEx(dis->hDC, cx - 7, cy - 1, nullptr); LineTo(dis->hDC, cx, cy - 7); LineTo(dis->hDC, cx + 7, cy - 1);
        MoveToEx(dis->hDC, cx - 5, cy - 2, nullptr); LineTo(dis->hDC, cx - 5, cy + 6);
        LineTo(dis->hDC, cx + 5, cy + 6); LineTo(dis->hDC, cx + 5, cy - 2);
    }
    else if (dis->CtlID == IDC_BROWSER_REFRESH) {
        Arc(dis->hDC, cx - 7, cy - 7, cx + 7, cy + 7, cx + 6, cy - 4, cx - 4, cy - 6);
        MoveToEx(dis->hDC, cx + 5, cy - 5, nullptr); LineTo(dis->hDC, cx + 7, cy - 1);
        MoveToEx(dis->hDC, cx + 5, cy - 5, nullptr); LineTo(dis->hDC, cx + 1, cy - 5);
    }

    SelectObject(dis->hDC, oldGlyphBrush);
    SelectObject(dis->hDC, oldGlyphPen);
    DeleteObject(glyphPen);

    if (focused && !disabled) {
        RECT focus = rc;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(dis->hDC, &focus);
    }
}

void CreateBrowserControls(HWND parent) {
    const DWORD iconButtonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;

    g_backButton = CreateWindowExW(0, L"BUTTON", L"Back",
        iconButtonStyle,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BROWSER_BACK),
        GetModuleHandleW(nullptr), nullptr);

    g_forwardButton = CreateWindowExW(0, L"BUTTON", L"Forward",
        iconButtonStyle,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BROWSER_FORWARD),
        GetModuleHandleW(nullptr), nullptr);

    g_homeButton = CreateWindowExW(0, L"BUTTON", L"Home",
        iconButtonStyle,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BROWSER_HOME),
        GetModuleHandleW(nullptr), nullptr);

    g_refreshButton = CreateWindowExW(0, L"BUTTON", L"Refresh",
        iconButtonStyle,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BROWSER_REFRESH),
        GetModuleHandleW(nullptr), nullptr);

    g_addressEdit = CreateWindowExW(0, L"EDIT", g_initialUrl.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BROWSER_ADDRESS),
        GetModuleHandleW(nullptr), nullptr);

    g_goButton = CreateWindowExW(0, L"BUTTON", L"Go",
        iconButtonStyle,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BROWSER_GO),
        GetModuleHandleW(nullptr), nullptr);

    g_contextLabel = CreateWindowExW(0, L"STATIC", L"User session  |  Isolated browser",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BROWSER_CONTEXT),
        GetModuleHandleW(nullptr), nullptr);

    g_statusLabel = CreateWindowExW(0, L"STATIC",
        L"Starting Hi5 Web browser engine...\r\n\r\nThis page will be replaced when WebView2 is ready.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, 0, 0, 0, parent, nullptr,
        GetModuleHandleW(nullptr), nullptr);

    ApplyUiFont(g_backButton);
    ApplyUiFont(g_forwardButton);
    ApplyUiFont(g_homeButton);
    ApplyUiFont(g_refreshButton);
    ApplyUiFont(g_addressEdit);
    ApplyUiFont(g_goButton);
    ApplyUiFont(g_contextLabel);
    ApplyUiFont(g_statusLabel);

    if (g_addressEdit) {
        g_addressOriginalProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_addressEdit, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(AddressWndProc)));
        SendMessageW(g_addressEdit, EM_SETSEL, 0, -1);
    }

    UpdateNavigationButtons();
}

void ResizeBrowserUi() {
    if (!g_hwnd) return;

    RECT client{};
    GetClientRect(g_hwnd, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);

    int x = 10;
    const int buttonY = 9;
    const int buttonH = 34;
    const int iconButtonW = 38;

    auto moveButton = [&](HWND hwnd, int w) {
        if (hwnd) {
            MoveWindow(hwnd, x, buttonY, w, buttonH, TRUE);
            ApplyRoundedRegion(hwnd, w, buttonH, 9);
        }
        x += w + 6;
    };

    moveButton(g_backButton, iconButtonW);
    moveButton(g_forwardButton, iconButtonW);
    moveButton(g_homeButton, iconButtonW);
    moveButton(g_refreshButton, iconButtonW);

    const int goWidth = 42;
    const int contextWidth = 184;
    const int addressWidth = std::max(180, width - x - goWidth - contextWidth - 30);

    if (g_addressEdit) {
        MoveWindow(g_addressEdit, x, buttonY, addressWidth, buttonH, TRUE);
        ApplyRoundedRegion(g_addressEdit, addressWidth, buttonH, 9);
        SendMessageW(g_addressEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
    }
    x += addressWidth + 6;
    moveButton(g_goButton, goWidth);

    if (g_contextLabel) {
        MoveWindow(g_contextLabel, x + 4, buttonY + 7, std::max(90, width - x - 12), 22, TRUE);
    }

    if (g_statusLabel) {
        MoveWindow(g_statusLabel, 24, kToolbarHeight + 72,
            std::max(120, width - 48), 120, TRUE);
    }

    if (g_controller) {
        RECT bounds{ 0, kToolbarHeight, width, std::max(kToolbarHeight + 1, height) };
        g_controller->put_Bounds(bounds);
    }
}

std::wstring BrowserUserDataFolder() {
    PWSTR localAppData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) && localAppData) {
        base = localAppData;
        CoTaskMemFree(localAppData);
    }
    if (base.empty()) {
        wchar_t tempPath[MAX_PATH]{};
        const DWORD n = GetTempPathW(MAX_PATH, tempPath);
        base = (n > 0 && n < MAX_PATH) ? tempPath : L"C:\\Windows\\Temp";
    }

    std::wstring folder = base + L"\\Hi5Central\\WebView2\\Background";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        LogWarn("[backstage-browser] failed to create user data folder ec=" + std::to_string(ec.value()));
    } else {
        LogInfo("[backstage-browser] user data folder ready");
    }
    return folder;
}

void InitWebView2() {
    const std::wstring userData = BrowserUserDataFolder();

    LPWSTR runtimeVersion = nullptr;
    const HRESULT runtimeHr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &runtimeVersion);
    if (SUCCEEDED(runtimeHr) && runtimeVersion) {
        if (g_statusLabel) {
            const std::wstring status =
                L"Starting Hi5 Web browser engine...\r\n\r\nWebView2 Runtime " +
                std::wstring(runtimeVersion);
            SetWindowTextW(g_statusLabel, status.c_str());
        }
        LogInfo("[backstage-browser] WebView2 runtime detected");
        CoTaskMemFree(runtimeVersion);
    } else {
        if (g_statusLabel) {
            SetWindowTextW(g_statusLabel,
                L"Microsoft Edge WebView2 Runtime was not detected.\r\n\r\n"
                L"Install or repair WebView2 Runtime on the target device.");
        }
        LogWarn("[backstage-browser] WebView2 runtime lookup failed hr=" +
            std::to_string(static_cast<long>(runtimeHr)));
    }

    LogInfo("[backstage-browser] creating WebView2 environment");

    // Private-desktop capture uses PrintWindow/GDI from the Backstage host.
    // WebView2 normally renders through GPU/DirectComposition surfaces, which often
    // capture as a blank white rectangle from an inactive/private desktop. Force the
    // WebView2 child process toward software/GDI-compatible rendering for this
    // Backstage experiment.
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (options) {
        options->put_AdditionalBrowserArguments(
            L"--disable-gpu "
            L"--disable-gpu-compositing "
            L"--disable-direct-composition "
            L"--disable-accelerated-2d-canvas "
            L"--disable-features=CalculateNativeWinOcclusion,DirectCompositionSwapChainPresenter");
        LogInfo("[backstage-browser] WebView2 software-rendering flags enabled for private-desktop capture");
    }

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userData.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    if (g_hwnd) KillTimer(g_hwnd, kWebViewInitTimerId);
                    if (g_statusLabel) {
                        SetWindowTextW(g_statusLabel,
                            L"Hi5 Web could not start the WebView2 environment.\r\n\r\n"
                            L"Install or repair Microsoft Edge WebView2 Runtime on the target device.");
                    }
                    LogWarn("[backstage-browser] CreateCoreWebView2Environment failed hr=" + std::to_string(static_cast<long>(result)));
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                if (g_hwnd) KillTimer(g_hwnd, kWebViewInitTimerId);
                                if (g_statusLabel) {
                                    SetWindowTextW(g_statusLabel,
                                        L"Hi5 Web started, but its browser surface could not be created.\r\n\r\n"
                                        L"Check the Agent diagnostics for the WebView2 error code.");
                                }
                                LogWarn("[backstage-browser] CreateCoreWebView2Controller failed hr=" + std::to_string(static_cast<long>(result)));
                                return S_OK;
                            }

                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            if (g_hwnd) KillTimer(g_hwnd, kWebViewInitTimerId);
                            if (g_statusLabel && g_webview) ShowWindow(g_statusLabel, SW_HIDE);
                            ResizeBrowserUi();

                            ComPtr<ICoreWebView2Settings> settings;
                            if (g_webview && SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                settings->put_IsStatusBarEnabled(TRUE);
                                settings->put_AreDefaultContextMenusEnabled(TRUE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(TRUE);
                            }

                            if (g_webview) {
                                EventRegistrationToken navigationToken{};
                                g_webview->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            BOOL success = FALSE;
                                            if (args) args->get_IsSuccess(&success);
                                            UpdateAddressFromSource();
                                            UpdateNavigationButtons();
                                            LogInfo(std::string("[backstage-browser] navigation completed success=") +
                                                (success ? "1" : "0"));
                                            return S_OK;
                                        }).Get(),
                                    &navigationToken);

                                EventRegistrationToken newWindowToken{};
                                g_webview->add_NewWindowRequested(
                                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                            if (!args) return S_OK;

                                            LPWSTR uri = nullptr;
                                            if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                                                NavigateTo(uri);
                                                CoTaskMemFree(uri);
                                            }
                                            args->put_Handled(TRUE);
                                            LogInfo("[backstage-browser] popup/new-window request kept inside Hi5 Web");
                                            return S_OK;
                                        }).Get(),
                                    &newWindowToken);

                                LogInfo("[backstage-browser] navigating initial URL");
                                NavigateTo(g_initialUrl);
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        if (g_hwnd) KillTimer(g_hwnd, kWebViewInitTimerId);
        if (g_statusLabel) {
            SetWindowTextW(g_statusLabel,
                L"Hi5 Web could not initialise WebView2.\r\n\r\n"
                L"Check the target device's WebView2 Runtime installation.");
        }
        LogWarn("[backstage-browser] CreateCoreWebView2EnvironmentWithOptions call failed hr=" + std::to_string(static_cast<long>(hr)));
    }
}

LRESULT CALLBACK BrowserWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateBrowserControls(hwnd);
        ResizeBrowserUi();
        SetTimer(hwnd, kWebViewInitTimerId, 15000, nullptr);
        InitWebView2();
        return 0;

    case WM_TIMER:
        if (wp == kWebViewInitTimerId && !g_webview) {
            KillTimer(hwnd, kWebViewInitTimerId);
            if (g_statusLabel) {
                SetWindowTextW(g_statusLabel,
                    L"Hi5 Web is taking too long to start.\r\n\r\n"
                    L"The WebView2 Runtime was found, but the browser process did not become ready. "
                    L"Check Agent diagnostics for [backstage-browser] entries.");
            }
            LogWarn("[backstage-browser] startup timeout waiting for WebView2");
            return 0;
        }
        break;

    case WM_SIZE:
        ResizeBrowserUi();
        return 0;

    case WM_DRAWITEM:
        if (lp) {
            const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (dis->CtlType == ODT_BUTTON) {
                DrawBrowserButton(dis);
                return TRUE;
            }
        }
        break;

    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH white = GetSysColorBrush(COLOR_WINDOW);
        FillRect(dc, &client, white);
        RECT toolbar{ 0, 0, client.right, kToolbarHeight };
        FillRect(dc, &toolbar, BrowserToolbarBrush());
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(225, 230, 237));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, 0, kToolbarHeight - 1, nullptr);
        LineTo(dc, client.right, kToolbarHeight - 1);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND child = reinterpret_cast<HWND>(lp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(75, 88, 104));
        if (child == g_contextLabel) return reinterpret_cast<LRESULT>(BrowserToolbarBrush());
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(32, 45, 62));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSER_BACK:
            if (g_webview) {
                BOOL canGoBack = FALSE;
                g_webview->get_CanGoBack(&canGoBack);
                if (canGoBack) g_webview->GoBack();
            }
            return 0;

        case IDC_BROWSER_FORWARD:
            if (g_webview) {
                BOOL canGoForward = FALSE;
                g_webview->get_CanGoForward(&canGoForward);
                if (canGoForward) g_webview->GoForward();
            }
            return 0;

        case IDC_BROWSER_HOME:
            NavigateHomePage();
            return 0;

        case IDC_BROWSER_REFRESH:
            if (g_webview) g_webview->Reload();
            return 0;

        case IDC_BROWSER_GO:
            NavigateFromAddressBar();
            if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;

        default:
            break;
        }
        break;

    case WM_SETFOCUS:
        if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_webview.Reset();
        g_controller.Reset();
        g_hwnd = nullptr;
        g_backButton = nullptr;
        g_forwardButton = nullptr;
        g_homeButton = nullptr;
        g_refreshButton = nullptr;
        g_addressEdit = nullptr;
        g_goButton = nullptr;
        g_contextLabel = nullptr;
        g_statusLabel = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

namespace hi5 {

int RunBackstageBrowserMain(int argc, char** argv) {
    LogInfo("[backstage-browser] process start");
    if (auto url = GetArgValue(argc, argv, "--url")) {        g_initialUrl = NormaliseUrl(Utf8ToWide(*url));
    }

    HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) {
        LogWarn("[backstage-browser] OleInitialize failed hr=" + std::to_string(static_cast<long>(ole)));
    }

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"Hi5CentralBackstageBrowserHostWindow";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);    wc.lpfnWndProc = BrowserWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        cls,
        L"Hi5 Web",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        96,
        72,
        1240,
        780,
        nullptr,
        nullptr,
        hinst,
        nullptr);

    if (!hwnd) {
        LogWarn("[backstage-browser] CreateWindowExW failed err=" + std::to_string(GetLastError()));
        if (SUCCEEDED(ole)) OleUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (ctrl && (msg.wParam == L'L' || msg.wParam == L'l')) {
                if (g_addressEdit) {
                    SetFocus(g_addressEdit);
                    SendMessageW(g_addressEdit, EM_SETSEL, 0, -1);
                }
                continue;
            }

            if (msg.wParam == VK_F5) {
                if (g_webview) g_webview->Reload();
                continue;
            }

            if (alt && msg.wParam == VK_LEFT) {
                if (g_webview) {
                    BOOL canGoBack = FALSE;
                    g_webview->get_CanGoBack(&canGoBack);
                    if (canGoBack) g_webview->GoBack();
                }
                continue;
            }

            if (alt && msg.wParam == VK_RIGHT) {
                if (g_webview) {
                    BOOL canGoForward = FALSE;
                    g_webview->get_CanGoForward(&canGoForward);
                    if (canGoForward) g_webview->GoForward();
                }
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(ole)) OleUninitialize();
    LogInfo("[backstage-browser] stopped");
    return 0;
}

} // namespace hi5