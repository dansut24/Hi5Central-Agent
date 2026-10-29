#include "frame_source.h"
#include <libyuv/convert.h>

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
        std::string out(static_cast<size_t>(len), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr) != len) return {};
        out.resize(static_cast<size_t>(len - 1));
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
            if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_ADAPTER_DESC1 ad{};
            adapter->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            for (UINT o = 0;; ++o) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;

                DXGI_OUTPUT_DESC od{};
                if (FAILED(output->GetDesc(&od))) continue;

                RECT r = od.DesktopCoordinates;

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
            throw std::runtime_error("Invalid desktop frame for I420 conversion");
        out.width = width;
        out.height = height;
        out.y.resize(static_cast<size_t>(width) * height);
        const int chromaWidth = (width + 1) / 2;
        const int chromaHeight = (height + 1) / 2;
        out.u.resize(static_cast<size_t>(chromaWidth) * chromaHeight);
        out.v.resize(static_cast<size_t>(chromaWidth) * chromaHeight);

        // DXGI_FORMAT_B8G8R8A8_UNORM and GDI 32-bit DIB are BGRA *bytes*
        // (B,G,R,A), which libyuv calls ARGB on little-endian Windows.
        // SIMD-enabled libyuv replaces the previous two full-frame scalar
        // loops. This runs for every changed frame at native resolution.
        const int result = libyuv::ARGBToI420(
            src, srcStride,
            out.y.data(), width,
            out.u.data(), chromaWidth,
            out.v.data(), chromaWidth,
            width, height);
        if (result != 0)
            throw std::runtime_error("libyuv ARGBToI420 failed");
    }

    static void compositeCursorBgra(int originX, int originY, int width, int height,
        std::vector<uint8_t>& bgra, int stride);

    // A native-resolution GDI frame can be 16MB. Cache compatible DC and
    // top-down DIB by capture thread, so only the display dimensions changing
    // (or a failed blit) recreates the expensive GDI objects. Each Windows
    // capture helper has its own capture thread/process and desktop.
    struct GdiCaptureSurface {
        HDC memDc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ original = nullptr;
        uint8_t* bits = nullptr;
        int width = 0;
        int height = 0;

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
        }
        ~GdiCaptureSurface() { reset(); }

        bool prepare(HDC screenDc, int w, int h) {
            if (memDc && dib && bits && width == w && height == h) return true;
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
        HDC screenDC = GetDC(nullptr);
        if (!screenDC) return false;
        static thread_local GdiCaptureSurface scratch;
        if (!scratch.prepare(screenDC, dstW, dstH)) {
            ReleaseDC(nullptr, screenDC);
            return false;
        }

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
        GdiFlush();
        strideOut = dstW * 4;
        bgraOut.resize(static_cast<size_t>(strideOut) * dstH);
        std::memcpy(bgraOut.data(), scratch.bits, bgraOut.size());
        return true;
    }

    static void compositeCursorBgra(int originX, int originY, int width, int height,
        std::vector<uint8_t>& bgra, int stride) {
        if (width <= 0 || height <= 0 || bgra.empty() || stride <= 0) return;
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

        ICONINFO ii{};
        POINT hot{0, 0};
        int iconW = std::max(1, GetSystemMetrics(SM_CXCURSOR));
        int iconH = std::max(1, GetSystemMetrics(SM_CYCURSOR));
        if (GetIconInfo(ci.hCursor, &ii)) {
            hot.x = static_cast<LONG>(ii.xHotspot);
            hot.y = static_cast<LONG>(ii.yHotspot);
            BITMAP bmp{};
            if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bmp), &bmp)) {
                iconW = std::max(iconW, static_cast<int>(bmp.bmWidth));
                iconH = std::max(iconH, static_cast<int>(bmp.bmHeight));
            } else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bmp), &bmp)) {
                iconW = std::max(iconW, static_cast<int>(bmp.bmWidth));
                iconH = std::max(iconH, static_cast<int>(bmp.bmHeight) / 2);
            }
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
        const int drawX = ci.ptScreenPos.x - originX - hot.x;
        const int drawY = ci.ptScreenPos.y - originY - hot.y;
        const int left = std::max(0, drawX);
        const int top = std::max(0, drawY);
        const int right = std::min(width, drawX + iconW);
        const int bottom = std::min(height, drawY + iconH);
        if (right <= left || bottom <= top) return;

        // Previously this built and copied a FULL-SCREEN DIB every frame just
        // to draw a small cursor (8 MB at 1080p, 33 MB at 4K). Only copy the
        // exact affected rectangle; the rest of the screenshot is unchanged.
        const int roiW = right - left;
        const int roiH = bottom - top;
        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        if (!memDC) {
            if (screenDC) ReleaseDC(nullptr, screenDC);
            return;
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = roiW;
        bmi.bmiHeader.biHeight = -roiH;
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
        const size_t roiStride = static_cast<size_t>(roiW) * 4;
        for (int row = 0; row < roiH; ++row) {
            const size_t srcOffset = static_cast<size_t>(top + row) * stride +
                                     static_cast<size_t>(left) * 4;
            std::memcpy(static_cast<uint8_t*>(bits) + static_cast<size_t>(row) * roiStride,
                        bgra.data() + srcOffset, roiStride);
        }
        HGDIOBJ old = SelectObject(memDC, dib);
        const BOOL drawn = DrawIconEx(memDC, drawX - left, drawY - top,
                                     ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        GdiFlush();
        if (drawn) {
            for (int row = 0; row < roiH; ++row) {
                const size_t dstOffset = static_cast<size_t>(top + row) * stride +
                                         static_cast<size_t>(left) * 4;
                std::memcpy(bgra.data() + dstOffset,
                            static_cast<const uint8_t*>(bits) + static_cast<size_t>(row) * roiStride,
                            roiStride);
            }
        }
        SelectObject(memDC, old);
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
    // Rebuilding DXGI factories/adapters on every video frame is expensive.
    // Refresh at most once per second, but allow explicit monitor switches.
    std::chrono::steady_clock::time_point nextDisplayProbeAt{};
    int currentIndex = 0;
    int requestedIndex = 0; // -1 explicitly means All Monitors, not DXGI fallback
    std::string currentName;
    bool gdiFallback = false;
    bool allowDxgiRecovery = false; // normal helper only, never UAC helper
    std::chrono::steady_clock::time_point nextDxgiRetryAt{};
    unsigned int dxgiRetryFailures = 0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;

    int width = 0;
    int height = 0;

    I420Frame lastFrame;
    std::vector<uint8_t> gdiBgra; // reusable BGRA buffer, not a 16MB allocation per frame
    std::vector<uint8_t> dxgiBgra; // reuse mapped DXGI staging buffer across frames
    std::chrono::steady_clock::time_point lastGdiRefresh{};
    uint64_t frameId = 0;

    Impl() {
        refreshDisplaysLocked();
        // On secure desktops, locked screens and some virtual displays, DXGI
        // enumeration or DuplicateOutput can fail. Keep the streamer alive
        // and capture the virtual desktop through the existing GDI path.
        try {
            initForCurrentLocked();
        } catch (const std::exception&) {
            useGdiFallbackLocked();
        }
    }

    void useGdiFallbackLocked() {
        resetD3DLocked();
        gdiFallback = requestedIndex >= 0;
        if (gdiFallback && nextDxgiRetryAt.time_since_epoch().count() == 0)
            nextDxgiRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        currentIndex = -1;
        currentName.clear();
        const auto desktop = buildAllMonitorsInfo();
        width = desktop.width;
        height = desktop.height;
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
        // Preserve the existing duplication when the monitor is unchanged.
        // On a display resize/adapter move, rebuild before capturing again.
        const auto* previousTarget = findCurrentTargetLocked();
        const int oldWidth = previousTarget ? previousTarget->info.width : 0;
        const int oldHeight = previousTarget ? previousTarget->info.height : 0;
        const int oldX = previousTarget ? previousTarget->info.x : 0;
        const int oldY = previousTarget ? previousTarget->info.y : 0;
        const UINT oldAdapter = previousTarget ? previousTarget->adapterIndex : 0;
        const UINT oldOutput = previousTarget ? previousTarget->outputIndex : 0;

        auto discovered = enumerateTargets();
        if (discovered.empty()) {
            // A transient empty DXGI enumeration must NOT discard a live
            // duplication. Losing it once previously stranded us in GDI.
            if (duplication && !targets.empty()) return;
            targets.clear();
            if (requestedIndex >= 0) useGdiFallbackLocked();
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
            if (oldWidth != it->info.width || oldHeight != it->info.height ||
                oldX != it->info.x || oldY != it->info.y ||
                oldAdapter != it->adapterIndex || oldOutput != it->outputIndex) {
                resetD3DLocked();
            }
            return;
        }

        auto primaryIt = std::find_if(targets.begin(), targets.end(),
            [](const OutputTarget& t) { return t.info.primary; });

        if (primaryIt != targets.end()) {
            currentIndex = primaryIt->info.index;
            currentName = primaryIt->info.name;
            resetD3DLocked();
            return;
        }

        currentIndex = targets.front().info.index;
        currentName = targets.front().info.name;
        resetD3DLocked();
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
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            throw std::runtime_error("CreateDXGIFactory1 failed");
        }

        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(target->adapterIndex, &adapter))) {
            throw std::runtime_error("EnumAdapters1 failed");
        }

        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        if (FAILED(D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            &fl,
            1,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context))) {
            throw std::runtime_error("D3D11CreateDevice failed");
        }

        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(target->outputIndex, &output))) {
            throw std::runtime_error("EnumOutputs failed");
        }

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) {
            throw std::runtime_error("Query IDXGIOutput1 failed");
        }

        if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) {
            throw std::runtime_error("DuplicateOutput failed");
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

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
            throw std::runtime_error("CreateTexture2D(staging) failed");
        }
        gdiFallback = false;
        dxgiRetryFailures = 0;
        nextDxgiRetryAt = {};
    }

    void tryRestoreDxgiLocked() {
        if (!allowDxgiRecovery || !gdiFallback || requestedIndex == -1) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextDxgiRetryAt) return;
        // A bounded retry, never a busy retry loop: persistent unsupported
        // display drivers keep their functioning GDI stream.
        nextDxgiRetryAt = now + std::chrono::seconds(
            dxgiRetryFailures >= 3 ? 60 : 15);
        ++dxgiRetryFailures;
        if (targets.empty()) return;

        auto selected = std::find_if(targets.begin(), targets.end(),
            [&](const OutputTarget& t) { return t.info.index == requestedIndex; });
        if (selected == targets.end()) {
            selected = std::find_if(targets.begin(), targets.end(),
                [](const OutputTarget& t) { return t.info.primary; });
        }
        if (selected == targets.end()) selected = targets.begin();

        currentIndex = selected->info.index;
        currentName = selected->info.name;
        try {
            initForCurrentLocked();
            // Prior GDI frame remains usable if DXGI initially times out.
        } catch (const std::exception&) {
            useGdiFallbackLocked();
        }
    }

    void setDxgiRecoveryEnabledLocked(bool enabled) {
        allowDxgiRecovery = enabled;
    }

    bool setDisplayIndexLocked(int index) {
        refreshDisplaysLocked(true);

        if (index == -1) {
            requestedIndex = -1;
            gdiFallback = false;
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
        } catch (const std::exception&) {
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

    FrameCaptureResult captureAllMonitorsGdiLocked() {
        FrameCaptureResult result;
        const DisplayInfo d = buildAllMonitorsInfo();
        if (d.width <= 0 || d.height <= 0) {
            result.frame = lastFrame;
            result.hasFrame = !lastFrame.y.empty();
            result.frameId = frameId;
            return result;
        }

        int stride = 0;
        if (!captureRectBgraGdi(d.x, d.y, d.width, d.height, d.width, d.height, gdiBgra, stride)) {
            result.frame = lastFrame;
            result.hasFrame = !lastFrame.y.empty();
            result.frameId = frameId;
            return result;
        }

        I420Frame out;
        bgraToI420(gdiBgra.data(), stride, d.width, d.height, out);
        lastFrame = out;
        lastGdiRefresh = std::chrono::steady_clock::now();
        result.frame = out;
        result.hasFrame = true;
        result.changed = true;
        result.frameId = ++frameId;
        return result;
    }

    FrameCaptureResult captureCurrentDisplayGdiLocked() {
        FrameCaptureResult result;
        const DisplayInfo d = currentDisplayInfoLocked();
        if (d.width <= 0 || d.height <= 0) {
            result.frame = lastFrame;
            result.hasFrame = !lastFrame.y.empty();
            result.frameId = frameId;
            return result;
        }

        int stride = 0;
        if (!captureRectBgraGdi(d.x, d.y, d.width, d.height, d.width, d.height, gdiBgra, stride)) {
            result.frame = lastFrame;
            result.hasFrame = !lastFrame.y.empty();
            result.frameId = frameId;
            return result;
        }

        I420Frame out;
        bgraToI420(gdiBgra.data(), stride, d.width, d.height, out);
        lastFrame = out;
        lastGdiRefresh = std::chrono::steady_clock::now();
        result.frame = out;
        result.hasFrame = true;
        result.changed = true;
        result.frameId = ++frameId;
        return result;
    }

    FrameCaptureResult captureOneLocked() {
        refreshDisplaysLocked();

        if (currentIndex == -1) {
            tryRestoreDxgiLocked();
            if (currentIndex == -1) return captureAllMonitorsGdiLocked();
        }

        if (!duplication || !findCurrentTargetLocked()) {
            try {
                initForCurrentLocked();
            } catch (const std::exception&) {
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
            initForCurrentLocked();
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
            throw std::runtime_error("AcquireNextFrame failed");
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
        dxgiBgra.resize(static_cast<size_t>(rowPitch) * static_cast<size_t>(height));
        std::memcpy(dxgiBgra.data(), mapped.pData, dxgiBgra.size());
        context->Unmap(staging.Get(), 0);

        const DisplayInfo d = currentDisplayInfoLocked();
        compositeCursorBgra(d.x, d.y, width, height, dxgiBgra, rowPitch);

        I420Frame out;
        bgraToI420(dxgiBgra.data(), rowPitch, width, height, out);

        lastFrame = out;

        FrameCaptureResult result;
        result.frame = out;
        result.hasFrame = true;
        result.changed = true;
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