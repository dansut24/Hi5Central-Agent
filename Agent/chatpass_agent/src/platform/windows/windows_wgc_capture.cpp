#include "windows_wgc_capture.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <roapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <cstring>
#include <cwchar>
#include <sstream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace D3D = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
std::string FormatError(const char* context, HRESULT hr) {
    std::ostringstream out;
    out << context << " hr=0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

// CaptureService is a per-user, demand-start service required by
// Windows.Graphics.Capture. On the test PC it existed but was Stopped; the
// WinRT IsSupported call then returned 0x80070424. Start only the single
// existing per-user instance, never change startup policy, and fall back to
// GDI without entering WinRT if the service cannot become ready.
bool EnsureCaptureServiceReady(std::string& failure) {
    const SC_HANDLE manager = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!manager) {
        failure = FormatError("OpenSCManager", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    // Windows bounds an EnumServicesStatusEx result to 256 KiB. Do not walk
    // the registry template or blindly start a different signed-in user's
    // capture service on a multi-user Windows host.
    std::vector<BYTE> bytes(256u * 1024u);
    DWORD required = 0, count = 0, resume = 0;
    const BOOL listed = EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32, SERVICE_STATE_ALL, bytes.data(),
        static_cast<DWORD>(bytes.size()), &required, &count, &resume, nullptr);
    if (!listed) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        failure = FormatError("EnumServicesStatusEx", HRESULT_FROM_WIN32(code));
        return false;
    }

    const auto* entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(bytes.data());
    std::wstring serviceName;
    DWORD initialState = SERVICE_STOPPED;
    unsigned matches = 0;
    for (DWORD index = 0; index < count; ++index) {
        const wchar_t* name = entries[index].lpServiceName;
        if (!name || std::wcsncmp(name, L"CaptureService_", 15) != 0) continue;
        ++matches;
        serviceName = name;
        initialState = entries[index].ServiceStatusProcess.dwCurrentState;
    }
    if (matches != 1) {
        CloseServiceHandle(manager);
        failure = matches == 0 ? "per-user CaptureService instance missing" :
            "multiple per-user CaptureService instances; refusing ambiguous start";
        return false;
    }
    if (initialState == SERVICE_RUNNING) {
        CloseServiceHandle(manager);
        return true;
    }

    const SC_HANDLE service = OpenServiceW(manager, serviceName.c_str(),
        SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        failure = FormatError("OpenService(CaptureService)", HRESULT_FROM_WIN32(code));
        return false;
    }
    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD code = GetLastError();
        if (code != ERROR_SERVICE_ALREADY_RUNNING) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            failure = FormatError("StartService(CaptureService)", HRESULT_FROM_WIN32(code));
            return false;
        }
    }
    bool ready = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        SERVICE_STATUS_PROCESS status{};
        DWORD size = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status), sizeof(status), &size)) {
            if (status.dwCurrentState == SERVICE_RUNNING) {
                ready = true;
                break;
            }
            if (status.dwCurrentState == SERVICE_STOPPED) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ready) failure = "per-user CaptureService did not reach Running";
    return ready;
}

struct MonitorLookup {
    POINT center{};
    HMONITOR found = nullptr;
};
BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT bounds, LPARAM param) {
    auto& lookup = *reinterpret_cast<MonitorLookup*>(param);
    if (bounds && lookup.center.x >= bounds->left &&
        lookup.center.x < bounds->right &&
        lookup.center.y >= bounds->top &&
        lookup.center.y < bounds->bottom) {
        lookup.found = monitor;
        return FALSE;
    }
    return TRUE;
}
}

struct WgcMonitorCapture::Impl {
    struct ArrivalState {
        std::atomic<uint64_t> events{0};
        std::atomic<int64_t> lastEventNs{0};
        std::atomic<uint64_t> gaps{0};
        std::atomic<uint64_t> totalGapNs{0};
        std::atomic<uint64_t> maxGapNs{0};
    };
    std::shared_ptr<ArrivalState> arrivalState;
    winrt::event_token frameArrivedToken{};
    bool subscribedToFrameArrived = false;
    uint64_t consumedFrames = 0;
    uint64_t eventsAtPreviousFrame = 0;
    int64_t previousSourceTimestamp100ns = 0;
    WgcFrameArrivalDiagnostics Snapshot() const {
        WgcFrameArrivalDiagnostics out{};
        if (arrivalState) {
            out.eventsTotal = arrivalState->events.load(std::memory_order_relaxed);
            out.arrivalGapCount = arrivalState->gaps.load(std::memory_order_relaxed);
            out.arrivalGapTotalNs = arrivalState->totalGapNs.load(std::memory_order_relaxed);
            out.arrivalGapMaxNs = arrivalState->maxGapNs.load(std::memory_order_relaxed);
        }
        out.framesConsumedTotal = consumedFrames;
        return out;
    }
    bool apartmentInitialized = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    int stagingWidth = 0;
    int stagingHeight = 0;
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession session{nullptr};

    void Stop() {
        // Close capture objects before releasing D3D and the WinRT apartment.
        if (session) { session.Close(); session = nullptr; }
        if (pool) {
            if (subscribedToFrameArrived) {
                pool.FrameArrived(frameArrivedToken);
                subscribedToFrameArrived = false;
            }
            pool.Close();
            pool = nullptr;
        }
        arrivalState.reset();
        consumedFrames = 0;
        eventsAtPreviousFrame = 0;
        previousSourceTimestamp100ns = 0;
        item = nullptr;
        staging.Reset();
        context.Reset();
        device.Reset();
        stagingWidth = stagingHeight = 0;
        if (apartmentInitialized) {
            RoUninitialize();
            apartmentInitialized = false;
        }
    }

    bool Start(int x, int y, int w, int h, std::string& failure) {
        Stop();
        failure.clear();
        if (w <= 0 || h <= 0) {
            failure = "invalid selected monitor geometry";
            return false;
        }
        std::string stage = "CaptureService readiness";
        try {
            // Avoid entering WinRT capture activation when the per-user
            // CaptureService cannot be started. Normal GDI remains available.
            if (!EnsureCaptureServiceReady(failure)) return false;
            stage = "RoInitialize";
            const HRESULT initHr = RoInitialize(RO_INIT_MULTITHREADED);
            if (SUCCEEDED(initHr)) apartmentInitialized = true;
            else if (initHr != RPC_E_CHANGED_MODE) {
                throw std::runtime_error(FormatError("RoInitialize", initHr));
            }

            // Agent 1.0.205 WER / matching PDB showed a repeatable
            // 0xC0000005 at GraphicsCaptureSession::IsSupported()'s
            // virtual interface call (RVA 0x319473). The capture service
            // may exist yet the WinRT support-probe interface be invalid.
            // Do not invoke that crashing preflight. This backend is
            // already opt-in and normal-desktop-only. CreateForMonitor()
            // and later CreateCaptureSession()/StartCapture() each validate
            // support via HRESULT; errors are caught below and return to
            // GDI without replacing the active video stream.
            // Keep the operating-system privacy border unchanged.
            stage = "GraphicsCaptureItem activation";

            MonitorLookup lookup{};
            lookup.center = { x + w / 2, y + h / 2 };
            EnumDisplayMonitors(nullptr, nullptr, FindMonitor,
                                reinterpret_cast<LPARAM>(&lookup));
            if (!lookup.found) {
                throw std::runtime_error("no HMONITOR for selected output");
            }

            stage = "GraphicsCaptureItem activation";
            auto activation = winrt::get_activation_factory<WGC::GraphicsCaptureItem>();
            auto interop = activation.as<IGraphicsCaptureItemInterop>();
            stage = "GraphicsCaptureItem.CreateForMonitor";
            winrt::check_hresult(interop->CreateForMonitor(
                lookup.found,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(item)));

            stage = "D3D11CreateDevice";
            const HRESULT deviceHr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION,
                &device, nullptr, &context);
            if (FAILED(deviceHr)) {
                throw std::runtime_error(FormatError("D3D11CreateDevice(WGC)", deviceHr));
            }

            ComPtr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(device.As(&dxgiDevice));
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.Get(), inspectable.put()));
            auto winrtDevice = inspectable.as<D3D::IDirect3DDevice>();
            const auto size = item.Size();
            if (size.Width <= 0 || size.Height <= 0) {
                throw std::runtime_error("WGC monitor has invalid size");
            }
            stage = "Direct3D11CaptureFramePool.CreateFreeThreaded";
            pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2, size);
            // Subscribe after pool construction, before StartCapture.
            // The callback must NEVER drain frames or synchronise with
            // GPU work; it only counts arrivals and their timing.
            arrivalState = std::make_shared<ArrivalState>();
            frameArrivedToken = pool.FrameArrived(
                [state = arrivalState](
                    WGC::Direct3D11CaptureFramePool const&,
                    winrt::Windows::Foundation::IInspectable const&) noexcept {
                    const auto now = std::chrono::steady_clock::now();
                    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now.time_since_epoch()).count();
                    const int64_t prev = state->lastEventNs.exchange(ns, std::memory_order_relaxed);
                    if (prev > 0 && ns > prev) {
                        const uint64_t gap = static_cast<uint64_t>(ns - prev);
                        state->totalGapNs.fetch_add(gap, std::memory_order_relaxed);
                        state->gaps.fetch_add(1, std::memory_order_relaxed);
                        uint64_t oldMax = state->maxGapNs.load(std::memory_order_relaxed);
                        while (gap > oldMax && !state->maxGapNs.compare_exchange_weak(
                            oldMax, gap, std::memory_order_relaxed)) {}
                    }
                    state->events.fetch_add(1, std::memory_order_release);
                });
            subscribedToFrameArrived = true;
            stage = "CreateCaptureSession";
            session = pool.CreateCaptureSession(item);
            // Do not request borderless capture; the operating system's
            // privacy/consent indicator must remain visible.
            stage = "GraphicsCaptureSession.StartCapture";
            session.StartCapture();
            return true;
        } catch (const winrt::hresult_error& ex) {
            failure = FormatError(stage.c_str(), ex.code());
        } catch (const std::exception& ex) {
            failure = stage + ": " + ex.what();
        }
        Stop();
        return false;
    }

    bool TryReadBgra(std::vector<uint8_t>& bgra, int& width, int& height,
                     int& stride, std::string& failure,
                     WgcReadbackTimings* timings,
                     WgcFrameArrivalDiagnostics* frameDiagnostics) {
        failure.clear();
        if (timings) *timings = {};
        if (frameDiagnostics) *frameDiagnostics = Snapshot();
        if (!session || !pool || !device) return false;
        try {
            auto frame = pool.TryGetNextFrame();
            if (!frame) return false;
            const int64_t frameTimestamp = frame.SystemRelativeTime().count();
            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0) {
                throw std::runtime_error("WGC frame has invalid dimensions");
            }

            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(texture.GetAddressOf())));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            width = std::min(static_cast<int>(desc.Width), size.Width);
            height = std::min(static_cast<int>(desc.Height), size.Height);
            if (width <= 0 || height <= 0 || width > 7680 || height > 4320 ||
                desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                throw std::runtime_error("WGC unexpected texture format or size");
            }
            if (!staging || stagingWidth != static_cast<int>(desc.Width) ||
                stagingHeight != static_cast<int>(desc.Height)) {
                staging.Reset();
                D3D11_TEXTURE2D_DESC cpuDesc = desc;
                cpuDesc.Usage = D3D11_USAGE_STAGING;
                cpuDesc.BindFlags = 0;
                cpuDesc.MiscFlags = 0;
                cpuDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                winrt::check_hresult(device->CreateTexture2D(
                    &cpuDesc, nullptr, &staging));
                stagingWidth = static_cast<int>(desc.Width);
                stagingHeight = static_cast<int>(desc.Height);
            }
            const auto gpuCopyStarted = std::chrono::steady_clock::now();
            context->CopyResource(staging.Get(), texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(
                staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            const double gpuCopyMapMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - gpuCopyStarted).count();
            // Validate GPU readback stride before copying or allocating.
            // A corrupt or unexpectedly large RowPitch must never overwrite
            // the frame buffer or bring down the remote-session helper.
            const size_t pitch = static_cast<size_t>(mapped.RowPitch);
            const size_t minimumPitch = static_cast<size_t>(width) * 4u;
            const size_t byteCount = pitch * static_cast<size_t>(height);
            if (!mapped.pData || pitch < minimumPitch ||
                byteCount > 256u * 1024u * 1024u) {
                context->Unmap(staging.Get(), 0);
                throw std::runtime_error("WGC mapped texture invalid stride or size");
            }
            stride = static_cast<int>(mapped.RowPitch);
            const auto cpuCopyStarted = std::chrono::steady_clock::now();
            // Caller owns a persistent BGRA buffer; resize reuses capacity
            // on every same-resolution frame instead of reallocating ~16MB.
            bgra.resize(byteCount);
            std::memcpy(bgra.data(), mapped.pData, byteCount);
            context->Unmap(staging.Get(), 0);
            if (timings) {
                timings->gpuCopyMapMs = gpuCopyMapMs;
                timings->cpuBufferCopyMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - cpuCopyStarted).count();
            }
            ++consumedFrames;
            if (frameDiagnostics) {
                *frameDiagnostics = Snapshot();
                frameDiagnostics->sourceTimestamp100ns = frameTimestamp;
                frameDiagnostics->sourceTimestampGap100ns =
                    (previousSourceTimestamp100ns > 0 && frameTimestamp > previousSourceTimestamp100ns)
                        ? frameTimestamp - previousSourceTimestamp100ns : 0;
                frameDiagnostics->eventsSincePreviousFrame =
                    (frameDiagnostics->eventsTotal >= eventsAtPreviousFrame)
                    ? frameDiagnostics->eventsTotal - eventsAtPreviousFrame : 0;
            }
            // Keep frame timestamps from the compositor rather than local
            // consumption time; latter can hide frame-pool coalescing.
            if (arrivalState) eventsAtPreviousFrame =
                arrivalState->events.load(std::memory_order_relaxed);
            previousSourceTimestamp100ns = frameTimestamp;
            return true;
        } catch (const winrt::hresult_error& ex) {
            failure = FormatError("WGC frame read", ex.code());
        } catch (const std::exception& ex) {
            failure = ex.what();
        }
        return false;
    }
};

WgcMonitorCapture::WgcMonitorCapture() : impl_(std::make_unique<Impl>()) {}
WgcMonitorCapture::~WgcMonitorCapture() { Stop(); }
bool WgcMonitorCapture::Start(int x, int y, int w, int h, std::string& failure) {
    return impl_->Start(x, y, w, h, failure);
}
bool WgcMonitorCapture::TryReadBgra(
    std::vector<uint8_t>& bgra, int& width, int& height, int& stride,
    std::string& failure, WgcReadbackTimings* timings,
    WgcFrameArrivalDiagnostics* frameDiagnostics) {
    return impl_->TryReadBgra(bgra, width, height, stride, failure, timings, frameDiagnostics);
}
void WgcMonitorCapture::Stop() { if (impl_) impl_->Stop(); }
bool WgcMonitorCapture::IsRunning() const {
    return impl_ && static_cast<bool>(impl_->session);
}
