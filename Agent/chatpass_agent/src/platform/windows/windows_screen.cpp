#include "frame_source.h"
#include <libyuv/convert.h>
#include "util/log.h"
#include "windows_wgc_capture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

    static bool Hi5CompositeCursorEnabled() {
        static const bool enabled = []() {
            char buf[16]{};
            DWORD n = GetEnvironmentVariableA("HI5_COMPOSITE_CURSOR", buf, static_cast<DWORD>(sizeof(buf)));
            if (n == 0 || n >= sizeof(buf)) return false;
            std::string v(buf, buf + n);
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return v == "1" || v == "true" || v == "yes" || v == "on";
        }();
        return enabled;
    }
    // Retain the actual DXGI HRESULT so diagnostics can distinguish no adapter,
    // driver access-denied, secure desktop and duplication-session limits.
    static std::runtime_error DxgiError(const char* operation, HRESULT result) {
        std::ostringstream detail;
        detail << operation << " hr=0x" << std::hex
               << static_cast<unsigned long>(result);
        return std::runtime_error(detail.str());
    }

    struct OutputTarget {
        int index = 0;
        UINT adapterIndex = 0;
        UINT outputIndex = 0;
        DisplayInfo info;
    };

    static uint8_t clampByte(int v) {
        return static_cast<uint8_t>(std::max(0, std::min(255, v)));
    }

    static std::string wideToUtf8(const wchar_t* w) {
        if (!w || !*w) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        // The measured byte count includes the terminating NUL. Allocate
        // all of it before asking WideCharToMultiByte to write `len` bytes:
        // using len - 1 silently corrupts the heap on every enumeration.
        std::string out(static_cast<size_t>(len), '\0');
        const int written = WideCharToMultiByte(
            CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
        if (written != len) return {};
        out.resize(static_cast<size_t>(written - 1));
        return out;
    }

    static DisplayInfo buildAllMonitorsInfo() {
        DisplayInfo d;
        d.index = -1;
        d.name = "All Monitors";
        d.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        d.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        d.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        d.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        d.primary = false;
        return d;
    }

    static std::vector<OutputTarget> enumerateTargets() {
        std::vector<OutputTarget> out;

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return out;
        }

        UINT globalIndex = 0;

        for (UINT a = 0;; ++a) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterHr = factory->EnumAdapters1(a, &adapter);
            if (adapterHr == DXGI_ERROR_NOT_FOUND) break;
            // Adapter/display enumeration can fail temporarily during lock,
            // hot-plug and desktop changes. Never dereference a failed COM
            // result or a null adapter.
            if (FAILED(adapterHr) || !adapter) break;

            DXGI_ADAPTER_DESC1 ad{};
            if (FAILED(adapter->GetDesc1(&ad))) continue;
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            for (UINT o = 0;; ++o) {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputHr = adapter->EnumOutputs(o, &output);
                if (outputHr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(outputHr) || !output) break;

                DXGI_OUTPUT_DESC od{};
                if (FAILED(output->GetDesc(&od))) continue;

                RECT r = od.DesktopCoordinates;
                if (!od.AttachedToDesktop || r.right <= r.left ||
                    r.bottom <= r.top) continue;

                OutputTarget t;
                t.index = static_cast<int>(globalIndex++);
                t.adapterIndex = a;
                t.outputIndex = o;
                t.info.index = t.index;
                t.info.name = wideToUtf8(od.DeviceName);
                t.info.x = r.left;
                t.info.y = r.top;
                t.info.width = r.right - r.left;
                t.info.height = r.bottom - r.top;
                t.info.primary = !!od.AttachedToDesktop && (r.left == 0 && r.top == 0);

                out.push_back(std::move(t));
            }
        }

        return out;
    }

    static void bgraToI420(const uint8_t* src, int srcStride, int width, int height, I420Frame& out) {
        if (!src || width <= 0 || height <= 0 || srcStride < width * 4)
            throw std::runtime_error("Invalid BGRA frame for I420 conversion");

        out.width = width;
        out.height = height;
        const int uvWidth = (width + 1) / 2;
        const int uvHeight = (height + 1) / 2;
        out.y.resize(static_cast<size_t>(width) * height);
        out.u.resize(static_cast<size_t>(uvWidth) * uvHeight);
        out.v.resize(static_cast<size_t>(uvWidth) * uvHeight);

        // Native DXGI and 32-bit GDI produce BGRA memory bytes; libyuv's
        // ARGBToI420 uses this ordering on little-endian Windows.
        // This is the ACTUAL Windows capture implementation compiled by CMake.
        // The previous scalar Y/UV loops processed every pixel on every frame.
        if (libyuv::ARGBToI420(src, srcStride,
                out.y.data(), width,
                out.u.data(), uvWidth,
                out.v.data(), uvWidth,
                width, height) != 0)
            throw std::runtime_error("libyuv ARGBToI420 failed");
    }

    static void compositeCursorBgra(int originX, int originY, int width, int height,
        std::vector<uint8_t>& bgra, int stride);

    struct GdiCaptureSurface {
        HDC memDc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ original = nullptr;
        uint8_t* bits = nullptr;
        int width = 0;
        int height = 0;
        HDESK threadDesktop = nullptr;

        void reset() {
            if (memDc && original && original != HGDI_ERROR) {
                SelectObject(memDc, original);
            }
            if (dib) DeleteObject(dib);
            if (memDc) DeleteDC(memDc);
            memDc = nullptr;
            dib = nullptr;
            original = nullptr;
            bits = nullptr;
            width = 0;
            height = 0;
            threadDesktop = nullptr;
        }
        ~GdiCaptureSurface() { reset(); }

        bool prepare(HDC screenDc, int w, int h) {
            const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
            if (memDc && dib && bits && width == w && height == h &&
                threadDesktop == desktop) return true;
            reset();
            memDc = CreateCompatibleDC(screenDc);
            if (!memDc) return false;
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!dib || !pixels) { reset(); return false; }
            original = SelectObject(memDc, dib);
            if (!original || original == HGDI_ERROR) { reset(); return false; }
            bits = static_cast<uint8_t*>(pixels);
            width = w;
            height = h;
            threadDesktop = desktop;
            return true;
        }
    };

    static void compositeCursorOnGdiSurface(HDC targetDc, int originX, int originY,
                                            int width, int height) {
        if (!targetDc || width <= 0 || height <= 0) return;
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
        ICONINFO info{};
        POINT hotspot{0, 0};
        if (GetIconInfo(ci.hCursor, &info)) {
            hotspot.x = static_cast<LONG>(info.xHotspot);
            hotspot.y = static_cast<LONG>(info.yHotspot);
            if (info.hbmMask) DeleteObject(info.hbmMask);
            if (info.hbmColor) DeleteObject(info.hbmColor);
        }
        const int drawX = ci.ptScreenPos.x - originX - hotspot.x;
        const int drawY = ci.ptScreenPos.y - originY - hotspot.y;
        if (drawX > width || drawY > height || drawX < -256 || drawY < -256) return;
        // GDI source already has a compatible, writable 32-bit DIB. Compose
        // directly onto that cached surface instead of constructing/copying
        // a second full-frame bitmap solely to draw the pointer.
        DrawIconEx(targetDc, drawX, drawY, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    }

    static bool captureRectBgraGdi(int x, int y, int srcW, int srcH, int dstW, int dstH,
        std::vector<uint8_t>& bgraOut, int& strideOut) {
        if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return false;
        // The broad gdi-stage-stats previously grouped BitBlt, GdiFlush,
        // cursor composition and memory copy into one 75-80ms operation.
        // Measure each stage to identify the actual cost before changing
        // layered-window semantics or sacrificing desktop quality.
        using GdiClock = std::chrono::steady_clock;
        const auto stageBegin = GdiClock::now();
        HDC screenDC = GetDC(nullptr);
        if (!screenDC) return false;
        static thread_local GdiCaptureSurface scratch;
        if (!scratch.prepare(screenDC, dstW, dstH)) {
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        const auto stagePrepared = GdiClock::now();
        // BitBlt avoids the HALFTONE scaler for full-resolution capture.
        // StretchBlt remains available for bounded preview thumbnails.
        BOOL copied = FALSE;
        if (srcW == dstW && srcH == dstH) {
            copied = BitBlt(scratch.memDc, 0, 0, dstW, dstH,
                            screenDC, x, y, SRCCOPY | CAPTUREBLT);
        } else {
            SetStretchBltMode(scratch.memDc, HALFTONE);
            copied = StretchBlt(scratch.memDc, 0, 0, dstW, dstH,
                                screenDC, x, y, srcW, srcH, SRCCOPY | CAPTUREBLT);
        }
        const auto stageBlt = GdiClock::now();
        ReleaseDC(nullptr, screenDC);
        if (!copied) {
            // A Windows desktop switch can invalidate old DC contents.
            // Never serve stale pixels; rebuild the surface on next attempt.
            scratch.reset();
            return false;
        }

        // CreateDIBSection exposes direct CPU pixel memory. Windows GDI can
        // batch drawing commands: flush BitBlt/StretchBlt before memcpy so a
        // reused surface never yields pixels from the previous desktop frame.
        compositeCursorOnGdiSurface(scratch.memDc, x, y, dstW, dstH);
        const auto stageCursor = GdiClock::now();
        GdiFlush();
        const auto stageFlush = GdiClock::now();
        strideOut = dstW * 4;
        bgraOut.resize(static_cast<size_t>(strideOut) * dstH);
        std::memcpy(bgraOut.data(), scratch.bits, bgraOut.size());
        const auto stageCopy = GdiClock::now();

        // Diagnostic only. An administrator may opt into a small, bounded
        // A/B probe with HKLM\SOFTWARE\Hi5Central\Agent\GdiBitBltProbe=1.
        // The stream ALWAYS uses the original CAPTUREBLT image. No user
        // desktop or secure/UAC capture semantics change in this build.
        // Benchmark plain SRCCOPY into a separate cached surface, never
        // replacing the pixels already copied into the live frame.
        struct BltProbe {
            GdiCaptureSurface surface;
            unsigned long captures = 0;
            unsigned int samples = 0;
        };
        static thread_local BltProbe bltProbe{};
        if (bltProbe.samples < 12 && (++bltProbe.captures % 60) == 0) {
            DWORD enabled = 0;
            DWORD bytes = sizeof(enabled);
            const bool enabledByAdmin =
                RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Hi5Central\\Agent",
                    "GdiBitBltProbe", RRF_RT_REG_DWORD, nullptr,
                    &enabled, &bytes) == ERROR_SUCCESS && enabled == 1;
            if (enabledByAdmin) {
                wchar_t desktopName[64]{};
                DWORD nameBytes = 0;
                const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
                const bool onDefaultDesktop =
                    desktop && GetUserObjectInformationW(desktop, UOI_NAME,
                        desktopName, sizeof(desktopName), &nameBytes) &&
                    _wcsicmp(desktopName, L"Default") == 0;
                // Never benchmark the Winlogon/secure desktop.
                if (onDefaultDesktop) {
                    HDC probeSource = GetDC(nullptr);
                    if (probeSource) {
                        if (bltProbe.surface.prepare(probeSource, dstW, dstH)) {
                            const auto probeStart = GdiClock::now();
                            BOOL probeOk = FALSE;
                            if (srcW == dstW && srcH == dstH) {
                                probeOk = BitBlt(bltProbe.surface.memDc,
                                    0, 0, dstW, dstH, probeSource, x, y, SRCCOPY);
                            } else {
                                SetStretchBltMode(bltProbe.surface.memDc, HALFTONE);
                                probeOk = StretchBlt(bltProbe.surface.memDc,
                                    0, 0, dstW, dstH, probeSource, x, y,
                                    srcW, srcH, SRCCOPY);
                            }
                            GdiFlush();
                            const auto probeEnd = GdiClock::now();
                            if (probeOk) {
                                // Sparse pixel difference is a diagnostic,
                                // not proof of visual equivalence: the desktop
                                // may change between the two captures.
                                int differing = 0;
                                int sampled = 0;
                                const size_t stepX = std::max(1, dstW / 96);
                                const size_t stepY = std::max(1, dstH / 54);
                                for (int yy = 0; yy < dstH; yy += static_cast<int>(stepY)) {
                                    for (int xx = 0; xx < dstW; xx += static_cast<int>(stepX)) {
                                        const size_t pixel = (static_cast<size_t>(yy) *
                                            static_cast<size_t>(dstW) +
                                            static_cast<size_t>(xx)) * 4;
                                        const uint8_t* baseline = bgraOut.data() + pixel;
                                        const uint8_t* candidate = bltProbe.surface.bits + pixel;
                                        ++sampled;
                                        if (std::abs(int(baseline[0]) - int(candidate[0])) > 12 ||
                                            std::abs(int(baseline[1]) - int(candidate[1])) > 12 ||
                                            std::abs(int(baseline[2]) - int(candidate[2])) > 12) {
                                            ++differing;
                                        }
                                    }
                                }
                                LogInfo("[capture] gdi-blt-probe mode=nonvisible " +
                                    std::string("default_desktop=1") +
                                    " display=" + std::to_string(dstW) + "x" +
                                    std::to_string(dstH) +
                                    " baseline_capblt_ms=" +
                                    std::to_string(std::chrono::duration<double, std::milli>(
                                        stageBlt - stagePrepared).count()) +
                                    " candidate_srcopy_ms=" +
                                    std::to_string(std::chrono::duration<double, std::milli>(
                                        probeEnd - probeStart).count()) +
                                    " pixels_different=" + std::to_string(differing) +
                                    " pixels_sampled=" + std::to_string(sampled));
                                ++bltProbe.samples;
                            } else {
                                LogWarn("[capture] gdi-blt-probe plain SRCCOPY failed; "
                                    "live CAPTUREBLT unaffected");
                            }
                        }
                        ReleaseDC(nullptr, probeSource);
                    }
                }
            }
        }

        struct GdiTiming {
            uint64_t count = 0;
            double acquireMs = 0, blitMs = 0, cursorMs = 0;
            double flushMs = 0, memcpyMs = 0;
        };
        static thread_local GdiTiming timing{};
        auto ms = [](GdiClock::time_point a, GdiClock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        timing.acquireMs += ms(stageBegin, stagePrepared);
        timing.blitMs += ms(stagePrepared, stageBlt);
        timing.cursorMs += ms(stageBlt, stageCursor);
        timing.flushMs += ms(stageCursor, stageFlush);
        timing.memcpyMs += ms(stageFlush, stageCopy);
        if (++timing.count == 50) {
            const double n = static_cast<double>(timing.count);
            LogInfo("[capture] gdi-copy-breakdown samples=" +
                std::to_string(timing.count) +
                " display=" + std::to_string(dstW) + "x" + std::to_string(dstH) +
                " acquire_avg_ms=" + std::to_string(timing.acquireMs / n) +
                " blit_avg_ms=" + std::to_string(timing.blitMs / n) +
                " cursor_avg_ms=" + std::to_string(timing.cursorMs / n) +
                " flush_avg_ms=" + std::to_string(timing.flushMs / n) +
                " memcpy_avg_ms=" + std::to_string(timing.memcpyMs / n));
            timing = {};
        }
        return true;
    }

    static void compositeCursorBgra(int originX, int originY, int width, int height,
        std::vector<uint8_t>& bgra, int stride) {
        if (width <= 0 || height <= 0 || bgra.empty() || stride <= 0) return;

        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
            return;
        }

        ICONINFO ii{};
        POINT hot{ 0, 0 };
        if (GetIconInfo(ci.hCursor, &ii)) {
            hot.x = static_cast<LONG>(ii.xHotspot);
            hot.y = static_cast<LONG>(ii.yHotspot);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }

        const int drawX = ci.ptScreenPos.x - originX - hot.x;
        const int drawY = ci.ptScreenPos.y - originY - hot.y;

        // Fast reject for the common case where the pointer is outside this monitor.
        if (drawX > width || drawY > height || drawX < -128 || drawY < -128) {
            return;
        }

        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC ? screenDC : nullptr);
        if (!memDC) {
            if (screenDC) ReleaseDC(nullptr, screenDC);
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            if (dib) DeleteObject(dib);
            DeleteDC(memDC);
            if (screenDC) ReleaseDC(nullptr, screenDC);
            return;
        }

        // A GPU texture's RowPitch can exceed width*4. The new GDI DIB
        // always has tightly packed 32-bpp rows: copying RowPitch*height
        // into it would overflow the bitmap and crash the helper.
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        const size_t srcStride = static_cast<size_t>(stride);
        if (srcStride < rowBytes ||
            bgra.size() < srcStride * static_cast<size_t>(height)) {
            DeleteObject(dib);
            DeleteDC(memDC);
            if (screenDC) ReleaseDC(nullptr, screenDC);
            return;
        }
        for (int row = 0; row < height; ++row) {
            std::memcpy(static_cast<uint8_t*>(bits) +
                        static_cast<size_t>(row) * rowBytes,
                        bgra.data() + static_cast<size_t>(row) * srcStride,
                        rowBytes);
        }

        HGDIOBJ old = SelectObject(memDC, dib);
        DrawIconEx(memDC, drawX, drawY, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        SelectObject(memDC, old);

        for (int row = 0; row < height; ++row) {
            std::memcpy(bgra.data() + static_cast<size_t>(row) * srcStride,
                        static_cast<uint8_t*>(bits) +
                            static_cast<size_t>(row) * rowBytes,
                        rowBytes);
        }

        DeleteObject(dib);
        DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }

    static std::vector<uint8_t> encodeJpegWic(const uint8_t* bgra, int width, int height, int stride, int quality) {
        std::vector<uint8_t> out;

        HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninit = SUCCEEDED(initHr);

        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(
            width,
            height,
            GUID_WICPixelFormat32bppBGRA,
            stride,
            stride * height,
            const_cast<BYTE*>(bgra),
            &bitmap))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        if (FAILED(converter->Initialize(
            bitmap.Get(),
            GUID_WICPixelFormat24bppBGR,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        ComPtr<IStream> memStream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memStream))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        ComPtr<IWICStream> wicStream;
        if (FAILED(factory->CreateStream(&wicStream))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        if (FAILED(wicStream->InitializeFromIStream(memStream.Get()))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        if (FAILED(encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> bag;
        if (FAILED(encoder->CreateNewFrame(&frame, &bag))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        if (bag) {
            PROPBAG2 pb{};
            pb.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v{};
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = std::max(0.1f, std::min(1.0f, quality / 100.0f));
            bag->Write(1, &pb, &v);
            VariantClear(&v);
        }

        if (FAILED(frame->Initialize(bag.Get()))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        if (FAILED(frame->SetSize(width, height))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->SetPixelFormat(&format))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        if (FAILED(frame->WriteSource(converter.Get(), nullptr))) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        HGLOBAL hMem = nullptr;
        if (FAILED(GetHGlobalFromStream(memStream.Get(), &hMem)) || !hMem) {
            if (shouldUninit) CoUninitialize();
            return out;
        }

        const SIZE_T size = GlobalSize(hMem);
        void* ptr = GlobalLock(hMem);
        if (ptr && size > 0) {
            out.assign(static_cast<uint8_t*>(ptr), static_cast<uint8_t*>(ptr) + size);
        }
        if (ptr) GlobalUnlock(hMem);

        if (shouldUninit) CoUninitialize();
        return out;
    }
}

struct DesktopFrameSource::Impl {
    mutable std::mutex mu;

    std::vector<OutputTarget> targets;
    int currentIndex = 0;
    // Keep the requested monitor even when the safe GDI fallback sets -1.
    // An explicit All Monitors selection (-1) must never trigger DXGI retries.
    int requestedIndex = 0;
    std::string currentName;
    std::chrono::steady_clock::time_point lastDxgiRetry{};
    std::chrono::steady_clock::time_point nextDisplayProbeAt{};
    bool allowDxgiRecovery = false; // normal desktop only, never secure/UAC
    std::unique_ptr<WgcMonitorCapture> wgc;
    bool wgcAttempted = false;
    bool wgcReceivedFrame = false;
    std::chrono::steady_clock::time_point wgcStarted{};

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;

    int width = 0;
    int height = 0;

    I420Frame lastFrame;
    // Reuse the multi-megabyte intermediate buffer instead of allocating a
    // new one on every capture. Streamer owns this frame source on one thread.
    std::vector<uint8_t> gdiBgraBuffer;
    uint64_t gdiProfileSamples = 0;
    double gdiCopyMs = 0;
    double gdiConvertMs = 0;
    std::chrono::steady_clock::time_point lastGdiRefresh{};
    uint64_t frameId = 0;

    Impl() {
        refreshDisplaysLocked();
        // Keep capturing through GDI if DXGI enumeration/duplication is not
        // available at helper startup. Unlike before, preserve the requested
        // display so the normal desktop can recover later without reconnecting.
        try {
            initForCurrentLocked();
            if (currentIndex == -1 && requestedIndex != -1) {
                LogWarn("[capture] no DXGI display at startup; using GDI until recovery");
                lastDxgiRetry = std::chrono::steady_clock::now();
            }
        } catch (const std::exception& ex) {
            LogWarn("[capture] DXGI startup failed: " + std::string(ex.what()) +
                " action=gdi_fallback");
            tryStartWgcLocked();
            useGdiFallbackLocked();
        }
    }

    // A registry opt-in, absent on every other customer. WGC respects its
    // mandatory capture border; it is never started for Winlogon/UAC.
    static bool WgcTrialEnabled() {
        DWORD enabled = 0;
        DWORD length = sizeof(enabled);
        return RegGetValueA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Hi5Central\\Agent", "WgcCaptureTrial",
            RRF_RT_REG_DWORD, nullptr, &enabled, &length) == ERROR_SUCCESS &&
            enabled == 1;
    }

    void tryStartWgcLocked() {
        if (wgcAttempted || requestedIndex < 0 || !WgcTrialEnabled()) return;
        wgcAttempted = true;

        wchar_t desktopName[96]{};
        DWORD nameBytes = 0;
        const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
        if (!desktop || !GetUserObjectInformationW(
                desktop, UOI_NAME, desktopName, sizeof(desktopName), &nameBytes) ||
            _wcsicmp(desktopName, L"Default") != 0) {
            LogWarn("[capture] WGC trial skipped: current thread is not on Default desktop");
            return;
        }

        const auto target = std::find_if(targets.begin(), targets.end(),
            [this](const OutputTarget& t) {
                return t.info.index == requestedIndex;
            });
        if (target == targets.end()) {
            LogWarn("[capture] WGC trial skipped: requested monitor missing");
            return;
        }
        std::string failure;
        auto candidate = std::make_unique<WgcMonitorCapture>();
        if (!candidate->Start(target->info.x, target->info.y,
                              target->info.width, target->info.height, failure)) {
            LogWarn("[capture] WGC trial start failed: " + failure +
                    " action=gdi_fallback");
            return;
        }
        wgc = std::move(candidate);
        wgcReceivedFrame = false;
        wgcStarted = std::chrono::steady_clock::now();
        LogInfo("[capture] WGC trial started monitor=" +
            std::to_string(requestedIndex) + " resolution=" +
            std::to_string(target->info.width) + "x" +
            std::to_string(target->info.height) +
            " privacy_border=enabled waiting_for_first_frame");
    }

    void useGdiFallbackLocked() {
        resetD3DLocked();
        currentIndex = -1;
        currentName.clear();
        const auto desktop = buildAllMonitorsInfo();
        width = desktop.width;
        height = desktop.height;
    }

    // Called only while the DesktopFrameSource mutex is held.
    void retryDxgiIfDueLocked() {
        if (!allowDxgiRecovery || requestedIndex < 0 || currentIndex != -1) return;
        const auto now = std::chrono::steady_clock::now();
        if (lastDxgiRetry.time_since_epoch().count() != 0 &&
            now - lastDxgiRetry < std::chrono::seconds(30)) return;
        lastDxgiRetry = now;

        refreshDisplaysLocked(true);
        const auto target = std::find_if(targets.begin(), targets.end(),
            [this](const OutputTarget& entry) { return entry.info.index == requestedIndex; });
        if (target == targets.end()) {
            LogWarn("[capture] DXGI retry: requested display not enumerated; retaining GDI");
            return;
        }
        currentIndex = target->info.index;
        currentName = target->info.name;
        try {
            initForCurrentLocked();
            if (duplication) {
                lastFrame = {};
                LogInfo("[capture] DXGI restored display=" + std::to_string(currentIndex) +
                    " resolution=" + std::to_string(width) + "x" + std::to_string(height));
            } else {
                useGdiFallbackLocked();
                LogWarn("[capture] DXGI retry returned no duplication; retaining GDI");
            }
        } catch (const std::exception& ex) {
            LogWarn("[capture] DXGI retry failed: " + std::string(ex.what()) +
                " action=continue_gdi");
            useGdiFallbackLocked();
        }
    }

    void resetD3DLocked() {
        duplication.Reset();
        staging.Reset();
        context.Reset();
        device.Reset();
        width = 0;
        height = 0;
    }

    void refreshDisplaysLocked(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < nextDisplayProbeAt) return;
        nextDisplayProbeAt = now + std::chrono::seconds(1);
        const int previousIndex = currentIndex;
        const std::string previousName = currentName;

        auto discovered = enumerateTargets();
        if (discovered.empty()) {
            // Preserve an ACTIVE duplication on transient empty enumeration.
            // Formerly this reset DXGI and permanently fell back to GDI.
            if (duplication && !targets.empty()) return;
            targets.clear();
            currentIndex = -1;
            currentName.clear();
            resetD3DLocked();
            return;
        }
        targets = std::move(discovered);

        if (previousIndex == -1) {
            currentIndex = -1;
            currentName.clear();
            return;
        }

        auto it = std::find_if(targets.begin(), targets.end(),
            [&](const OutputTarget& t) { return !previousName.empty() && t.info.name == previousName; });

        if (it != targets.end()) {
            currentIndex = it->info.index;
            currentName = it->info.name;
            return;
        }

        auto primaryIt = std::find_if(targets.begin(), targets.end(),
            [](const OutputTarget& t) { return t.info.primary; });

        if (primaryIt != targets.end()) {
            currentIndex = primaryIt->info.index;
            currentName = primaryIt->info.name;
            return;
        }

        currentIndex = targets.front().info.index;
        currentName = targets.front().info.name;
    }

    const OutputTarget* findCurrentTargetLocked() const {
        for (const auto& t : targets) {
            if (t.info.index == currentIndex) {
                return &t;
            }
        }
        return nullptr;
    }

    void initForCurrentLocked() {
        refreshDisplaysLocked();

        if (currentIndex == -1) {
            resetD3DLocked();
            auto d = buildAllMonitorsInfo();
            width = d.width;
            height = d.height;
            return;
        }

        const auto* target = findCurrentTargetLocked();
        if (!target) {
            throw std::runtime_error("Current display target not found");
        }

        resetD3DLocked();

        ComPtr<IDXGIFactory1> factory;
        if (const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
            throw DxgiError("CreateDXGIFactory1", hr);
        }

        ComPtr<IDXGIAdapter1> adapter;
        if (const HRESULT hr = factory->EnumAdapters1(target->adapterIndex, &adapter); FAILED(hr)) {
            throw DxgiError("EnumAdapters1", hr);
        }

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        if (const HRESULT hr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            &fl,
            1,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context); FAILED(hr)) {
            throw DxgiError("D3D11CreateDevice", hr);
        }

        ComPtr<IDXGIOutput> output;
        if (const HRESULT hr = adapter->EnumOutputs(target->outputIndex, &output); FAILED(hr)) {
            throw DxgiError("EnumOutputs", hr);
        }

        ComPtr<IDXGIOutput1> output1;
        if (const HRESULT hr = output.As(&output1); FAILED(hr)) {
            throw DxgiError("Query IDXGIOutput1", hr);
        }

        if (const HRESULT hr = output1->DuplicateOutput(device.Get(), &duplication); FAILED(hr)) {
            if (hr == E_ACCESSDENIED) {
                // Read-only context: distinguishes wrong desktop/session from
                // a GPU/driver refusal. No SetThreadDesktop, privilege change
                // or secure desktop switch is attempted here.
                wchar_t threadDesktop[96]{};
                wchar_t inputDesktop[96]{};
                DWORD ignored = 0;
                DWORD inputError = 0;
                const HDESK thread = GetThreadDesktop(GetCurrentThreadId());
                if (!thread || !GetUserObjectInformationW(thread, UOI_NAME,
                        threadDesktop, sizeof(threadDesktop), &ignored)) {
                    wcscpy_s(threadDesktop, L"unavailable");
                }
                const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
                if (input) {
                    ignored = 0;
                    if (!GetUserObjectInformationW(input, UOI_NAME,
                            inputDesktop, sizeof(inputDesktop), &ignored)) {
                        wcscpy_s(inputDesktop, L"unavailable");
                    }
                    CloseDesktop(input);
                } else {
                    inputError = GetLastError();
                    wcscpy_s(inputDesktop, L"unavailable");
                }
                DWORD processSession = 0;
                ProcessIdToSessionId(GetCurrentProcessId(), &processSession);
                DXGI_OUTPUT_DESC outputDesc{};
                const HRESULT descResult = output->GetDesc(&outputDesc);
                LogWarn("[capture] dxgi-access-context" +
                    std::string(" process_session=") + std::to_string(processSession) +
                    " console_session=" + std::to_string(WTSGetActiveConsoleSessionId()) +
                    " thread_desktop=" + wideToUtf8(threadDesktop) +
                    " input_desktop=" + wideToUtf8(inputDesktop) +
                    " input_error=" + std::to_string(inputError) +
                    " adapter_index=" + std::to_string(target->adapterIndex) +
                    " output_index=" + std::to_string(target->outputIndex) +
                    " output_desc_ok=" + std::to_string(SUCCEEDED(descResult) ? 1 : 0));
            }
            throw DxgiError("DuplicateOutput", hr);
        }

        width = target->info.width;
        height = target->info.height;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging); FAILED(hr)) {
            throw DxgiError("CreateTexture2D(staging)", hr);
        }
    }

    void setDxgiRecoveryEnabledLocked(bool enabled) {
        allowDxgiRecovery = enabled;
    }

    bool setDisplayIndexLocked(int index) {
        // The normal capture helper is constructed before RunStreamerMain
        // explicitly selects args.display. If DXGI duplication fails, that
        // constructor already starts the opt-in WGC trial on requestedIndex.
        // Selecting the *same* output moments later must not close a running
        // capture session and synchronously reactivate its WinRT factory.
        // Physical Lenovo Agent 1.0.225: that duplicate activation crashed
        // (0xC0000005 RVA 0x31C8EF, windows_wgc_capture.cpp:207), while
        // its first WGC trial had started successfully 30 ms earlier.
        // Switching to a different display still recreates WGC below.
        if (index >= 0 && index == requestedIndex &&
            wgc && wgc->IsRunning()) {
            LogInfo("[capture] retain running WGC trial for unchanged display=" +
                std::to_string(index) + " action=skip_duplicate_winrt_activation");
            return true;
        }

        refreshDisplaysLocked(true);

        // Switching monitors discards the old monitor's WGC capture item.
        wgc.reset();
        wgcAttempted = false;
        wgcReceivedFrame = false;
        if (index == -1) {
            requestedIndex = -1;
            currentIndex = -1;
            currentName.clear();
            initForCurrentLocked();
            return true;
        }

        auto it = std::find_if(targets.begin(), targets.end(),
            [&](const OutputTarget& t) { return t.info.index == index; });

        if (it == targets.end()) {
            return false;
        }

        requestedIndex = it->info.index;
        currentIndex = it->info.index;
        currentName = it->info.name;
        try {
            initForCurrentLocked();
            if (duplication) {
                lastFrame = {};
                LogInfo("[capture] DXGI selected display=" + std::to_string(currentIndex) +
                    " resolution=" + std::to_string(width) + "x" + std::to_string(height));
            }
        } catch (const std::exception& ex) {
            LogWarn("[capture] DXGI display selection failed: " + std::string(ex.what()) +
                " display=" + std::to_string(index) + " action=gdi_fallback");
            lastDxgiRetry = std::chrono::steady_clock::now();
            tryStartWgcLocked();
            useGdiFallbackLocked();
        }
        return true;
    }

    DisplayInfo currentDisplayInfoLocked() const {
        if (currentIndex == -1) {
            return buildAllMonitorsInfo();
        }

        const auto* t = findCurrentTargetLocked();
        if (!t) return {};
        return t->info;
    }

    FrameCaptureResult captureGdiDisplayLocked(const DisplayInfo& display) {
        FrameCaptureResult result;
        if (display.width <= 0 || display.height <= 0) {
            result.frame = lastFrame;
            result.hasFrame = !lastFrame.y.empty();
            result.frameId = frameId;
            return result;
        }

        // Profile the real compiled Windows GDI backend, not the unused
        // alternate frame_source.cpp implementation. Separate screen-copy
        // latency from BGRA->I420 conversion for the next capture tuning pass.
        const auto copyStart = std::chrono::steady_clock::now();
        int stride = 0;
        const bool copied = captureRectBgraGdi(
            display.x, display.y, display.width, display.height,
            display.width, display.height, gdiBgraBuffer, stride);
        const auto copyEnd = std::chrono::steady_clock::now();
        if (!copied) {
            // Preserve the previous valid frame on transient desktop changes.
            result.frame = lastFrame;
            result.hasFrame = !lastFrame.y.empty();
            result.frameId = frameId;
            return result;
        }

        I420Frame converted;
        bgraToI420(gdiBgraBuffer.data(), stride,
                   display.width, display.height, converted);
        const auto convertEnd = std::chrono::steady_clock::now();
        gdiCopyMs += std::chrono::duration<double, std::milli>(
            copyEnd - copyStart).count();
        gdiConvertMs += std::chrono::duration<double, std::milli>(
            convertEnd - copyEnd).count();
        if (++gdiProfileSamples >= 25) {
            LogInfo("[capture] gdi-stage-stats samples=" +
                std::to_string(gdiProfileSamples) +
                " display=" + std::to_string(display.width) + "x" +
                std::to_string(display.height) +
                " copy_avg_ms=" + std::to_string(
                    gdiCopyMs / static_cast<double>(gdiProfileSamples)) +
                " convert_avg_ms=" + std::to_string(
                    gdiConvertMs / static_cast<double>(gdiProfileSamples)));
            gdiProfileSamples = 0;
            gdiCopyMs = 0;
            gdiConvertMs = 0;
        }

        // Exactly one I420 copy remains: lastFrame is a recovery image used
        // when DXGI/WGC times out or a display is switching. Move the fresh
        // frame into the result to avoid the former second full-frame copy.
        lastFrame = converted;
        result.frame = std::move(converted);
        lastGdiRefresh = std::chrono::steady_clock::now();
        result.hasFrame = true;
        result.changed = true; // GDI has no dirty-frame signal; sampled upstream
        result.frameId = ++frameId;
        return result;
    }

    FrameCaptureResult captureAllMonitorsGdiLocked() {
        return captureGdiDisplayLocked(buildAllMonitorsInfo());
    }

    FrameCaptureResult captureCurrentDisplayGdiLocked() {
        return captureGdiDisplayLocked(currentDisplayInfoLocked());
    }

    FrameCaptureResult captureOneLocked() {
        refreshDisplaysLocked();

        if (wgc && wgc->IsRunning()) {
            std::vector<uint8_t> bgra;
            int w = 0, h = 0, pitch = 0;
            std::string error;
            if (wgc->TryReadBgra(bgra, w, h, pitch, error)) {
                // Compare suspected empty compositor output against GDI.
                // Truly dark desktops remain valid; nonempty GDI means WGC
                // returned an unusable frame, so stop WGC for this session.
                auto nonDarkSamples = [](const std::vector<uint8_t>& pixels,
                                         int stride, int width, int height) {
                    int count = 0;
                    if (width <= 0 || height <= 0 ||
                        stride < width * 4 ||
                        pixels.size() < static_cast<size_t>(stride) * height)
                        return count;
                    for (int row = 0; row < 10; ++row) {
                        const int y = ((2 * row + 1) * height) / 20;
                        for (int col = 0; col < 16; ++col) {
                            const int x = ((2 * col + 1) * width) / 32;
                            const auto* p = pixels.data() +
                                static_cast<size_t>(y) * stride + x * 4;
                            if (std::max({p[0], p[1], p[2]}) > 24) ++count;
                        }
                    }
                    return count;
                };
                const int wgcSamples = nonDarkSamples(bgra, pitch, w, h);
                if (wgcSamples <= 1) {
                    const auto target = std::find_if(targets.begin(), targets.end(),
                        [this](const OutputTarget& t) {
                            return t.info.index == requestedIndex;
                        });
                    if (target != targets.end() &&
                        w == target->info.width && h == target->info.height) {
                        std::vector<uint8_t> reference;
                        int referenceStride = 0;
                        if (captureRectBgraGdi(target->info.x, target->info.y,
                            w, h, w, h, reference, referenceStride)) {
                            const int gdiSamples = nonDarkSamples(
                                reference, referenceStride, w, h);
                            if (gdiSamples > 14) {
                                LogWarn("[capture] WGC produced dark frames, GDI "
                                    "still shows desktop; disabling WGC backend "
                                    "wgc_samples=" + std::to_string(wgcSamples) +
                                    " gdi_samples=" + std::to_string(gdiSamples));
                                wgc.reset();
                                lastDxgiRetry = std::chrono::steady_clock::now();
                                return captureAllMonitorsGdiLocked();
                            }
                        }
                    }
                }
                I420Frame frame;
                // Use the proven desktop BGRA->I420 path; codec/transport unchanged.
                const auto target = std::find_if(targets.begin(), targets.end(),
                    [this](const OutputTarget& t) {
                        return t.info.index == requestedIndex;
                    });
                if (target != targets.end()) {
                    compositeCursorBgra(target->info.x, target->info.y,
                                        w, h, bgra, pitch);
                }
                bgraToI420(bgra.data(), pitch, w, h, frame);
                lastFrame = frame;
                FrameCaptureResult result;
                result.frame = std::move(frame);
                result.hasFrame = true;
                result.changed = true;
                result.changedIsNative = true; // WGC compositor delivered a new frame
                result.frameId = ++frameId;
                if (!wgcReceivedFrame) {
                    wgcReceivedFrame = true;
                    LogInfo("[capture] WGC first frame monitor=" +
                        std::to_string(requestedIndex) + " resolution=" +
                        std::to_string(w) + "x" + std::to_string(h) +
                        " backend=wgc");
                }
                return result;
            }
            if (!error.empty() ||
                (!wgcReceivedFrame &&
                 std::chrono::steady_clock::now() - wgcStarted > std::chrono::seconds(4))) {
                LogWarn("[capture] WGC trial stopped: " +
                    (error.empty() ? std::string("no first frame after 4s") : error) +
                    " action=continue_gdi");
                wgc.reset();
                lastDxgiRetry = std::chrono::steady_clock::now();
            } else if (wgcReceivedFrame && !lastFrame.y.empty()) {
                FrameCaptureResult result;
                result.frame = lastFrame;
                result.hasFrame = true;
                result.changed = false;
                result.frameId = frameId;
                return result;
            }
            // Before the first WGC compositor frame, continue live GDI.
            return captureAllMonitorsGdiLocked();
        }

        if (currentIndex == -1) {
            retryDxgiIfDueLocked();
        }
        if (currentIndex == -1) {
            return captureAllMonitorsGdiLocked();
        }

        if (!duplication || !findCurrentTargetLocked()) {
            try {
                initForCurrentLocked();
            } catch (const std::exception& ex) {
                LogWarn("[capture] DXGI reinitialization failed: " + std::string(ex.what()) +
                    " action=gdi_fallback");
                lastDxgiRetry = std::chrono::steady_clock::now();
                useGdiFallbackLocked();
                return captureAllMonitorsGdiLocked();
            }
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;

        // Keep this timeout below the target frame interval. A long wait here makes
        // the streamer look like it is running at 4-12 FPS whenever DXGI has no
        // fresh dirty frame. On timeout we reuse the last frame, which lets the
        // service/encoder maintain smooth RTP pacing without changing capture architecture.
        constexpr UINT kAcquireTimeoutMs = 8;
        HRESULT hr = duplication->AcquireNextFrame(kAcquireTimeoutMs, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            const bool noFrameYet = lastFrame.y.empty() || lastFrame.width <= 0 || lastFrame.height <= 0;
            if (noFrameYet) {
                return captureCurrentDisplayGdiLocked();
            }

            FrameCaptureResult result;
            result.frame = lastFrame;
            result.hasFrame = true;
            result.changed = false;
            result.frameId = frameId;
            return result;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            try {
                initForCurrentLocked();
            } catch (const std::exception& ex) {
                LogWarn("[capture] DXGI access lost, restore failed: " +
                    std::string(ex.what()) + " action=gdi_fallback");
                lastDxgiRetry = std::chrono::steady_clock::now();
                useGdiFallbackLocked();
                return captureAllMonitorsGdiLocked();
            }
            if (lastFrame.y.empty() || lastFrame.width <= 0 || lastFrame.height <= 0) {
                return captureCurrentDisplayGdiLocked();
            }

            FrameCaptureResult result;
            result.frame = lastFrame;
            result.hasFrame = true;
            result.changed = false;
            result.frameId = frameId;
            return result;
        }
        if (FAILED(hr)) {
            LogWarn(std::string("[capture] DXGI acquire failed: ") + DxgiError("AcquireNextFrame", hr).what() +
                " action=gdi_fallback");
            lastDxgiRetry = std::chrono::steady_clock::now();
            useGdiFallbackLocked();
            return captureAllMonitorsGdiLocked();
        }

        const bool cursorOnly = frameInfo.TotalMetadataBufferSize == 0 && frameInfo.LastMouseUpdateTime.QuadPart != 0;

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(resource.As(&tex))) {
            duplication->ReleaseFrame();
            throw std::runtime_error("Query ID3D11Texture2D failed");
        }

        context->CopyResource(staging.Get(), tex.Get());
        duplication->ReleaseFrame();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            throw std::runtime_error("Map staging failed");
        }

        const int rowPitch = static_cast<int>(mapped.RowPitch);
        std::vector<uint8_t> bgra(static_cast<size_t>(rowPitch) * static_cast<size_t>(height));
        std::memcpy(bgra.data(), mapped.pData, bgra.size());
        context->Unmap(staging.Get(), 0);

        const DisplayInfo d = currentDisplayInfoLocked();
        compositeCursorBgra(d.x, d.y, width, height, bgra, rowPitch);

        I420Frame out;
        bgraToI420(bgra.data(), rowPitch, width, height, out);

        lastFrame = out;

        FrameCaptureResult result;
        result.frame = out;
        result.hasFrame = true;
        result.changed = true;
        result.changedIsNative = true; // DXGI reported a new acquired frame
        result.cursorOnly = cursorOnly;
        result.frameId = ++frameId;
        return result;
    }
};

DesktopFrameSource::DesktopFrameSource() : m_impl(new Impl()) {}
DesktopFrameSource::~DesktopFrameSource() { delete m_impl; }
DesktopFrameSource::DesktopFrameSource(DesktopFrameSource&& other) noexcept : m_impl(other.m_impl) {
    other.m_impl = nullptr;
}
DesktopFrameSource& DesktopFrameSource::operator=(DesktopFrameSource&& other) noexcept {
    if (this != &other) {
        delete m_impl;
        m_impl = other.m_impl;
        other.m_impl = nullptr;
    }
    return *this;
}

I420Frame DesktopFrameSource::nextFrame() {
    FrameCaptureResult r = nextFrameEx();
    return r.frame;
}

FrameCaptureResult DesktopFrameSource::nextFrameEx() {
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->captureOneLocked();
}

std::vector<DisplayInfo> DesktopFrameSource::listDisplays() const {
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->refreshDisplaysLocked();

    std::vector<DisplayInfo> out;
    out.reserve(m_impl->targets.size() + 1);

    for (const auto& t : m_impl->targets) {
        out.push_back(t.info);
    }

    out.push_back(buildAllMonitorsInfo());
    return out;
}

DisplayInfo DesktopFrameSource::currentDisplayInfo() const {
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->refreshDisplaysLocked();
    return m_impl->currentDisplayInfoLocked();
}

int DesktopFrameSource::currentDisplayIndex() const {
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->refreshDisplaysLocked();
    return m_impl->currentIndex;
}

bool DesktopFrameSource::setDisplayIndex(int index) {
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->setDisplayIndexLocked(index);
}

void DesktopFrameSource::setDxgiRecoveryEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->setDxgiRecoveryEnabledLocked(enabled);
}

std::vector<uint8_t> DesktopFrameSource::capturePreviewJpeg(int index, int maxWidth, int maxHeight, int quality) const {
    std::lock_guard<std::mutex> lock(m_impl->mu);

    std::vector<DisplayInfo> displays;
    for (const auto& t : m_impl->targets) {
        displays.push_back(t.info);
    }
    displays.push_back(buildAllMonitorsInfo());

    auto it = std::find_if(displays.begin(), displays.end(),
        [&](const DisplayInfo& d) { return d.index == index; });

    if (it == displays.end()) return {};

    const DisplayInfo d = *it;
    if (d.width <= 0 || d.height <= 0) return {};

    double scale = std::min(
        static_cast<double>(maxWidth) / std::max(1, d.width),
        static_cast<double>(maxHeight) / std::max(1, d.height)
    );
    scale = std::min(1.0, scale);

    const int dstW = std::max(1, static_cast<int>(std::round(d.width * scale)));
    const int dstH = std::max(1, static_cast<int>(std::round(d.height * scale)));

    std::vector<uint8_t> bgra;
    int stride = 0;
    if (!captureRectBgraGdi(d.x, d.y, d.width, d.height, dstW, dstH, bgra, stride)) {
        return {};
    }

    return encodeJpegWic(bgra.data(), dstW, dstH, stride, quality);
}