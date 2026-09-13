#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hi5 {

// Backstage-native browser backend.
//
// When HI5_ENABLE_CEF_BROWSER is enabled at build time this uses CEF
// windowless/off-screen rendering and provides BGRA pixels directly to the
// Backstage renderer. When CEF is not enabled, it compiles as a safe stub so
// the rest of the agent keeps building.
class BackstageCefBrowser {
public:
    BackstageCefBrowser();
    ~BackstageCefBrowser();

    bool Initialize(const std::wstring& cacheDir, std::wstring* errorOut = nullptr);
    void Shutdown();

    bool Open(const std::wstring& initialUrl, int width, int height, std::wstring* errorOut = nullptr);
    void Close();
    bool IsOpen() const;
    bool IsAvailable() const;

    void SetViewSize(int width, int height);
    void DoMessageLoopWork();

    bool PaintToHdc(HDC dc, int x, int y, int width, int height);

    void Navigate(const std::wstring& url);
    void SendMouseMove(int x, int y);
    void SendMouseButton(int x, int y, bool down, int button);
    void SendMouseWheel(int x, int y, int deltaY);
    void SendKey(WORD vk, bool down, bool extended, uint16_t scanCode);
    void SendChar(wchar_t ch);

    std::wstring LastError() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Returns >= 0 when the current process was a CEF subprocess and should exit
// with that code. Returns -1 when the normal Hi5Central main() should continue.
int MaybeRunCefSubprocess(int argc, char** argv);

} // namespace hi5
