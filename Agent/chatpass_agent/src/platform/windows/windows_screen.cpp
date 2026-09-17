#include "frame_source.h"

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
        std::string out(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
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
        out.width = width;
        out.height = height;
        out.y.resize(static_cast<size_t>(width) * height);
        out.u.resize(static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2));
        out.v.resize(static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2));

        for (int y = 0; y < height; ++y) {
            const uint8_t* row = src + y * srcStride;
            for (int x = 0; x < width; ++x) {
                const uint8_t b = row[x * 4 + 0];
                const uint8_t g = row[x * 4 + 1];
                const uint8_t r = row[x * 4 + 2];

                const int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                out.y[static_cast<size_t>(y) * width + x] = clampByte(Y);
            }
        }

        const int uvWidth = (width + 1) / 2;
        const int uvHeight = (height + 1) / 2;

        for (int by = 0; by < uvHeight; ++by) {
            for (int bx = 0; bx < uvWidth; ++bx) {
                int sumU = 0;
                int sumV = 0;
                int count = 0;

                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = bx * 2 + dx;
                        const int y = by * 2 + dy;
                        if (x >= width || y >= height) continue;

                        const uint8_t* px = src + y * srcStride + x * 4;
                        const uint8_t b = px[0];
                        const uint8_t g = px[1];
                        const uint8_t r = px[2];

                        const int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                        const int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

                        sumU += U;
                        sumV += V;
                        ++count;
                    }
                }

                out.u[static_cast<size_t>(by) * uvWidth + bx] = clampByte(sumU / std::max(1, count));
                out.v[static_cast<size_t>(by) * uvWidth + bx] = clampByte(sumV / std::max(1, count));
            }
        }
    }

    static void compositeCursorBgra(int originX, int originY, int width, int height,
        std::vector<uint8_t>& bgra, int stride);

    static bool captureRectBgraGdi(int x, int y, int srcW, int srcH, int dstW, int dstH,
        std::vector<uint8_t>& bgraOut, int& strideOut) {
        HDC screenDC = GetDC(nullptr);
        if (!screenDC) return false;

        HDC memDC = CreateCompatibleDC(screenDC);
        if (!memDC) {
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = dstW;
        bmi.bmiHeader.biHeight = -dstH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            DeleteDC(memDC);
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        HGDIOBJ old = SelectObject(memDC, bmp);
        SetStretchBltMode(memDC, HALFTONE);

        StretchBlt(
            memDC,
            0, 0, dstW, dstH,
            screenDC,
            x, y, srcW, srcH,
            SRCCOPY | CAPTUREBLT
        );

        strideOut = dstW * 4;
        bgraOut.resize(static_cast<size_t>(strideOut) * dstH);
        std::memcpy(bgraOut.data(), bits, bgraOut.size());
        compositeCursorBgra(x, y, dstW, dstH, bgraOut, strideOut);

        SelectObject(memDC, old);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return true;
    }


    static void compositeCursorBgra(int originX, int originY, int width, int height,
        std::vector<uint8_t>& bgra, int stride) {
        // The Viewer renders a predicted low-latency cursor overlay. Baking the
        // endpoint cursor into video creates a second, delayed pointer and makes
        // normal control feel less responsive. Keep composition as an explicit
        // compatibility/debug override only.
        if (!Hi5CompositeCursorEnabled()) return;
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

        const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(height);
        std::memcpy(bits, bgra.data(), std::min(bytes, bgra.size()));

        HGDIOBJ old = SelectObject(memDC, dib);
        DrawIconEx(memDC, drawX, drawY, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        SelectObject(memDC, old);

        std::memcpy(bgra.data(), bits, std::min(bytes, bgra.size()));

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
    std::string currentName;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;

    int width = 0;
    int height = 0;

    I420Frame lastFrame;
    std::vector<uint8_t> bgraScratch;
    bool hasCapturedFrame = false;
    std::chrono::steady_clock::time_point lastGdiRefresh{};
    uint64_t frameId = 0;

    Impl() {
        refreshDisplaysLocked();
        if (targets.empty()) {
            throw std::runtime_error("No displays found for desktop capture");
        }
        initForCurrentLocked();
    }

    void resetD3DLocked() {
        duplication.Reset();
        staging.Reset();
        context.Reset();
        device.Reset();
        width = 0;
        height = 0;
        hasCapturedFrame = false;
        lastFrame = {};
    }

    void refreshDisplaysLocked() {
        const int previousIndex = currentIndex;
        const std::string previousName = currentName;

        targets = enumerateTargets();

        if (targets.empty()) {
            currentIndex = -1;
            currentName.clear();
            resetD3DLocked();
            return;
        }

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
    }

    bool setDisplayIndexLocked(int index) {
        refreshDisplaysLocked();

        if (index == -1) {
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

        currentIndex = it->info.index;
        currentName = it->info.name;
        initForCurrentLocked();
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

    void captureAllMonitorsGdiLocked(FrameCaptureResult& result, bool includeUnchangedFrame) {
        const DisplayInfo d = buildAllMonitorsInfo();
        if (d.width <= 0 || d.height <= 0) {
            const bool cached = !lastFrame.y.empty();
            if (includeUnchangedFrame && cached) result.frame = lastFrame;
            result.hasFrame = includeUnchangedFrame && cached;
            result.frameId = frameId;
            return;
        }

        int stride = 0;
        if (!captureRectBgraGdi(d.x, d.y, d.width, d.height, d.width, d.height, bgraScratch, stride)) {
            const bool cached = !lastFrame.y.empty();
            if (includeUnchangedFrame && cached) result.frame = lastFrame;
            result.hasFrame = includeUnchangedFrame && cached;
            result.frameId = frameId;
            return;
        }

        bgraToI420(bgraScratch.data(), stride, d.width, d.height, result.frame);
        hasCapturedFrame = true;
        if (includeUnchangedFrame) {
            lastFrame = result.frame;
        }
        lastGdiRefresh = std::chrono::steady_clock::now();
        result.hasFrame = true;
        result.changed = true;
        result.frameId = ++frameId;
        return;
    }

    void captureCurrentDisplayGdiLocked(FrameCaptureResult& result, bool includeUnchangedFrame) {
        const DisplayInfo d = currentDisplayInfoLocked();
        if (d.width <= 0 || d.height <= 0) {
            const bool cached = !lastFrame.y.empty();
            if (includeUnchangedFrame && cached) result.frame = lastFrame;
            result.hasFrame = includeUnchangedFrame && cached;
            result.frameId = frameId;
            return;
        }

        int stride = 0;
        if (!captureRectBgraGdi(d.x, d.y, d.width, d.height, d.width, d.height, bgraScratch, stride)) {
            const bool cached = !lastFrame.y.empty();
            if (includeUnchangedFrame && cached) result.frame = lastFrame;
            result.hasFrame = includeUnchangedFrame && cached;
            result.frameId = frameId;
            return;
        }

        bgraToI420(bgraScratch.data(), stride, d.width, d.height, result.frame);
        hasCapturedFrame = true;
        if (includeUnchangedFrame) {
            lastFrame = result.frame;
        }
        lastGdiRefresh = std::chrono::steady_clock::now();
        result.hasFrame = true;
        result.changed = true;
        result.frameId = ++frameId;
        return;
    }

    void captureOneLocked(FrameCaptureResult& result, bool includeUnchangedFrame) {
        // Reset metadata only. Keep result.frame vector capacity so the RemoteHost
        // reuses one Y/U/V allocation set for the lifetime of the capture loop.
        result.hasFrame = false;
        result.changed = false;
        result.cursorOnly = false;
        result.frameId = frameId;
        refreshDisplaysLocked();

        if (currentIndex == -1) {
            captureAllMonitorsGdiLocked(result, includeUnchangedFrame);
            return;
        }

        if (!duplication || !findCurrentTargetLocked()) {
            initForCurrentLocked();
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
            const bool noFrameYet = !hasCapturedFrame;
            if (noFrameYet) {
                captureCurrentDisplayGdiLocked(result, includeUnchangedFrame);
                return;
            }

            if (includeUnchangedFrame) result.frame = lastFrame;
            result.hasFrame = includeUnchangedFrame;
            result.changed = false;
            result.frameId = frameId;
            return;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            initForCurrentLocked();
            if (lastFrame.y.empty() || lastFrame.width <= 0 || lastFrame.height <= 0) {
                captureCurrentDisplayGdiLocked(result, includeUnchangedFrame);
                return;
            }

            if (includeUnchangedFrame) result.frame = lastFrame;
            result.hasFrame = includeUnchangedFrame;
            result.changed = false;
            result.frameId = frameId;
            return;
        }
        if (FAILED(hr)) {
            throw std::runtime_error("AcquireNextFrame failed");
        }

        const bool cursorOnly = frameInfo.TotalMetadataBufferSize == 0 && frameInfo.LastMouseUpdateTime.QuadPart != 0;

        // DXGI reports pointer-only updates separately from desktop pixel damage.
        // The Viewer already renders the remote pointer on its low-latency cursor
        // overlay, so do not copy/map/convert a full desktop frame just because
        // Windows moved the hardware cursor. This keeps mouse movement off the
        // expensive GPU->CPU BGRA->I420->encoder path.
        if (cursorOnly && !Hi5CompositeCursorEnabled()) {
            duplication->ReleaseFrame();
            const bool cached = !lastFrame.y.empty() && lastFrame.width > 0 && lastFrame.height > 0;
            if (includeUnchangedFrame && cached) result.frame = lastFrame;
            result.hasFrame = includeUnchangedFrame && cached;
            result.changed = false;
            result.cursorOnly = true;
            result.frameId = frameId;
            return;
        }

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
        bgraScratch.resize(static_cast<size_t>(rowPitch) * static_cast<size_t>(height));
        std::memcpy(bgraScratch.data(), mapped.pData, bgraScratch.size());
        context->Unmap(staging.Get(), 0);

        const DisplayInfo d = currentDisplayInfoLocked();
        compositeCursorBgra(d.x, d.y, width, height, bgraScratch, rowPitch);

        bgraToI420(bgraScratch.data(), rowPitch, width, height, result.frame);
        hasCapturedFrame = true;

        if (includeUnchangedFrame) {
            lastFrame = result.frame;
        }
        result.hasFrame = true;
        result.changed = true;
        result.cursorOnly = cursorOnly;
        result.frameId = ++frameId;
        return;
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
    FrameCaptureResult r = nextFrameEx(true);
    return r.frame;
}

FrameCaptureResult DesktopFrameSource::nextFrameEx(bool includeUnchangedFrame) {
    FrameCaptureResult result;
    nextFrameExInto(result, includeUnchangedFrame);
    return result;
}

void DesktopFrameSource::nextFrameExInto(FrameCaptureResult& result, bool includeUnchangedFrame) {
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->captureOneLocked(result, includeUnchangedFrame);
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