#include "backstage_cef_browser.h"

#include "util/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace hi5 {

#ifndef HI5_ENABLE_CEF_BROWSER

struct BackstageCefBrowser::Impl {
    std::wstring lastError = L"CEF browser backend is not enabled in this build. Rebuild with -DHI5_ENABLE_CEF_BROWSER=ON and CEF_ROOT.";
};

BackstageCefBrowser::BackstageCefBrowser() : impl_(new Impl()) {}
BackstageCefBrowser::~BackstageCefBrowser() { delete impl_; }
bool BackstageCefBrowser::Initialize(const std::wstring&, std::wstring* errorOut) {
    if (errorOut) *errorOut = impl_->lastError;
    return false;
}
void BackstageCefBrowser::Shutdown() {}
bool BackstageCefBrowser::Open(const std::wstring&, int, int, std::wstring* errorOut) {
    if (errorOut) *errorOut = impl_->lastError;
    return false;
}
void BackstageCefBrowser::Close() {}
bool BackstageCefBrowser::IsOpen() const { return false; }
bool BackstageCefBrowser::IsAvailable() const { return false; }
void BackstageCefBrowser::SetViewSize(int, int) {}
void BackstageCefBrowser::DoMessageLoopWork() {}
bool BackstageCefBrowser::PaintToHdc(HDC, int, int, int, int) { return false; }
void BackstageCefBrowser::Navigate(const std::wstring&) {}
void BackstageCefBrowser::SendMouseMove(int, int) {}
void BackstageCefBrowser::SendMouseButton(int, int, bool, int) {}
void BackstageCefBrowser::SendMouseWheel(int, int, int) {}
void BackstageCefBrowser::SendKey(WORD, bool, bool, uint16_t) {}
void BackstageCefBrowser::SendChar(wchar_t) {}
std::wstring BackstageCefBrowser::LastError() const { return impl_ ? impl_->lastError : L"CEF unavailable"; }
int MaybeRunCefSubprocess(int, char**) { return -1; }

#else

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

namespace {

std::string WideToUtf8Local(const std::wstring& value) {
    if (value.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

class Hi5CefApp : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    IMPLEMENT_REFCOUNTING(Hi5CefApp);
};

class OsrClient : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefLoadHandler {
public:
    OsrClient(int width, int height) : width_(std::max(1, width)), height_(std::max(1, height)) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, std::max(1, width_), std::max(1, height_));
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        CEF_REQUIRE_UI_THREAD();
        browser_ = browser;
    }

    bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override { browser_ = nullptr; }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode, const CefString& errorText, const CefString& failedUrl) override {
        std::lock_guard<std::mutex> lock(mu_);
        lastStatus_ = L"Load failed: " + failedUrl.ToWString() + L" - " + errorText.ToWString();
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&, const void* buffer, int width, int height) override {
        if (type != PET_VIEW || !buffer || width <= 0 || height <= 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        width_ = width;
        height_ = height;
        pixels_.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        std::memcpy(pixels_.data(), buffer, pixels_.size());
        hasFrame_ = true;
    }

    void Resize(int width, int height) {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        if (browser_) browser_->GetHost()->WasResized();
    }

    bool PaintToHdc(HDC dc, int x, int y, int dstW, int dstH) {
        std::vector<uint8_t> copy;
        int srcW = 0;
        int srcH = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!hasFrame_ || pixels_.empty()) return false;
            copy = pixels_;
            srcW = width_;
            srcH = height_;
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = srcW;
        bi.bmiHeader.biHeight = -srcH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(dc, x, y, dstW, dstH, 0, 0, srcW, srcH, copy.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        return true;
    }

    void Navigate(const std::wstring& url) {
        if (browser_) browser_->GetMainFrame()->LoadURL(WideToUtf8Local(url));
    }

    void SendMouseMove(int x, int y) {
        if (!browser_) return;
        CefMouseEvent e; e.x = x; e.y = y;
        browser_->GetHost()->SendMouseMoveEvent(e, false);
    }

    void SendMouseButton(int x, int y, bool down, int button) {
        if (!browser_) return;
        CefMouseEvent e; e.x = x; e.y = y;
        CefBrowserHost::MouseButtonType b = MBT_LEFT;
        if (button == 1) b = MBT_MIDDLE;
        if (button == 2) b = MBT_RIGHT;
        browser_->GetHost()->SendMouseClickEvent(e, b, !down, 1);
    }

    void SendMouseWheel(int x, int y, int deltaY) {
        if (!browser_) return;
        CefMouseEvent e; e.x = x; e.y = y;
        browser_->GetHost()->SendMouseWheelEvent(e, 0, deltaY);
    }

    void SendKey(WORD vk, bool down, bool, uint16_t scanCode) {
        if (!browser_) return;
        CefKeyEvent ev;
        ev.windows_key_code = static_cast<int>(vk);
        ev.native_key_code = static_cast<int>(scanCode) << 16;
        ev.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
        browser_->GetHost()->SendKeyEvent(ev);
    }

    void SendChar(wchar_t ch) {
        if (!browser_ || !ch) return;
        CefKeyEvent ev;
        ev.type = KEYEVENT_CHAR;
        ev.windows_key_code = static_cast<int>(ch);
        ev.character = ch;
        ev.unmodified_character = ch;
        browser_->GetHost()->SendKeyEvent(ev);
    }

    bool IsOpen() const { return browser_ != nullptr; }
    std::wstring Status() const { std::lock_guard<std::mutex> lock(mu_); return lastStatus_; }

private:
    int width_ = 1;
    int height_ = 1;
    mutable std::mutex mu_;
    std::vector<uint8_t> pixels_;
    bool hasFrame_ = false;
    std::wstring lastStatus_;
    CefRefPtr<CefBrowser> browser_;
    IMPLEMENT_REFCOUNTING(OsrClient);
};

static bool g_cefInitialized = false;
static CefRefPtr<Hi5CefApp> g_cefApp;

} // namespace

struct BackstageCefBrowser::Impl {
    CefRefPtr<OsrClient> client;
    bool initialized = false;
    bool open = false;
    std::wstring lastError;
};

BackstageCefBrowser::BackstageCefBrowser() : impl_(new Impl()) {}
BackstageCefBrowser::~BackstageCefBrowser() { Shutdown(); delete impl_; }

bool BackstageCefBrowser::Initialize(const std::wstring& cacheDir, std::wstring* errorOut) {
    if (g_cefInitialized) {
        impl_->initialized = true;
        return true;
    }

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop = false;
    settings.external_message_pump = false;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.cache_path) = cacheDir.empty() ? L"C:\\ProgramData\\Hi5Central\\Agent\\CefCache" : cacheDir;

    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    g_cefApp = new Hi5CefApp();

    if (!CefInitialize(mainArgs, settings, g_cefApp.get(), nullptr)) {
        impl_->lastError = L"CefInitialize failed";
        if (errorOut) *errorOut = impl_->lastError;
        return false;
    }

    g_cefInitialized = true;
    impl_->initialized = true;
    LogInfo("[backstage-cef] CEF initialized windowless/off-screen");
    return true;
}

void BackstageCefBrowser::Shutdown() {
    if (impl_) {
        if (impl_->client && impl_->client->IsOpen()) impl_->client->CloseAllBrowsers(true);
        impl_->client = nullptr;
        impl_->open = false;
        impl_->initialized = false;
    }
}

bool BackstageCefBrowser::Open(const std::wstring& initialUrl, int width, int height, std::wstring* errorOut) {
    if (!impl_->initialized && !Initialize(L"", errorOut)) return false;
    impl_->client = new OsrClient(width, height);

    CefWindowInfo wi;
    wi.SetAsWindowless(nullptr);
    CefBrowserSettings bs;
    const std::string url = WideToUtf8Local(initialUrl.empty() ? L"https://www.google.com" : initialUrl);
    const bool ok = CefBrowserHost::CreateBrowser(wi, impl_->client.get(), url, bs, nullptr, nullptr);
    impl_->open = ok;
    if (!ok) {
        impl_->lastError = L"CefBrowserHost::CreateBrowser failed";
        if (errorOut) *errorOut = impl_->lastError;
    } else {
        LogInfo("[backstage-cef] browser created url=" + url);
    }
    return ok;
}

void BackstageCefBrowser::Close() {
    if (impl_->client && impl_->client->IsOpen()) impl_->client->CloseAllBrowsers(true);
    impl_->client = nullptr;
    impl_->open = false;
}

bool BackstageCefBrowser::IsOpen() const { return impl_ && impl_->client && impl_->client->IsOpen(); }
bool BackstageCefBrowser::IsAvailable() const { return true; }
void BackstageCefBrowser::SetViewSize(int width, int height) { if (impl_ && impl_->client) impl_->client->Resize(width, height); }
void BackstageCefBrowser::DoMessageLoopWork() { if (g_cefInitialized) CefDoMessageLoopWork(); }
bool BackstageCefBrowser::PaintToHdc(HDC dc, int x, int y, int width, int height) { return impl_ && impl_->client && impl_->client->PaintToHdc(dc, x, y, width, height); }
void BackstageCefBrowser::Navigate(const std::wstring& url) { if (impl_ && impl_->client) impl_->client->Navigate(url); }
void BackstageCefBrowser::SendMouseMove(int x, int y) { if (impl_ && impl_->client) impl_->client->SendMouseMove(x, y); }
void BackstageCefBrowser::SendMouseButton(int x, int y, bool down, int button) { if (impl_ && impl_->client) impl_->client->SendMouseButton(x, y, down, button); }
void BackstageCefBrowser::SendMouseWheel(int x, int y, int deltaY) { if (impl_ && impl_->client) impl_->client->SendMouseWheel(x, y, deltaY); }
void BackstageCefBrowser::SendKey(WORD vk, bool down, bool extended, uint16_t scanCode) { if (impl_ && impl_->client) impl_->client->SendKey(vk, down, extended, scanCode); }
void BackstageCefBrowser::SendChar(wchar_t ch) { if (impl_ && impl_->client) impl_->client->SendChar(ch); }
std::wstring BackstageCefBrowser::LastError() const { return impl_ ? impl_->lastError : L"CEF not initialized"; }

int MaybeRunCefSubprocess(int argc, char** argv) {
    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    CefRefPtr<Hi5CefApp> app = new Hi5CefApp();
    const int exitCode = CefExecuteProcess(mainArgs, app.get(), nullptr);
    if (exitCode >= 0) return exitCode;
    return -1;
}

#endif // HI5_ENABLE_CEF_BROWSER

} // namespace hi5
